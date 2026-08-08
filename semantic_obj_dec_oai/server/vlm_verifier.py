#!/usr/bin/env python3
"""VLM (Vision-Language Model) verifier for fine-grained attribute checking.

Supports three backends (tried in order):
1. OpenAI-compatible API  (GPT-4o / GPT-4o-mini with vision)
2. Ollama local server    (llava, bakllava, etc.)
3. CV heuristic fallback  (color histogram + edge density — no LLM needed)
"""
from __future__ import annotations

import base64
import logging
import re
from dataclasses import dataclass
from typing import Any, Optional

import cv2
import numpy as np

LOG = logging.getLogger("vlm_verifier")


@dataclass
class VerificationResult:
    attribute: str
    prompt: str
    confidence: float
    raw_answer: str = ""
    backend: str = "unknown"


# ─────────────────────────────────────────
# Backend 1: OpenAI-compatible Vision API
# ─────────────────────────────────────────
def _verify_openai(
    crop_jpg: bytes,
    prompt: str,
    api_key: str,
    model: str = "gemini-2.5-flash",
    base_url: str = "https://generativelanguage.googleapis.com/v1beta/openai/",
) -> Optional[tuple[float, str]]:
    try:
        from openai import OpenAI
    except ImportError:
        return None

    b64 = base64.b64encode(crop_jpg).decode()
    system = (
        "You are a visual inspection assistant. "
        "Answer the question about the image with a confidence score 0.0-1.0. "
        "Reply ONLY in JSON: {\"answer\": \"yes\"|\"no\", \"confidence\": 0.xx, \"reason\": \"...\"}"
    )
    try:
        client = OpenAI(api_key=api_key, base_url=base_url)
        resp = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": system},
                {"role": "user", "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/jpeg;base64,{b64}",
                        "detail": "low",
                    }},
                ]},
            ],
            temperature=0.1,
            max_tokens=256,
        )
        text = resp.choices[0].message.content.strip()
        text = re.sub(r"^```(?:json)?\s*", "", text)
        text = re.sub(r"\s*```$", "", text)
        import json
        obj = json.loads(text)
        conf = float(obj.get("confidence", 0.5))
        if obj.get("answer", "").lower().startswith("n"):
            conf = 1.0 - conf
        return conf, text
    except Exception as e:
        LOG.warning("OpenAI vision API failed: %s", e)
        return None


# ─────────────────────────────────────────
# Backend 2: Ollama local VLM
# ─────────────────────────────────────────
def _verify_ollama(
    crop_jpg: bytes,
    prompt: str,
    model: str = "llava",
    ollama_url: str = "http://localhost:11434",
) -> Optional[tuple[float, str]]:
    try:
        import requests
    except ImportError:
        return None

    b64 = base64.b64encode(crop_jpg).decode()
    full_prompt = (
        f"{prompt}\n"
        "Answer with YES or NO followed by a confidence percentage. "
        "Example: YES 85%"
    )
    try:
        resp = requests.post(
            f"{ollama_url}/api/generate",
            json={
                "model": model,
                "prompt": full_prompt,
                "images": [b64],
                "stream": False,
            },
            timeout=30,
        )
        resp.raise_for_status()
        text = resp.json().get("response", "").strip()
        conf = _parse_confidence(text)
        return conf, text
    except Exception as e:
        LOG.warning("Ollama VLM failed: %s", e)
        return None


def _parse_confidence(text: str) -> float:
    """Extract confidence from free-form text like 'YES 85%' or 'No, 30%'."""
    text_lower = text.lower()
    is_yes = text_lower.startswith("yes")

    pct_match = re.search(r"(\d{1,3})%", text)
    if pct_match:
        raw = float(pct_match.group(1)) / 100.0
        return raw if is_yes else 1.0 - raw

    return 0.8 if is_yes else 0.2


# ─────────────────────────────────────────
# Backend 3: CV heuristic fallback
# ─────────────────────────────────────────
_COLOR_HSV_RANGES: dict[str, tuple[np.ndarray, np.ndarray]] = {
    "blue":   (np.array([100, 80, 50]),  np.array([130, 255, 255])),
    "蓝":     (np.array([100, 80, 50]),  np.array([130, 255, 255])),
    "red":    (np.array([0, 80, 50]),    np.array([10, 255, 255])),
    "红":     (np.array([0, 80, 50]),    np.array([10, 255, 255])),
    "green":  (np.array([35, 80, 50]),   np.array([85, 255, 255])),
    "绿":     (np.array([35, 80, 50]),   np.array([85, 255, 255])),
    "yellow": (np.array([20, 80, 50]),   np.array([35, 255, 255])),
    "黄":     (np.array([20, 80, 50]),   np.array([35, 255, 255])),
    "white":  (np.array([0, 0, 180]),    np.array([180, 40, 255])),
    "白":     (np.array([0, 0, 180]),    np.array([180, 40, 255])),
    "black":  (np.array([0, 0, 0]),      np.array([180, 80, 60])),
    "黑":     (np.array([0, 0, 0]),      np.array([180, 80, 60])),
    "orange": (np.array([10, 80, 50]),   np.array([20, 255, 255])),
    "橙":     (np.array([10, 80, 50]),   np.array([20, 255, 255])),
    "purple": (np.array([130, 80, 50]),  np.array([160, 255, 255])),
    "紫":     (np.array([130, 80, 50]),  np.array([160, 255, 255])),
}


def _check_color_cv(bgr: np.ndarray, prompt: str) -> float:
    """Check if the dominant color matches using HSV histogram."""
    hsv = cv2.cvtColor(bgr, cv2.COLOR_BGR2HSV)
    total_pixels = hsv.shape[0] * hsv.shape[1]
    if total_pixels == 0:
        return 0.5

    prompt_lower = prompt.lower()
    for color_name, (lo, hi) in _COLOR_HSV_RANGES.items():
        if color_name in prompt_lower:
            mask = cv2.inRange(hsv, lo, hi)
            if color_name in ("red", "红"):
                lo2, hi2 = np.array([170, 80, 50]), np.array([180, 255, 255])
                mask = mask | cv2.inRange(hsv, lo2, hi2)
            ratio = float(np.count_nonzero(mask)) / total_pixels
            return min(1.0, ratio * 2.5)
    return 0.5


def _check_damage_cv(bgr: np.ndarray) -> float:
    """Estimate surface damage by edge density (scratches = high edge ratio)."""
    gray = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY)
    blurred = cv2.GaussianBlur(gray, (3, 3), 0)
    edges = cv2.Canny(blurred, 50, 150)
    total = edges.shape[0] * edges.shape[1]
    if total == 0:
        return 0.5
    edge_ratio = float(np.count_nonzero(edges)) / total
    return min(1.0, edge_ratio * 5.0)


def _verify_cv_heuristic(
    crop_bgr: np.ndarray,
    attribute: str,
    prompt: str,
) -> tuple[float, str]:
    if attribute == "color":
        conf = _check_color_cv(crop_bgr, prompt)
        return conf, f"CV color check: ratio={conf:.2f}"
    if attribute == "damage":
        conf = _check_damage_cv(crop_bgr)
        return conf, f"CV edge density: score={conf:.2f}"

    return 0.5, "CV heuristic: no handler for this attribute"


# ─────────────────────────────────────────
# Public API
# ─────────────────────────────────────────
class VLMVerifier:
    """Unified verifier that tries backends in priority order."""

    def __init__(
        self,
        openai_api_key: str = "",
        openai_model: str = "gemini-2.5-flash",
        openai_base_url: str = "https://generativelanguage.googleapis.com/v1beta/openai/",
        ollama_model: str = "llava",
        ollama_url: str = "http://localhost:11434",
    ) -> None:
        self.openai_api_key = openai_api_key
        self.openai_model = openai_model
        self.openai_base_url = openai_base_url
        self.ollama_model = ollama_model
        self.ollama_url = ollama_url

    def verify(
        self,
        crop_jpg: bytes,
        crop_bgr: np.ndarray,
        attribute: str,
        prompt: str,
    ) -> VerificationResult:
        """Run verification through available backends."""

        if self.openai_api_key:
            result = _verify_openai(
                crop_jpg, prompt, self.openai_api_key,
                self.openai_model, self.openai_base_url,
            )
            if result is not None:
                return VerificationResult(
                    attribute=attribute, prompt=prompt,
                    confidence=result[0], raw_answer=result[1],
                    backend="openai",
                )

        result = _verify_ollama(
            crop_jpg, prompt, self.ollama_model, self.ollama_url,
        )
        if result is not None:
            return VerificationResult(
                attribute=attribute, prompt=prompt,
                confidence=result[0], raw_answer=result[1],
                backend="ollama",
            )

        conf, reason = _verify_cv_heuristic(crop_bgr, attribute, prompt)
        return VerificationResult(
            attribute=attribute, prompt=prompt,
            confidence=conf, raw_answer=reason,
            backend="cv_heuristic",
        )
