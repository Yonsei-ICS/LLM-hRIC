#!/usr/bin/env python3
"""Semantic Object Detection Server — runs inside oai-ext-dn container.

Responsibilities:
1. Accept TCP connections from UE(s).
2. Push task configuration (which COCO classes to detect locally).
3. Receive detection reports (metadata + cropped JPEG) from UE.
4. Run VLM verification on crops for fine-grained attribute checking.
5. If confidence is uncertain, request a high-res frame from UE.
6. Write shared state for xApp PRB allocator.

Protocol (JSON lines over TCP, newline-terminated):
  UE → Server:
    {"type":"hello",   "ue_id":"10.0.0.7"}
    {"type":"report",  "ue_id":"...", "frame_id":N,
     "detections":[{"class":"sports ball","conf":0.82,"bbox":[x1,y1,x2,y2]}],
     "crop_b64":"<base64 JPEG>"}
    {"type":"highres_frame", "ue_id":"...", "frame_id":N, "image_b64":"<base64>"}
    {"type":"heartbeat", "ue_id":"..."}

  Server → UE:
    {"type":"task_config", "detect_classes":["sports ball"], "min_confidence":0.3,
     "crop_padding":1.3}
    {"type":"request_highres", "frame_id":N}
    {"type":"verified", "frame_id":N, "match":true/false,
     "results":[{"attribute":"color","confidence":0.9}, ...]}
    {"type":"ack"}
"""
from __future__ import annotations

import argparse
import base64
import json
import logging
import os
import signal
import socket
import threading
import time
from typing import Any, Optional

import cv2
import numpy as np

from task_decomposer import DecomposedTask, decompose_task
from vlm_verifier import VLMVerifier, VerificationResult

LOG = logging.getLogger("semantic_server")

SHARED_STATE_PATH = "/tmp/semantic_detection_state.json"
_state_lock = threading.Lock()


# ─────────────────────────────────────────
# Shared state for xApp
# ─────────────────────────────────────────
def _write_shared_state(
    ue_id: str,
    status: str,
    verification_results: list[dict[str, Any]] | None = None,
    match: bool = False,
    need_highres: bool = False,
) -> None:
    """Write per-UE state for the xApp PRB allocator."""
    with _state_lock:
        try:
            with open(SHARED_STATE_PATH, "r") as f:
                state = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            state = {}
        state[ue_id] = {
            "status": status,
            "match": match,
            "need_highres": need_highres,
            "verification": verification_results or [],
            "updated_at": time.time(),
        }
        tmp = SHARED_STATE_PATH + ".tmp"
        with open(tmp, "w") as f:
            json.dump(state, f)
        os.replace(tmp, SHARED_STATE_PATH)


# ─────────────────────────────────────────
# UE connection handler
# ─────────────────────────────────────────
class UEHandler:
    """Handles one TCP connection from a UE."""

    def __init__(
        self,
        conn: socket.socket,
        addr: Any,
        task: DecomposedTask,
        verifier: VLMVerifier,
    ) -> None:
        self.conn = conn
        self.addr = addr
        self.task = task
        self.verifier = verifier
        self.ue_id: str = str(addr[0])
        self._pending_highres: dict[int, list[dict]] = {}

    def run(self) -> None:
        LOG.info("UE connected from %s", self.addr)
        buf = b""
        try:
            with self.conn:
                self.conn.settimeout(300.0)
                self._send_task_config()
                while True:
                    chunk = self.conn.recv(1048576)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            msg = json.loads(line.decode("utf-8"))
                        except json.JSONDecodeError as e:
                            LOG.warning("Bad JSON from %s: %s", self.ue_id, e)
                            continue
                        self._handle_message(msg)
        except (ConnectionResetError, BrokenPipeError, socket.timeout, OSError) as e:
            LOG.info("UE %s disconnected: %s", self.ue_id, e)
        finally:
            _write_shared_state(self.ue_id, "offline")
            LOG.info("UE %s handler exiting", self.ue_id)

    def _send(self, obj: dict) -> None:
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        try:
            self.conn.sendall(data)
        except OSError as e:
            LOG.warning("Send to %s failed: %s", self.ue_id, e)

    def _send_task_config(self) -> None:
        rt = self.task.robot_task
        self._send({
            "type": "task_config",
            "detect_classes": rt.detect_classes,
            "min_confidence": rt.min_confidence,
            "crop_padding": rt.crop_padding,
            "report_mode": rt.report_mode,
        })
        LOG.info("Sent task_config to %s: classes=%s conf=%.2f",
                 self.ue_id, rt.detect_classes, rt.min_confidence)

    def _handle_message(self, msg: dict) -> None:
        msg_type = msg.get("type", "")
        if msg_type == "hello":
            self.ue_id = msg.get("ue_id", self.ue_id)
            _write_shared_state(self.ue_id, "idle")
            self._send({"type": "ack", "ue_id": self.ue_id})
            LOG.info("UE hello: %s", self.ue_id)

        elif msg_type == "report":
            self._handle_report(msg)

        elif msg_type == "highres_frame":
            self._handle_highres(msg)

        elif msg_type == "heartbeat":
            _write_shared_state(self.ue_id, "idle")
            self._send({"type": "ack"})

        else:
            LOG.debug("Unknown message type from %s: %s", self.ue_id, msg_type)

    def _handle_report(self, msg: dict) -> None:
        """Process a coarse detection report from the UE."""
        frame_id = msg.get("frame_id", 0)
        detections = msg.get("detections", [])
        crop_b64 = msg.get("crop_b64", "")

        if not detections:
            _write_shared_state(self.ue_id, "idle")
            return

        _write_shared_state(self.ue_id, "verifying")

        if not crop_b64:
            LOG.warning("Report from %s frame %d has no crop", self.ue_id, frame_id)
            _write_shared_state(self.ue_id, "idle")
            return

        crop_jpg = base64.b64decode(crop_b64)
        crop_bgr = cv2.imdecode(
            np.frombuffer(crop_jpg, dtype=np.uint8), cv2.IMREAD_COLOR
        )
        if crop_bgr is None:
            LOG.warning("Failed to decode crop from %s", self.ue_id)
            return

        LOG.info("UE %s frame %d: %d detection(s), running VLM verification",
                 self.ue_id, frame_id, len(detections))

        results = self._run_verification(crop_jpg, crop_bgr)

        all_pass, any_uncertain = self._evaluate_results(results)

        if any_uncertain and self.task.server_task.require_highres_if_uncertain:
            LOG.info("UE %s frame %d: uncertain, requesting high-res", self.ue_id, frame_id)
            self._pending_highres[frame_id] = [
                {"attribute": r.attribute, "prompt": r.prompt}
                for r in results if self._is_uncertain(r.confidence)
            ]
            _write_shared_state(
                self.ue_id, "requesting_highres",
                need_highres=True,
                verification_results=[self._result_to_dict(r) for r in results],
            )
            self._send({"type": "request_highres", "frame_id": frame_id})
            return

        _write_shared_state(
            self.ue_id, "verified",
            match=all_pass,
            verification_results=[self._result_to_dict(r) for r in results],
        )
        self._send({
            "type": "verified",
            "frame_id": frame_id,
            "match": all_pass,
            "results": [self._result_to_dict(r) for r in results],
        })
        LOG.info("UE %s frame %d: match=%s", self.ue_id, frame_id, all_pass)

    def _handle_highres(self, msg: dict) -> None:
        """Process a high-resolution frame uploaded by UE after request."""
        frame_id = msg.get("frame_id", 0)
        image_b64 = msg.get("image_b64", "")

        pending = self._pending_highres.pop(frame_id, None)

        if not image_b64:
            LOG.warning("Empty high-res from %s frame %d", self.ue_id, frame_id)
            _write_shared_state(self.ue_id, "idle")
            return

        img_jpg = base64.b64decode(image_b64)
        img_bgr = cv2.imdecode(
            np.frombuffer(img_jpg, dtype=np.uint8), cv2.IMREAD_COLOR
        )
        if img_bgr is None:
            LOG.warning("Failed to decode high-res from %s", self.ue_id)
            _write_shared_state(self.ue_id, "idle")
            return

        LOG.info("UE %s frame %d: high-res received (%dx%d), re-verifying",
                 self.ue_id, frame_id, img_bgr.shape[1], img_bgr.shape[0])

        results = self._run_verification(img_jpg, img_bgr)
        all_pass, _ = self._evaluate_results(results)

        _write_shared_state(
            self.ue_id, "verified",
            match=all_pass,
            need_highres=False,
            verification_results=[self._result_to_dict(r) for r in results],
        )
        self._send({
            "type": "verified",
            "frame_id": frame_id,
            "match": all_pass,
            "results": [self._result_to_dict(r) for r in results],
        })
        LOG.info("UE %s frame %d (high-res): match=%s", self.ue_id, frame_id, all_pass)

    def _run_verification(
        self, crop_jpg: bytes, crop_bgr: np.ndarray,
    ) -> list[VerificationResult]:
        results = []
        for vp in self.task.server_task.verification_prompts:
            r = self.verifier.verify(crop_jpg, crop_bgr, vp.attribute, vp.prompt)
            results.append(r)
            LOG.debug("  verify '%s': conf=%.2f backend=%s", vp.attribute, r.confidence, r.backend)
        return results

    def _evaluate_results(
        self, results: list[VerificationResult],
    ) -> tuple[bool, bool]:
        all_pass = True
        any_uncertain = False
        lo, hi = self.task.server_task.uncertainty_range
        for r in results:
            vp = next(
                (p for p in self.task.server_task.verification_prompts
                 if p.attribute == r.attribute),
                None,
            )
            threshold = vp.threshold if vp else 0.7
            if r.confidence < threshold:
                all_pass = False
            if lo <= r.confidence <= hi:
                any_uncertain = True
        return all_pass, any_uncertain

    def _is_uncertain(self, conf: float) -> bool:
        lo, hi = self.task.server_task.uncertainty_range
        return lo <= conf <= hi

    @staticmethod
    def _result_to_dict(r: VerificationResult) -> dict[str, Any]:
        return {
            "attribute": r.attribute,
            "confidence": round(r.confidence, 3),
            "backend": r.backend,
            "raw_answer": r.raw_answer[:200],
        }


# ─────────────────────────────────────────
# TCP Server
# ─────────────────────────────────────────
def run_server(
    host: str,
    port: int,
    task: DecomposedTask,
    verifier: VLMVerifier,
    stop: threading.Event,
) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(8)
    srv.settimeout(2.0)
    LOG.info("Semantic server listening on %s:%d", host, port)

    threads: list[threading.Thread] = []
    try:
        while not stop.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            handler = UEHandler(conn, addr, task, verifier)
            t = threading.Thread(target=handler.run, daemon=True)
            t.start()
            threads.append(t)
    finally:
        srv.close()
        for t in threads:
            t.join(timeout=3.0)


# ─────────────────────────────────────────
# Main
# ─────────────────────────────────────────
def main() -> None:
    global SHARED_STATE_PATH

    p = argparse.ArgumentParser(
        description="Semantic Object Detection Server (runs in oai-ext-dn)"
    )
    p.add_argument("--listen", default="0.0.0.0:9770",
                    help="bind address:port for UE connections")
    p.add_argument("--task", required=True,
                    help="Natural-language task description, e.g. 'Find the ball with blue color and scratches'")
    p.add_argument("--api-key", default=os.environ.get("GEMINI_API_KEY", ""),
                    help="API key for LLM/VLM (Gemini or OpenAI compatible)")
    p.add_argument("--model", default="gemini-2.5-flash",
                    help="Model name (e.g. gemini-2.5-flash, gpt-4o-mini)")
    p.add_argument("--api-base-url",
                    default="https://generativelanguage.googleapis.com/v1beta/openai/",
                    help="API base URL (Gemini or OpenAI compatible endpoint)")
    p.add_argument("--ollama-model", default="llava",
                    help="Ollama VLM model name")
    p.add_argument("--ollama-url", default="http://localhost:11434")
    p.add_argument("--shared-state-path", default=SHARED_STATE_PATH)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
    )

    SHARED_STATE_PATH = args.shared_state_path

    host, port_s = args.listen.rsplit(":", 1)
    port = int(port_s)

    LOG.info("Decomposing task: %s", args.task)
    task = decompose_task(
        args.task,
        api_key=args.api_key,
        model=args.model,
        base_url=args.api_base_url,
    )
    LOG.info("Robot classes: %s", task.robot_task.detect_classes)
    LOG.info("Verification prompts: %d", len(task.server_task.verification_prompts))
    for vp in task.server_task.verification_prompts:
        LOG.info("  [%s] %s (threshold=%.2f)", vp.attribute, vp.prompt, vp.threshold)

    verifier = VLMVerifier(
        openai_api_key=args.api_key,
        openai_model=args.model,
        openai_base_url=args.api_base_url,
        ollama_model=args.ollama_model,
        ollama_url=args.ollama_url,
    )

    stop = threading.Event()

    def _sig(_a: Any, _b: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    run_server(host, port, task, verifier, stop)
    LOG.info("Server exited.")


if __name__ == "__main__":
    main()
