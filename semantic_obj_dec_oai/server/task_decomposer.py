#!/usr/bin/env python3
"""LLM-based task decomposer: converts natural-language mission descriptions
into structured detection tasks for the Robot (YOLO) and Server (VLM).

When an OpenAI-compatible API is available the decomposition is fully automatic.
Otherwise a rule-based fallback parses simple structured commands so the system
remains functional without any external LLM service.
"""
from __future__ import annotations

import json
import logging
import re
from dataclasses import dataclass, field
from typing import Any, Optional

LOG = logging.getLogger("task_decomposer")

# ── COCO-80 class names (used by YOLOv8) ──
COCO_CLASSES: list[str] = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella",
    "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard",
    "sports ball", "kite", "baseball bat", "baseball glove", "skateboard",
    "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork",
    "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
    "couch", "potted plant", "bed", "dining table", "toilet", "tv",
    "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave",
    "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase",
    "scissors", "teddy bear", "hair drier", "toothbrush",
]

COCO_SET = {c.lower() for c in COCO_CLASSES}


@dataclass
class RobotTask:
    """Detection task pushed to the robot (YOLO-Nano)."""
    detect_classes: list[str] = field(default_factory=lambda: ["sports ball"])
    min_confidence: float = 0.3
    crop_padding: float = 1.3
    report_mode: str = "crop"


@dataclass
class VerificationPrompt:
    attribute: str = ""
    prompt: str = ""
    threshold: float = 0.7


@dataclass
class ServerTask:
    """Fine-grained verification task run on server (VLM)."""
    verification_prompts: list[VerificationPrompt] = field(default_factory=list)
    require_highres_if_uncertain: bool = True
    uncertainty_range: tuple[float, float] = (0.4, 0.75)


@dataclass
class PRBPolicy:
    idle: str = "minimal"
    coarse_detect: str = "medium"
    highres_upload: str = "maximum"


@dataclass
class DecomposedTask:
    raw_description: str = ""
    robot_task: RobotTask = field(default_factory=RobotTask)
    server_task: ServerTask = field(default_factory=ServerTask)
    prb_policy: PRBPolicy = field(default_factory=PRBPolicy)


# ─────────────────────────────────────────
# LLM-based decomposition (OpenAI API)
# ─────────────────────────────────────────
_SYSTEM_PROMPT = """\
You are a task-decomposition engine for a robot visual inspection system.

Given a natural-language mission description, output a JSON object with:
1. "robot_task": what the robot should detect locally using YOLOv8 (COCO-80 classes).
   - "detect_classes": list of COCO class names the robot should look for.
   - "min_confidence": float 0.1-0.9 (lower = more sensitive, prefer 0.3-0.4).
2. "server_task": what the server VLM should verify on cropped detections.
   - "verification_prompts": list of {attribute, prompt, threshold}.
     Each prompt is a yes/no question about a visual attribute.
3. "prb_policy": {"idle": "minimal", "coarse_detect": "medium", "highres_upload": "maximum"}.

COCO-80 classes include: person, car, bus, truck, bicycle, motorcycle, sports ball,
bottle, cup, chair, dog, cat, bird, laptop, cell phone, book, etc.

If the target object is not in COCO-80, pick the closest parent category.

Respond ONLY with valid JSON, no markdown fences.
"""


def decompose_with_llm(
    description: str,
    api_key: str,
    model: str = "gemini-2.5-flash",
    base_url: str = "https://generativelanguage.googleapis.com/v1beta/openai/",
) -> Optional[DecomposedTask]:
    """Call an OpenAI-compatible API to decompose the task."""
    try:
        from openai import OpenAI
    except ImportError:
        LOG.warning("openai package not installed, LLM decomposition unavailable")
        return None

    try:
        client = OpenAI(api_key=api_key, base_url=base_url)
        resp = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": _SYSTEM_PROMPT},
                {"role": "user", "content": description},
            ],
            temperature=0.1,
            max_tokens=1024,
        )
        text = resp.choices[0].message.content.strip()
        text = re.sub(r"^```(?:json)?\s*", "", text)
        text = re.sub(r"\s*```$", "", text)
        obj = json.loads(text)
    except Exception as e:
        LOG.error("LLM decomposition failed: %s", e)
        return None

    return _parse_llm_output(description, obj)


def _parse_llm_output(description: str, obj: dict[str, Any]) -> DecomposedTask:
    rt = obj.get("robot_task", {})
    st = obj.get("server_task", {})

    robot = RobotTask(
        detect_classes=rt.get("detect_classes", ["sports ball"]),
        min_confidence=float(rt.get("min_confidence", 0.3)),
    )
    prompts = []
    for p in st.get("verification_prompts", []):
        prompts.append(VerificationPrompt(
            attribute=p.get("attribute", ""),
            prompt=p.get("prompt", ""),
            threshold=float(p.get("threshold", 0.7)),
        ))
    server = ServerTask(verification_prompts=prompts)

    return DecomposedTask(
        raw_description=description,
        robot_task=robot,
        server_task=server,
    )


# ─────────────────────────────────────────
# Rule-based fallback decomposition
# ─────────────────────────────────────────
_COLOR_WORDS = {
    "红", "蓝", "绿", "黄", "白", "黑", "橙", "紫", "粉", "灰", "棕",
    "red", "blue", "green", "yellow", "white", "black", "orange",
    "purple", "pink", "gray", "grey", "brown",
}

_DAMAGE_WORDS = {
    "划痕", "裂纹", "破损", "缺口", "磨损", "凹陷", "损坏", "锈",
    "scratch", "crack", "damage", "dent", "worn", "rust", "broken", "chip",
}

_OBJECT_MAP_ZH: dict[str, str] = {
    "球": "sports ball", "人": "person", "车": "car", "狗": "dog",
    "猫": "cat", "瓶子": "bottle", "杯子": "cup", "椅子": "chair",
    "手机": "cell phone", "电脑": "laptop", "书": "book", "鸟": "bird",
    "自行车": "bicycle", "摩托车": "motorcycle", "飞机": "airplane",
    "公交": "bus", "卡车": "truck", "船": "boat", "伞": "umbrella",
    "背包": "backpack", "刀": "knife", "碗": "bowl", "苹果": "apple",
    "香蕉": "banana", "橘子": "orange", "沙发": "couch", "桌子": "dining table",
    "电视": "tv", "时钟": "clock", "花瓶": "vase", "剪刀": "scissors",
    "泰迪熊": "teddy bear",
}


def decompose_with_rules(description: str) -> DecomposedTask:
    """Heuristic fallback: parse Chinese/English description into structured task."""
    desc_lower = description.lower()
    detect_classes: list[str] = []

    for zh, en in _OBJECT_MAP_ZH.items():
        if zh in description:
            detect_classes.append(en)

    for coco in COCO_CLASSES:
        if coco in desc_lower and coco not in detect_classes:
            detect_classes.append(coco)

    if not detect_classes:
        detect_classes = ["sports ball"]

    colors_found = [c for c in _COLOR_WORDS if c in desc_lower or c in description]
    damages_found = [d for d in _DAMAGE_WORDS if d in desc_lower or d in description]

    prompts: list[VerificationPrompt] = []
    for color in colors_found:
        prompts.append(VerificationPrompt(
            attribute="color",
            prompt=f"Is the main color of this object {color}?",
            threshold=0.7,
        ))
    for damage in damages_found:
        prompts.append(VerificationPrompt(
            attribute="damage",
            prompt=f"Does this object have {damage} on its surface?",
            threshold=0.65,
        ))

    if not prompts:
        prompts.append(VerificationPrompt(
            attribute="general",
            prompt=f"Does this image match the description: {description}?",
            threshold=0.6,
        ))

    return DecomposedTask(
        raw_description=description,
        robot_task=RobotTask(
            detect_classes=detect_classes,
            min_confidence=0.3,
        ),
        server_task=ServerTask(verification_prompts=prompts),
    )


# ─────────────────────────────────────────
# Public API
# ─────────────────────────────────────────
def decompose_task(
    description: str,
    api_key: str = "",
    model: str = "gemini-2.5-flash",
    base_url: str = "https://generativelanguage.googleapis.com/v1beta/openai/",
) -> DecomposedTask:
    """Decompose a natural-language task.  Uses LLM if api_key is provided,
    otherwise falls back to rule-based parsing."""
    if api_key:
        result = decompose_with_llm(description, api_key, model, base_url)
        if result is not None:
            LOG.info("Task decomposed via LLM: %s", result)
            return result
        LOG.warning("LLM failed, falling back to rules")

    result = decompose_with_rules(description)
    LOG.info("Task decomposed via rules: robot_classes=%s, prompts=%d",
             result.robot_task.detect_classes, len(result.server_task.verification_prompts))
    return result
