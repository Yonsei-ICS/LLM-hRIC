"""
Gymnasium environment that simulates semantic-object-detection PRB scheduling.

The environment models:
 - N UEs, each with a time-varying wireless channel (SNR, CQI, MCS, BLER)
 - Each UE transitions through detection states: idle → verifying → requesting_highres → verified → idle
 - The agent decides PRB allocation fractions for each UE every decision epoch
 - Reward combines: detection-task completion rate, latency, fairness, PRB efficiency

The sim is lightweight (no real RAN) so RL training can run millions of steps on CPU.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any

import gymnasium as gym
import numpy as np
from gymnasium import spaces


# ── Channel model ──────────────────────────────────────────────
# Simplified TBS lookup: maps (MCS, num_PRB) → bytes per slot (1 ms)
# Loosely based on 3GPP 38.214 Table 5.1.3.1-2 for QPSK/16QAM/64QAM, 1 layer
_MCS_EFFICIENCY = {
    # mcs_index: spectral efficiency  bits/RE  (approximate)
    0: 0.23, 1: 0.31, 2: 0.49, 3: 0.60, 4: 0.74,
    5: 0.88, 6: 1.03, 7: 1.18, 8: 1.33, 9: 1.48,
    10: 1.69, 11: 1.91, 12: 2.16, 13: 2.41, 14: 2.57,
    15: 2.73, 16: 3.03, 17: 3.32, 18: 3.61, 19: 3.90,
    20: 4.21, 21: 4.52, 22: 4.82, 23: 5.12, 24: 5.55,
    25: 5.89, 26: 6.23, 27: 6.57,
}

RE_PER_PRB_PER_SLOT = 12 * 14  # 12 subcarriers × 14 OFDM symbols (normal CP)
OVERHEAD_FACTOR = 0.85          # DMRS + control overhead


def _snr_to_mcs(snr_db: float) -> int:
    """Map SNR to MCS index (simplified, loosely from link-level curves)."""
    if snr_db < -3:
        return 0
    if snr_db > 30:
        return 27
    return min(27, max(0, int((snr_db + 3) * 27 / 33)))


def _tbs_bytes(mcs: int, num_prb: int) -> int:
    """Estimate Transport Block Size in bytes for one 1-ms slot."""
    eff = _MCS_EFFICIENCY.get(mcs, 0.23)
    bits = eff * RE_PER_PRB_PER_SLOT * num_prb * OVERHEAD_FACTOR
    return max(0, int(bits / 8))


def _bler_from_mcs_snr(mcs: int, snr_db: float) -> float:
    """Simple BLER model: higher MCS at low SNR => higher BLER."""
    target_snr = -3.0 + mcs * (33.0 / 27.0)
    margin = snr_db - target_snr
    if margin > 5:
        return 0.001
    if margin > 0:
        return 0.01
    if margin > -3:
        return 0.10
    return 0.30


# ── UE state machine ──────────────────────────────────────────
@dataclass
class SimUE:
    """Per-UE simulation state."""
    ue_id: int = 0
    # Channel
    snr_db: float = 15.0
    snr_drift_speed: float = 0.0   # random walk speed
    mcs: int = 10
    bler: float = 0.01
    cqi: int = 8

    # Detection state: idle / verifying / requesting_highres / verified
    det_state: str = "idle"
    det_timer: int = 0             # steps remaining in current state

    # Data queue (bytes pending upload)
    tx_queue: int = 0
    # Allocated PRBs this epoch
    allocated_prb: int = 0
    # Tracking
    total_data_sent: int = 0
    total_data_generated: int = 0
    highres_delivered: int = 0
    highres_requested: int = 0
    highres_deadline_missed: int = 0
    wait_since_request: int = 0
    cumulative_delay: int = 0

    # BSR (buffer status)
    bsr: int = 0


_STATUS_TO_INT = {"idle": 0, "verifying": 1, "requesting_highres": 2, "verified": 3}


# ── Data sizes ─────────────────────────────────────────────────
CROP_SIZE_BYTES = 5_000          # ~5 KB crop JPEG
HIGHRES_SIZE_BYTES = 150_000     # ~150 KB full-frame JPEG
HEARTBEAT_SIZE_BYTES = 200


# ── Gymnasium Env ──────────────────────────────────────────────
class SemanticPRBEnv(gym.Env):
    """
    Multi-UE PRB scheduling environment for semantic object detection over 5G.

    Observation (per UE, concatenated):
        [snr_norm, bler, cqi_norm, mcs_norm, bsr_norm,
         status_idle, status_verifying, status_highres, status_verified,
         need_highres, queue_norm, wait_norm]

    Action: continuous [0,1]^N — raw PRB-share weights (softmax-normalised internally)

    Reward: weighted sum of success_rate, -delay, fairness, -waste
    """

    metadata = {"render_modes": []}

    def __init__(
        self,
        num_ues: int = 2,
        total_prb: int = 106,        # 20 MHz NR
        reserve_prb: int = 6,
        epoch_ms: int = 100,          # decision interval
        max_steps: int = 1000,
        snr_range: tuple[float, float] = (0.0, 30.0),
        detection_prob: float = 0.15, # probability a UE detects something each idle step
        highres_deadline_ms: int = 500,
        reward_weights: dict[str, float] | None = None,
        seed: int | None = None,
    ):
        super().__init__()
        self.num_ues = num_ues
        self.total_prb = total_prb
        self.reserve_prb = reserve_prb
        self.available_prb = total_prb - reserve_prb
        self.epoch_ms = epoch_ms
        self.slots_per_epoch = epoch_ms  # 1 slot = 1 ms in NR
        self.max_steps = max_steps
        self.snr_range = snr_range
        self.detection_prob = detection_prob
        self.highres_deadline_steps = max(1, highres_deadline_ms // epoch_ms)

        rw = reward_weights or {}
        self.w_success = rw.get("success", 2.0)
        self.w_delay = rw.get("delay", 0.5)
        self.w_fairness = rw.get("fairness", 0.5)
        self.w_waste = rw.get("waste", 0.3)

        per_ue_obs = 12  # see _obs_for_ue
        self.observation_space = spaces.Box(
            low=0.0, high=1.0,
            shape=(num_ues * per_ue_obs,),
            dtype=np.float32,
        )
        self.action_space = spaces.Box(
            low=0.0, high=1.0,
            shape=(num_ues,),
            dtype=np.float32,
        )

        self._rng = np.random.default_rng(seed)
        self.ues: list[SimUE] = []
        self._step_count = 0

    # ── reset / step ────────────────────────────────────────────
    def reset(self, *, seed: int | None = None, options: dict | None = None):
        if seed is not None:
            self._rng = np.random.default_rng(seed)
        self._step_count = 0
        self.ues = []
        for i in range(self.num_ues):
            snr = self._rng.uniform(*self.snr_range)
            ue = SimUE(
                ue_id=i,
                snr_db=snr,
                snr_drift_speed=self._rng.uniform(-0.5, 0.5),
                mcs=_snr_to_mcs(snr),
            )
            ue.cqi = min(15, max(0, int(ue.mcs * 15 / 27)))
            ue.bler = _bler_from_mcs_snr(ue.mcs, snr)
            self.ues.append(ue)
        return self._get_obs(), {}

    def step(self, action: np.ndarray):
        self._step_count += 1

        # 1) Normalise action → PRB allocation
        weights = np.clip(action, 1e-6, None)
        shares = weights / weights.sum()
        prb_alloc = np.floor(shares * self.available_prb).astype(int)
        remainder = self.available_prb - prb_alloc.sum()
        for _ in range(int(remainder)):
            idx = self._rng.integers(self.num_ues)
            prb_alloc[idx] += 1

        # 2) Per-UE simulation
        epoch_successes = 0
        epoch_delays = []
        epoch_waste = 0
        throughputs = []

        for i, ue in enumerate(self.ues):
            ue.allocated_prb = int(prb_alloc[i])
            self._evolve_channel(ue)
            self._evolve_detection_state(ue)

            # Data generation based on state
            if ue.det_state == "verifying":
                ue.tx_queue += CROP_SIZE_BYTES
                ue.total_data_generated += CROP_SIZE_BYTES
            elif ue.det_state == "requesting_highres":
                if ue.wait_since_request == 0:
                    ue.tx_queue += HIGHRES_SIZE_BYTES
                    ue.total_data_generated += HIGHRES_SIZE_BYTES
                    ue.highres_requested += 1
            elif ue.det_state == "idle":
                ue.tx_queue += HEARTBEAT_SIZE_BYTES

            # Simulate transmission over the epoch (all slots)
            tbs_per_slot = _tbs_bytes(ue.mcs, ue.allocated_prb)
            effective_rate = tbs_per_slot * (1.0 - ue.bler)
            bytes_sent = int(effective_rate * self.slots_per_epoch)
            actually_sent = min(bytes_sent, ue.tx_queue)
            ue.tx_queue = max(0, ue.tx_queue - bytes_sent)
            ue.total_data_sent += actually_sent
            ue.bsr = ue.tx_queue

            throughputs.append(actually_sent)
            waste_bytes = max(0, bytes_sent - actually_sent)
            epoch_waste += waste_bytes

            # Highres tracking
            if ue.det_state == "requesting_highres":
                ue.wait_since_request += 1
                if ue.tx_queue == 0:
                    epoch_successes += 1
                    ue.highres_delivered += 1
                    epoch_delays.append(ue.wait_since_request)
                    ue.cumulative_delay += ue.wait_since_request
                    ue.det_state = "verified"
                    ue.det_timer = self._rng.integers(2, 6)
                    ue.wait_since_request = 0
                elif ue.wait_since_request >= self.highres_deadline_steps:
                    ue.highres_deadline_missed += 1
                    epoch_delays.append(ue.wait_since_request)
                    ue.cumulative_delay += ue.wait_since_request
                    ue.tx_queue = 0
                    ue.det_state = "idle"
                    ue.det_timer = self._rng.integers(1, 5)
                    ue.wait_since_request = 0

        # 3) Compute reward
        total_requested = sum(u.highres_requested for u in self.ues)
        total_delivered = sum(u.highres_delivered for u in self.ues)
        success_rate = total_delivered / max(1, total_requested)

        avg_delay = np.mean(epoch_delays) / self.highres_deadline_steps if epoch_delays else 0.0

        tp_array = np.array(throughputs, dtype=np.float64) + 1e-9
        jain_fairness = (tp_array.sum() ** 2) / (self.num_ues * (tp_array ** 2).sum())

        max_possible = _tbs_bytes(27, self.available_prb) * self.slots_per_epoch * self.num_ues
        waste_ratio = epoch_waste / max(1, max_possible)

        reward = (
            self.w_success * success_rate
            - self.w_delay * avg_delay
            + self.w_fairness * jain_fairness
            - self.w_waste * waste_ratio
        )

        # 4) Done?
        truncated = self._step_count >= self.max_steps
        terminated = False

        info = {
            "success_rate": success_rate,
            "avg_delay": avg_delay,
            "jain_fairness": jain_fairness,
            "waste_ratio": waste_ratio,
            "prb_alloc": prb_alloc.tolist(),
            "throughputs": throughputs,
        }

        return self._get_obs(), float(reward), terminated, truncated, info

    # ── observation builder ────────────────────────────────────
    def _obs_for_ue(self, ue: SimUE) -> np.ndarray:
        snr_lo, snr_hi = self.snr_range
        snr_norm = np.clip((ue.snr_db - snr_lo) / (snr_hi - snr_lo + 1e-6), 0, 1)
        cqi_norm = ue.cqi / 15.0
        mcs_norm = ue.mcs / 27.0
        bler = np.clip(ue.bler, 0, 1)
        bsr_norm = min(1.0, ue.bsr / (HIGHRES_SIZE_BYTES + 1))
        queue_norm = min(1.0, ue.tx_queue / (HIGHRES_SIZE_BYTES + 1))
        wait_norm = min(1.0, ue.wait_since_request / (self.highres_deadline_steps + 1))

        status_oh = [0.0, 0.0, 0.0, 0.0]
        idx = _STATUS_TO_INT.get(ue.det_state, 0)
        status_oh[idx] = 1.0

        need_hr = 1.0 if ue.det_state == "requesting_highres" else 0.0

        return np.array([
            snr_norm, bler, cqi_norm, mcs_norm, bsr_norm,
            status_oh[0], status_oh[1], status_oh[2], status_oh[3],
            need_hr, queue_norm, wait_norm,
        ], dtype=np.float32)

    def _get_obs(self) -> np.ndarray:
        parts = [self._obs_for_ue(ue) for ue in self.ues]
        return np.concatenate(parts)

    # ── channel evolution ──────────────────────────────────────
    def _evolve_channel(self, ue: SimUE) -> None:
        ue.snr_drift_speed += self._rng.normal(0, 0.1)
        ue.snr_drift_speed = np.clip(ue.snr_drift_speed, -1.0, 1.0)
        ue.snr_db += ue.snr_drift_speed + self._rng.normal(0, 0.5)
        ue.snr_db = np.clip(ue.snr_db, *self.snr_range)
        ue.mcs = _snr_to_mcs(ue.snr_db)
        ue.cqi = min(15, max(0, int(ue.mcs * 15 / 27)))
        ue.bler = _bler_from_mcs_snr(ue.mcs, ue.snr_db)

    # ── detection state machine ────────────────────────────────
    def _evolve_detection_state(self, ue: SimUE) -> None:
        if ue.det_state == "idle":
            if ue.det_timer > 0:
                ue.det_timer -= 1
            elif self._rng.random() < self.detection_prob:
                ue.det_state = "verifying"
                ue.det_timer = self._rng.integers(1, 4)
        elif ue.det_state == "verifying":
            if ue.det_timer > 0:
                ue.det_timer -= 1
            else:
                if self._rng.random() < 0.5:
                    ue.det_state = "requesting_highres"
                    ue.wait_since_request = 0
                else:
                    ue.det_state = "idle"
                    ue.det_timer = self._rng.integers(3, 10)
        elif ue.det_state == "verified":
            if ue.det_timer > 0:
                ue.det_timer -= 1
            else:
                ue.det_state = "idle"
                ue.det_timer = self._rng.integers(2, 8)
        # requesting_highres is handled in step() via queue drain


# ── Register with Gymnasium ────────────────────────────────────
gym.register(
    id="SemanticPRB-v0",
    entry_point="prb_env:SemanticPRBEnv",
)
