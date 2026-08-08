"""
RL inference agent for real-time PRB allocation.

Loads a trained SB3 model (PPO/SAC) and exposes a simple API:
    agent = RLPRBAgent("checkpoints/ppo_semantic_prb_best.zip", num_ues=2, total_prb=106)
    allocation = agent.decide(semantic_state, mac_snapshot)

Returns the same dict format as the heuristic `compute_allocation`, so it's
a drop-in replacement inside xapp_semantic_prb.py.
"""

from __future__ import annotations

import logging
import os
import sys
from typing import Any

import numpy as np

LOG = logging.getLogger("rl_agent")

_STATUS_TO_INT = {"idle": 0, "verifying": 1, "requesting_highres": 2, "verified": 3}

HIGHRES_SIZE_BYTES = 150_000
HIGHRES_DEADLINE_STEPS = 5  # matching env default (500ms / 100ms epoch)


class RLPRBAgent:
    """Wraps a trained Stable-Baselines3 model for real-time PRB decisions."""

    def __init__(
        self,
        model_path: str,
        num_ues: int = 2,
        total_prb: int = 106,
        reserve_prb: int = 6,
        snr_range: tuple[float, float] = (0.0, 30.0),
        fallback: str = "heuristic",
    ):
        self.num_ues = num_ues
        self.total_prb = total_prb
        self.reserve_prb = reserve_prb
        self.available_prb = total_prb - reserve_prb
        self.snr_range = snr_range
        self.fallback = fallback
        self._model = None

        if not os.path.exists(model_path):
            LOG.warning("RL model not found at %s, will use fallback=%s", model_path, fallback)
            return

        try:
            from stable_baselines3 import PPO, SAC

            if "sac" in os.path.basename(model_path).lower():
                self._model = SAC.load(model_path)
            else:
                self._model = PPO.load(model_path)
            LOG.info("Loaded RL model from %s (%s)", model_path, type(self._model).__name__)
        except Exception as exc:
            LOG.warning("Failed to load RL model: %s, using fallback=%s", exc, fallback)

    # ── Public API ────────────────────────────────────────────
    def decide(
        self,
        semantic_state: dict[str, Any],
        mac_snapshot: dict[int, Any],
    ) -> dict[str, dict[str, Any]]:
        """
        Decide PRB allocation.

        Parameters
        ----------
        semantic_state : per-UE semantic detection state from shared JSON.
        mac_snapshot   : per-RNTI UEChannelInfo from FlexRIC MAC SM.

        Returns
        -------
        Dict[ue_id -> {pos_low, pos_high, prb_count, status, level, need_highres}]
        Same format as heuristic compute_allocation.
        """
        ue_ids = sorted(semantic_state.keys())[:self.num_ues]
        if not ue_ids:
            return {}

        if self._model is None:
            return self._fallback_allocation(ue_ids, semantic_state, mac_snapshot)

        obs = self._build_obs(ue_ids, semantic_state, mac_snapshot)
        action, _ = self._model.predict(obs, deterministic=True)
        return self._action_to_allocation(action, ue_ids, semantic_state)

    # ── Observation builder ───────────────────────────────────
    def _build_obs(
        self,
        ue_ids: list[str],
        semantic_state: dict[str, Any],
        mac_snapshot: dict[int, Any],
    ) -> np.ndarray:
        """Build the same observation vector as the training env."""
        parts = []
        mac_by_id = self._match_mac_to_ue(ue_ids, mac_snapshot)

        for uid in ue_ids:
            entry = semantic_state.get(uid, {})
            ch = mac_by_id.get(uid)

            snr = ch.pusch_snr if ch else 15.0
            bler = ch.ul_bler if ch else 0.01
            cqi = ch.wb_cqi if ch else 8
            mcs = ch.ul_mcs1 if ch else 10
            bsr = ch.bsr if ch else 0

            snr_lo, snr_hi = self.snr_range
            snr_norm = np.clip((snr - snr_lo) / (snr_hi - snr_lo + 1e-6), 0, 1)
            cqi_norm = min(1.0, cqi / 15.0)
            mcs_norm = min(1.0, mcs / 27.0)
            bler = np.clip(bler, 0, 1)
            bsr_norm = min(1.0, bsr / (HIGHRES_SIZE_BYTES + 1))

            status = entry.get("status", "idle")
            status_oh = [0.0, 0.0, 0.0, 0.0]
            idx = _STATUS_TO_INT.get(status, 0)
            status_oh[idx] = 1.0

            need_hr = 1.0 if entry.get("need_highres", False) else 0.0
            queue_est = HIGHRES_SIZE_BYTES if need_hr else 0
            queue_norm = min(1.0, queue_est / (HIGHRES_SIZE_BYTES + 1))
            wait = entry.get("wait_steps", 0)
            wait_norm = min(1.0, wait / (HIGHRES_DEADLINE_STEPS + 1))

            parts.append(np.array([
                snr_norm, bler, cqi_norm, mcs_norm, bsr_norm,
                status_oh[0], status_oh[1], status_oh[2], status_oh[3],
                need_hr, queue_norm, wait_norm,
            ], dtype=np.float32))

        # Pad if fewer UEs than training expects
        per_ue = 12
        while len(parts) < self.num_ues:
            parts.append(np.zeros(per_ue, dtype=np.float32))

        return np.concatenate(parts)

    # ── Action → allocation dict ──────────────────────────────
    def _action_to_allocation(
        self,
        action: np.ndarray,
        ue_ids: list[str],
        semantic_state: dict[str, Any],
    ) -> dict[str, dict[str, Any]]:
        n = len(ue_ids)
        weights = np.clip(action[:n], 1e-6, None)
        shares = weights / weights.sum()
        prb_alloc = np.floor(shares * self.available_prb).astype(int)
        remainder = self.available_prb - prb_alloc.sum()
        for _ in range(int(remainder)):
            prb_alloc[np.argmax(shares)] += 1

        result: dict[str, dict[str, Any]] = {}
        pos = 0
        for i, uid in enumerate(ue_ids):
            count = int(prb_alloc[i])
            if count <= 0:
                continue
            entry = semantic_state.get(uid, {})
            status = entry.get("status", "idle")
            need_hr = entry.get("need_highres", False)

            pos_high = min(pos + count - 1, self.available_prb - 1)
            result[uid] = {
                "pos_low": pos,
                "pos_high": pos_high,
                "prb_count": pos_high - pos + 1,
                "status": status,
                "level": f"rl_{status}",
                "need_highres": need_hr,
            }
            pos = pos_high + 1

        return result

    # ── MAC matching ──────────────────────────────────────────
    @staticmethod
    def _match_mac_to_ue(
        ue_ids: list[str],
        mac_snapshot: dict[int, Any],
    ) -> dict[str, Any]:
        """Best-effort match: ue_id may contain RNTI, or positional."""
        result: dict[str, Any] = {}
        rntis = sorted(mac_snapshot.keys())
        for i, uid in enumerate(ue_ids):
            try:
                rnti = int(uid)
                if rnti in mac_snapshot:
                    result[uid] = mac_snapshot[rnti]
                    continue
            except (ValueError, TypeError):
                pass
            if i < len(rntis):
                result[uid] = mac_snapshot[rntis[i]]
        return result

    # ── Heuristic fallback ────────────────────────────────────
    def _fallback_allocation(
        self,
        ue_ids: list[str],
        semantic_state: dict[str, Any],
        mac_snapshot: dict[int, Any],
    ) -> dict[str, dict[str, Any]]:
        """Static 3-level heuristic (same as rule-based xApp)."""
        _LEVELS = {
            "idle": 0.10,
            "verifying": 0.30,
            "requesting_highres": 0.60,
            "verified": 0.15,
        }
        weights = []
        for uid in ue_ids:
            entry = semantic_state.get(uid, {})
            st = entry.get("status", "idle")
            if entry.get("need_highres", False):
                st = "requesting_highres"
            weights.append(_LEVELS.get(st, 0.10))

        total_w = sum(weights) or 1.0
        result: dict[str, dict[str, Any]] = {}
        pos = 0
        for i, uid in enumerate(ue_ids):
            count = max(1, int(self.available_prb * weights[i] / total_w))
            if i == len(ue_ids) - 1:
                count = self.available_prb - pos
            entry = semantic_state.get(uid, {})
            status = entry.get("status", "idle")
            pos_high = min(pos + count - 1, self.available_prb - 1)
            result[uid] = {
                "pos_low": pos,
                "pos_high": pos_high,
                "prb_count": pos_high - pos + 1,
                "status": status,
                "level": f"heuristic_{status}",
                "need_highres": entry.get("need_highres", False),
            }
            pos = pos_high + 1
        return result
