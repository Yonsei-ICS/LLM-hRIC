#!/usr/bin/env python3
"""Lightweight local YOLO detector for the UE/Robot side.

Runs YOLOv8n (3.2M params) for coarse object detection.  Detectable classes
are configured dynamically by the server's task_config message.
"""
from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Any, Optional

import cv2
import numpy as np

LOG = logging.getLogger("yolo_detector")


@dataclass
class Detection:
    class_name: str
    class_id: int
    confidence: float
    bbox: list[int]  # [x1, y1, x2, y2] in pixels


@dataclass
class DetectionResult:
    detections: list[Detection] = field(default_factory=list)
    infer_ms: float = 0.0
    frame_w: int = 0
    frame_h: int = 0


class LocalYOLO:
    """Lazy-loaded YOLO detector that filters by configurable class names."""

    def __init__(
        self,
        weights: str = "yolov8n.pt",
        device: str = "cpu",
        conf: float = 0.3,
        iou: float = 0.45,
    ) -> None:
        self._weights = weights
        self._device = device
        self._conf = conf
        self._iou = iou
        self._model: Any = None
        self._target_classes: set[str] = set()
        self._target_class_ids: set[int] = set()

    def _ensure_model(self) -> None:
        if self._model is not None:
            return
        try:
            from ultralytics import YOLO
            self._model = YOLO(self._weights)
            LOG.info("YOLO model loaded: %s (device=%s)", self._weights, self._device)
        except Exception as e:
            LOG.error("Failed to load YOLO: %s", e)
            raise

    def set_target_classes(self, class_names: list[str]) -> None:
        """Update which COCO classes to report.  Called when server sends task_config."""
        self._ensure_model()
        self._target_classes = {c.lower() for c in class_names}
        self._target_class_ids = set()
        if self._model is not None and hasattr(self._model, "names"):
            for cid, name in self._model.names.items():
                if name.lower() in self._target_classes:
                    self._target_class_ids.add(cid)
        LOG.info("Target classes set: %s → IDs %s", self._target_classes, self._target_class_ids)

    def detect(self, frame_bgr: np.ndarray) -> DetectionResult:
        """Run YOLO on a frame, returning only target-class detections."""
        self._ensure_model()
        h, w = frame_bgr.shape[:2]
        t0 = time.monotonic()

        try:
            results = self._model.predict(
                frame_bgr,
                conf=self._conf,
                iou=self._iou,
                device=self._device,
                verbose=False,
            )
        except Exception as e:
            LOG.error("YOLO inference failed: %s", e)
            return DetectionResult(frame_w=w, frame_h=h)

        infer_ms = (time.monotonic() - t0) * 1000.0

        dets: list[Detection] = []
        if results and len(results) > 0:
            boxes = results[0].boxes
            if boxes is not None:
                for i in range(len(boxes)):
                    cls_id = int(boxes.cls[i].item())
                    if self._target_class_ids and cls_id not in self._target_class_ids:
                        continue
                    conf = float(boxes.conf[i].item())
                    x1, y1, x2, y2 = boxes.xyxy[i].tolist()
                    cls_name = self._model.names.get(cls_id, str(cls_id))
                    dets.append(Detection(
                        class_name=cls_name,
                        class_id=cls_id,
                        confidence=round(conf, 3),
                        bbox=[int(x1), int(y1), int(x2), int(y2)],
                    ))

        return DetectionResult(
            detections=dets,
            infer_ms=round(infer_ms, 1),
            frame_w=w,
            frame_h=h,
        )


def crop_detection(
    frame_bgr: np.ndarray,
    bbox: list[int],
    padding: float = 1.3,
) -> np.ndarray:
    """Crop and pad a detection bbox from the frame."""
    h, w = frame_bgr.shape[:2]
    x1, y1, x2, y2 = bbox
    bw, bh = x2 - x1, y2 - y1
    cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
    half_w = int(bw * padding / 2)
    half_h = int(bh * padding / 2)
    cx1 = max(0, cx - half_w)
    cy1 = max(0, cy - half_h)
    cx2 = min(w, cx + half_w)
    cy2 = min(h, cy + half_h)
    return frame_bgr[cy1:cy2, cx1:cx2].copy()
