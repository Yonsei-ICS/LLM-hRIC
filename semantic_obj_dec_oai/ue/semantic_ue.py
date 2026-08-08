#!/usr/bin/env python3
"""Semantic UE Client — runs on the OAI nrUE machine (robot side).

Workflow:
1. Start camera capture at maximum resolution.
2. Connect to semantic_server via TCP (through OAI 5G data plane).
3. Receive task_config from server (which classes to detect).
4. Run local YOLOv8n on each frame.
5. On detection → crop target region → send report + base64 crop to server.
6. If server requests high-res → send full frame at capture resolution.
7. No detection → send periodic heartbeats (low bandwidth).
"""
from __future__ import annotations

import argparse
import base64
import json
import logging
import os
import signal
import socket
import subprocess
import threading
import time
from collections import deque
from typing import Any, Optional

import cv2
import numpy as np

from yolo_detector import LocalYOLO, Detection, DetectionResult, crop_detection

LOG = logging.getLogger("semantic_ue")


# ─────────────────────────────────────────
# Camera capture thread
# ─────────────────────────────────────────
class CameraCapture(threading.Thread):
    """Continuously captures frames at fixed (max) resolution."""

    def __init__(self, device: int, width: int, height: int) -> None:
        super().__init__(daemon=True)
        self.device = device
        self.width = width
        self.height = height
        self._stop_event = threading.Event()
        self._lock = threading.Lock()
        self._latest: Optional[np.ndarray] = None
        self._frame_count = 0

    def stop(self) -> None:
        self._stop_event.set()

    def get_latest(self) -> Optional[np.ndarray]:
        with self._lock:
            return self._latest

    @property
    def frame_count(self) -> int:
        return self._frame_count

    def run(self) -> None:
        cap = cv2.VideoCapture(self.device)
        if not cap.isOpened():
            LOG.error("Cannot open camera device %d", self.device)
            return
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, float(self.width))
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, float(self.height))
        aw = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        ah = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        LOG.info("Camera started: device=%d capture=%dx%d (actual %dx%d)",
                 self.device, self.width, self.height, aw, ah)

        while not self._stop_event.is_set():
            ok, frame = cap.read()
            if not ok or frame is None:
                time.sleep(0.02)
                continue
            with self._lock:
                self._latest = frame
            self._frame_count += 1

        cap.release()
        LOG.info("Camera stopped")


# ─────────────────────────────────────────
# Server communication
# ─────────────────────────────────────────
class ServerConnection:
    """Manages TCP connection to semantic_server."""

    def __init__(self, host: str, port: int, src_ip: str = "") -> None:
        self.host = host
        self.port = port
        self.src_ip = src_ip
        self._sock: Optional[socket.socket] = None
        self._buf = b""
        self._lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self) -> bool:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(15.0)
            if self.src_ip:
                s.bind((self.src_ip, 0))
            s.connect((self.host, self.port))
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(5.0)
            self._sock = s
            LOG.info("Connected to server %s:%d", self.host, self.port)
            return True
        except OSError as e:
            LOG.warning("Connect failed: %s", e)
            return False

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def send(self, obj: dict) -> bool:
        if not self._sock:
            return False
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        with self._lock:
            try:
                self._sock.sendall(data)
                return True
            except OSError as e:
                LOG.warning("Send failed: %s", e)
                self.close()
                return False

    def recv_messages(self) -> list[dict]:
        """Non-blocking receive; returns list of parsed JSON messages."""
        if not self._sock:
            return []
        msgs: list[dict] = []
        try:
            chunk = self._sock.recv(1048576)
            if not chunk:
                self.close()
                return []
            self._buf += chunk
        except socket.timeout:
            pass
        except OSError:
            self.close()
            return []

        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            try:
                msgs.append(json.loads(line.decode("utf-8")))
            except json.JSONDecodeError:
                pass
        return msgs


# ─────────────────────────────────────────
# Auto-detect oaitun_ue1 IP
# ─────────────────────────────────────────
def auto_detect_ue_ip() -> str:
    try:
        out = subprocess.check_output(
            ["ip", "-j", "addr", "show", "dev", "oaitun_ue1"],
            stderr=subprocess.DEVNULL, text=True,
        )
        data = json.loads(out)
        if data and "addr_info" in data[0]:
            for ai in data[0]["addr_info"]:
                if ai.get("family") == "inet" and ai.get("local"):
                    return str(ai["local"])
    except Exception:
        pass
    return ""


# ─────────────────────────────────────────
# Main loop
# ─────────────────────────────────────────
def main() -> None:
    p = argparse.ArgumentParser(description="Semantic UE: local YOLO + server VLM verification")
    p.add_argument("--server", required=True, help="Server address host:port (e.g. 10.0.0.1:9770)")
    p.add_argument("--camera", type=int, default=0)
    p.add_argument("--capture-width", type=int, default=1280)
    p.add_argument("--capture-height", type=int, default=720)
    p.add_argument("--detect-fps", type=float, default=5.0,
                    help="Detection framerate (how many frames/sec to run YOLO on)")
    p.add_argument("--weights", default="yolov8n.pt")
    p.add_argument("--device", default="cpu", help="YOLO device: cpu or 0 (GPU)")
    p.add_argument("--jpeg-quality", type=int, default=85)
    p.add_argument("--src-ip", default="",
                    help="Source IP for TCP (auto-detects oaitun_ue1 if empty)")
    p.add_argument("--heartbeat-interval", type=float, default=5.0)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
    )

    stop = threading.Event()

    def _sig(_a: Any, _b: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    src_ip = args.src_ip or auto_detect_ue_ip()
    if src_ip:
        LOG.info("Using source IP: %s", src_ip)

    srv_host, srv_port_s = args.server.rsplit(":", 1)
    srv_port = int(srv_port_s)

    cam = CameraCapture(args.camera, args.capture_width, args.capture_height)
    cam.start()

    yolo = LocalYOLO(weights=args.weights, device=args.device)

    conn = ServerConnection(srv_host, srv_port, src_ip)

    detect_period = 1.0 / max(0.1, args.detect_fps)
    frame_id = 0
    last_heartbeat = 0.0
    task_configured = False
    crop_padding = 1.3
    pending_highres_frame: Optional[int] = None

    LOG.info("Semantic UE starting (detect_fps=%.1f, server=%s:%d)",
             args.detect_fps, srv_host, srv_port)

    try:
        while not stop.is_set():
            if not conn.connected:
                if conn.connect():
                    conn.send({"type": "hello", "ue_id": src_ip or "unknown"})
                    task_configured = False
                else:
                    time.sleep(2.0)
                    continue

            for msg in conn.recv_messages():
                msg_type = msg.get("type", "")
                if msg_type == "task_config":
                    classes = msg.get("detect_classes", [])
                    min_conf = msg.get("min_confidence", 0.3)
                    crop_padding = msg.get("crop_padding", 1.3)
                    yolo._conf = min_conf
                    yolo.set_target_classes(classes)
                    task_configured = True
                    LOG.info("Task configured: classes=%s conf=%.2f", classes, min_conf)

                elif msg_type == "request_highres":
                    req_fid = msg.get("frame_id", 0)
                    LOG.info("Server requests high-res for frame %d", req_fid)
                    pending_highres_frame = req_fid

                elif msg_type == "verified":
                    match = msg.get("match", False)
                    fid = msg.get("frame_id", 0)
                    LOG.info("Verification result frame %d: match=%s", fid, match)
                    if match:
                        LOG.info("*** TARGET CONFIRMED ***")

                elif msg_type == "ack":
                    pass

            if pending_highres_frame is not None:
                frame = cam.get_latest()
                if frame is not None:
                    ok, enc = cv2.imencode(
                        ".jpg", frame,
                        [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_quality],
                    )
                    if ok:
                        conn.send({
                            "type": "highres_frame",
                            "ue_id": src_ip or "unknown",
                            "frame_id": pending_highres_frame,
                            "image_b64": base64.b64encode(enc.tobytes()).decode(),
                        })
                        LOG.info("Sent high-res frame %d (%dx%d)",
                                 pending_highres_frame, frame.shape[1], frame.shape[0])
                    pending_highres_frame = None

            if not task_configured:
                time.sleep(0.5)
                continue

            frame = cam.get_latest()
            if frame is None:
                time.sleep(0.02)
                continue

            result = yolo.detect(frame)
            frame_id += 1

            if result.detections:
                best = max(result.detections, key=lambda d: d.confidence)
                crop_bgr = crop_detection(frame, best.bbox, padding=crop_padding)
                ok, enc = cv2.imencode(
                    ".jpg", crop_bgr,
                    [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_quality],
                )
                crop_b64 = base64.b64encode(enc.tobytes()).decode() if ok else ""

                report = {
                    "type": "report",
                    "ue_id": src_ip or "unknown",
                    "frame_id": frame_id,
                    "detections": [
                        {
                            "class": d.class_name,
                            "conf": d.confidence,
                            "bbox": d.bbox,
                        }
                        for d in result.detections
                    ],
                    "crop_b64": crop_b64,
                    "infer_ms": result.infer_ms,
                }
                conn.send(report)
                LOG.info("Frame %d: %d det(s) [best=%s %.2f] infer=%.0fms crop=%dB",
                         frame_id, len(result.detections),
                         best.class_name, best.confidence,
                         result.infer_ms, len(crop_b64))
            else:
                now = time.time()
                if now - last_heartbeat > args.heartbeat_interval:
                    conn.send({
                        "type": "heartbeat",
                        "ue_id": src_ip or "unknown",
                    })
                    last_heartbeat = now

            time.sleep(detect_period)

    finally:
        cam.stop()
        cam.join(timeout=3.0)
        conn.close()

    LOG.info("Semantic UE exited.")


if __name__ == "__main__":
    main()
