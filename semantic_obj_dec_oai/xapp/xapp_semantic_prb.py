#!/usr/bin/env python3
"""FlexRIC xApp: PRB allocation for semantic object detection.

Supports two allocation modes:
  --allocator heuristic   (default) rule-based 3-level policy
  --allocator rl          deep-RL agent (PPO/SAC trained offline)

Reads semantic_server shared state to determine each UE's bandwidth need:
  - idle:              no detection → minimal PRB (heartbeat only)
  - coarse_detect:     detected + verifying → medium PRB (for crop uploads)
  - requesting_highres: server wants full frame → maximum PRB

Runs on the server host (ics1), requires FlexRIC nearRT-RIC + gNB connected via E2.

Prerequisites:
  export PYTHONPATH=/path/to/flexric/build/examples/xApp/python3:$PYTHONPATH
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import signal
import threading
import time
from dataclasses import dataclass
from typing import Any, Optional



LOG = logging.getLogger("semantic_prb")

SHARED_STATE_PATH = "/tmp/semantic_detection_state.json"
STALE_SECONDS = 30.0
OFFLINE_SECONDS = 120.0


# ─────────────────────────────────────────
# PRB policy: 3-level bandwidth allocation
# ─────────────────────────────────────────
@dataclass
class PRBLevel:
    name: str
    prb_share: float  # fraction of available PRBs (0.0-1.0)


PRB_LEVELS: dict[str, PRBLevel] = {
    "idle":              PRBLevel("idle",          0.10),
    "verifying":         PRBLevel("verifying",     0.30),
    "requesting_highres": PRBLevel("highres",      0.60),
    "verified":          PRBLevel("verified",      0.15),
    "offline":           PRBLevel("offline",       0.00),
}


def _get_prb_level(status: str, need_highres: bool) -> PRBLevel:
    if need_highres:
        return PRB_LEVELS["requesting_highres"]
    return PRB_LEVELS.get(status, PRB_LEVELS["idle"])


# ─────────────────────────────────────────
# MAC metrics store (same pattern as airan version)
# ─────────────────────────────────────────
@dataclass
class UEChannelInfo:
    rnti: int = 0
    ul_aggr_prb: int = 0
    pusch_snr: float = -64.0
    ul_bler: float = 0.0
    wb_cqi: int = 0
    ul_mcs1: int = 0
    bsr: int = 0
    updated_at: float = 0.0


class MACStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._ues: dict[int, UEChannelInfo] = {}

    def update(self, rnti: int, info: UEChannelInfo) -> None:
        with self._lock:
            self._ues[rnti] = info

    def snapshot(self) -> dict[int, UEChannelInfo]:
        with self._lock:
            return dict(self._ues)


mac_store = MACStore()


def _build_mac_callback_class(ric_module: Any) -> type:
    class _MACCallback(ric_module.mac_cb):
        def __init__(self) -> None:
            ric_module.mac_cb.__init__(self)

        def handle(self, ind: Any) -> None:
            if len(ind.ue_stats) == 0:
                return
            for ue in ind.ue_stats:
                info = UEChannelInfo(
                    rnti=int(ue.rnti),
                    ul_aggr_prb=int(ue.ul_aggr_prb),
                    pusch_snr=float(ue.pusch_snr),
                    ul_bler=float(ue.ul_bler),
                    wb_cqi=int(ue.wb_cqi),
                    ul_mcs1=int(ue.ul_mcs1),
                    bsr=int(ue.bsr),
                    updated_at=time.time(),
                )
                mac_store.update(info.rnti, info)

    return _MACCallback


# ─────────────────────────────────────────
# Read semantic server state
# ─────────────────────────────────────────
def read_semantic_state() -> dict[str, Any]:
    try:
        with open(SHARED_STATE_PATH, "r") as f:
            raw: dict[str, Any] = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}

    now = time.time()
    active: dict[str, Any] = {}
    for ue_id, entry in raw.items():
        age = now - entry.get("updated_at", 0)
        if age < OFFLINE_SECONDS:
            active[ue_id] = entry
        else:
            LOG.debug("UE %s offline (%.0fs), removing", ue_id, age)
    return active


# ─────────────────────────────────────────
# PRB allocation  (2 UE aware)
# ─────────────────────────────────────────
MIN_PRB_PER_UE = 3  # never starve a UE completely


def _snr_compensation(snr_db: float) -> float:
    """Low-SNR UEs need more PRBs to achieve the same throughput."""
    if snr_db < 3.0:
        return 2.0
    if snr_db < 8.0:
        return 1.5
    if snr_db < 15.0:
        return 1.2
    return 1.0


def compute_allocation(
    semantic_state: dict[str, Any],
    mac_snapshot: dict[int, UEChannelInfo],
    rnti_map: dict[str, int],
    total_prb: int,
    reserve_prb: int,
) -> dict[str, dict[str, Any]]:
    """Compute per-UE PRB ranges for STATIC slice allocation.

    For 2 UEs the logic is:
      1. Look up each UE's semantic status → base weight
      2. Look up each UE's real SNR via rnti_map → compensation factor
      3. Weighted split of available PRBs, with a minimum floor
    """
    available = total_prb - reserve_prb
    if available <= 0:
        return {}

    ue_demands: list[dict[str, Any]] = []
    for ue_id, entry in semantic_state.items():
        status = entry.get("status", "idle")
        need_highres = entry.get("need_highres", False)
        level = _get_prb_level(status, need_highres)
        if level.prb_share <= 0:
            continue

        snr_comp = 1.0
        rnti = rnti_map.get(ue_id)
        if rnti is not None and rnti in mac_snapshot:
            ch = mac_snapshot[rnti]
            snr_comp = _snr_compensation(ch.pusch_snr)
            LOG.debug("UE %s rnti=0x%04x SNR=%.1fdB comp=%.1f",
                      ue_id, rnti, ch.pusch_snr, snr_comp)

        weight = level.prb_share * snr_comp
        ue_demands.append({
            "ue_id": ue_id,
            "status": status,
            "level": level.name,
            "weight": weight,
            "need_highres": need_highres,
        })

    if not ue_demands:
        return {}

    total_weight = sum(d["weight"] for d in ue_demands)
    if total_weight <= 0:
        total_weight = 1.0

    n = len(ue_demands)
    floor_total = MIN_PRB_PER_UE * n
    distributable = max(0, available - floor_total)

    result: dict[str, dict[str, Any]] = {}
    pos = 0
    for i, d in enumerate(ue_demands):
        share = MIN_PRB_PER_UE + max(0, int(distributable * d["weight"] / total_weight))
        if i == n - 1:
            share = available - pos
        pos_low = pos
        pos_high = min(pos + share - 1, available - 1)
        result[d["ue_id"]] = {
            "pos_low": pos_low,
            "pos_high": pos_high,
            "prb_count": pos_high - pos_low + 1,
            "status": d["status"],
            "level": d["level"],
            "need_highres": d["need_highres"],
        }
        pos = pos_high + 1

    return result


# ─────────────────────────────────────────
# Slice control (FlexRIC)
# ─────────────────────────────────────────
_active_assoc: dict[int, int] = {}
_active_slice_count: int = 0


def _build_addmod_msg(ric_module: Any, slices: list[dict]) -> Any:
    msg = ric_module.slice_ctrl_msg_t()
    msg.type = ric_module.SLICE_CTRL_SM_V0_ADD
    dl = ric_module.ul_dl_slice_conf_t()
    dl.sched_name = "PF"
    dl.len_sched_name = 2
    dl.len_slices = len(slices)
    arr = ric_module.slice_array(len(slices))
    for i, sl in enumerate(slices):
        s = ric_module.fr_slice_t()
        s.id = sl["id"]
        s.label = sl["label"]
        s.len_label = len(sl["label"])
        s.sched = "PF"
        s.len_sched = 2
        s.params.type = ric_module.SLICE_ALG_SM_V0_STATIC
        s.params.u.sta.pos_low = sl["pos_low"]
        s.params.u.sta.pos_high = sl["pos_high"]
        arr[i] = s
    dl.slices = arr
    msg.u.add_mod_slice.dl = dl
    return msg


def _build_assoc_msg(ric_module: Any, assoc: list[dict]) -> Any:
    msg = ric_module.slice_ctrl_msg_t()
    msg.type = ric_module.SLICE_CTRL_SM_V0_UE_SLICE_ASSOC
    msg.u.ue_slice.len_ue_slice = len(assoc)
    arr = ric_module.ue_slice_assoc_array(len(assoc))
    for i, entry in enumerate(assoc):
        a = ric_module.ue_slice_assoc_t()
        a.rnti = entry["rnti"]
        a.dl_id = entry["dl_slice_id"]
        arr[i] = a
    msg.u.ue_slice.ues = arr
    return msg


def _build_del_msg(ric_module: Any, slice_ids: list[int]) -> Any:
    msg = ric_module.slice_ctrl_msg_t()
    msg.type = ric_module.SLICE_CTRL_SM_V0_DEL
    msg.u.del_slice.len_dl = len(slice_ids)
    arr = ric_module.del_dl_array(len(slice_ids))
    for i, sid in enumerate(slice_ids):
        arr[i] = sid
    msg.u.del_slice.dl = arr
    return msg


def apply_slice_allocation(
    ric_module: Any,
    conn: Any,
    node_idx: int,
    allocation: dict[str, dict[str, Any]],
    rnti_map: dict[str, int],
) -> None:
    global _active_slice_count

    if not allocation:
        return

    new_count = len(allocation)

    if _active_slice_count > new_count:
        stale_ids = list(range(new_count, _active_slice_count))
        try:
            ric_module.control_slice_sm(
                conn[node_idx].id, _build_del_msg(ric_module, stale_ids)
            )
            LOG.info("DEL stale slices: %s", stale_ids)
            for r in [r for r, s in _active_assoc.items() if s in stale_ids]:
                del _active_assoc[r]
        except Exception as e:
            LOG.error("DEL failed: %s", e)
        time.sleep(0.5)

    slices = []
    desired_assoc: dict[int, int] = {}
    for i, (ue_id, alloc) in enumerate(allocation.items()):
        slices.append({
            "id": i, "label": f"s{i}",
            "pos_low": alloc["pos_low"], "pos_high": alloc["pos_high"],
        })
        rnti = rnti_map.get(ue_id)
        if rnti is not None:
            desired_assoc[rnti] = i

    try:
        ric_module.control_slice_sm(
            conn[node_idx].id, _build_addmod_msg(ric_module, slices)
        )
        _active_slice_count = new_count
        LOG.info("ADDMOD: %d slice(s)", len(slices))
    except Exception as e:
        LOG.error("ADDMOD failed: %s", e)
        return

    need_assoc = [
        {"rnti": r, "dl_slice_id": s}
        for r, s in desired_assoc.items()
        if _active_assoc.get(r) != s
    ]
    if not need_assoc:
        return

    time.sleep(1)
    try:
        ric_module.control_slice_sm(
            conn[node_idx].id, _build_assoc_msg(ric_module, need_assoc)
        )
        for a in need_assoc:
            _active_assoc[a["rnti"]] = a["dl_slice_id"]
            LOG.info("ASSOC: rnti=0x%04x → slice %d", a["rnti"], a["dl_slice_id"])
    except Exception as e:
        LOG.error("ASSOC failed: %s", e)


# ─────────────────────────────────────────
# Main
# ─────────────────────────────────────────
def main() -> None:
    global SHARED_STATE_PATH

    p = argparse.ArgumentParser(
        description="Semantic xApp: PRB allocation via FlexRIC (heuristic or RL)"
    )
    p.add_argument("--allocator", choices=["heuristic", "rl"], default="heuristic",
                   help="PRB allocation algorithm: heuristic (rule-based) or rl (trained model)")
    p.add_argument("--rl-model", default=None,
                   help="Path to trained RL model .zip (required when --allocator=rl)")
    p.add_argument("--num-ues", type=int, default=2,
                   help="Max UEs the RL model was trained for")
    p.add_argument("--poll-interval", type=float, default=2.0)
    p.add_argument("--total-prb", type=int, default=106)
    p.add_argument("--reserve-prb", type=int, default=6)
    p.add_argument("--mac-interval", default="10ms", choices=["1ms", "2ms", "5ms", "10ms"])
    p.add_argument("--apply-slice", action="store_true")
    p.add_argument("--shared-state-path", default=SHARED_STATE_PATH)
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    SHARED_STATE_PATH = args.shared_state_path

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
    )

    # ── Initialise allocator ──
    rl_agent = None
    if args.allocator == "rl":
        import sys as _sys
        rl_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "rl")
        _sys.path.insert(0, rl_dir)
        from rl_agent import RLPRBAgent

        model_path = args.rl_model
        if model_path is None:
            model_path = os.path.join(rl_dir, "checkpoints", "best_model.zip")
        rl_agent = RLPRBAgent(
            model_path=model_path,
            num_ues=args.num_ues,
            total_prb=args.total_prb,
            reserve_prb=args.reserve_prb,
        )
        LOG.info("Using RL allocator (model=%s)", model_path)
    else:
        LOG.info("Using heuristic allocator")

    stop = threading.Event()

    def _sig(_a: Any, _b: Any) -> None:
        stop.set()

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    try:
        import xapp_sdk as ric
    except ImportError:
        LOG.error(
            "Cannot import xapp_sdk. Set PYTHONPATH to FlexRIC build output, e.g.:\n"
            "  export PYTHONPATH=/path/to/flexric/build/examples/xApp/python3:$PYTHONPATH"
        )
        raise SystemExit(1)

    ric.init()
    conn = ric.conn_e2_nodes()
    if len(conn) == 0:
        LOG.error("No E2 nodes connected")
        raise SystemExit(1)

    for i in range(len(conn)):
        LOG.info("E2 Node [%d]: MCC=%s MNC=%s", i, conn[i].id.plmn.mcc, conn[i].id.plmn.mnc)

    node_idx = 0
    interval_map = {
        "1ms": ric.Interval_ms_1, "2ms": ric.Interval_ms_2,
        "5ms": ric.Interval_ms_5, "10ms": ric.Interval_ms_10,
    }

    MACCallbackClass = _build_mac_callback_class(ric)
    mac_cb = MACCallbackClass()
    mac_hndlr = ric.report_mac_sm(conn[node_idx].id, interval_map[args.mac_interval], mac_cb)
    LOG.info("MAC SM subscribed (interval=%s)", args.mac_interval)
    time.sleep(1)

    last_allocation: dict[str, dict[str, Any]] = {}
    LOG.info("Policy loop started (allocator=%s, interval=%.1fs, total_prb=%d, apply=%s)",
             args.allocator, args.poll_interval, args.total_prb, args.apply_slice)

    try:
        while not stop.is_set():
            sem_state = read_semantic_state()
            mac_snapshot = mac_store.snapshot()

            rnti_list = sorted(mac_snapshot.keys())
            ue_ids = sorted(sem_state.keys())
            rnti_map: dict[str, int] = {}
            for idx, uid in enumerate(ue_ids):
                if idx < len(rnti_list):
                    rnti_map[uid] = rnti_list[idx]

            if rl_agent is not None:
                allocation = rl_agent.decide(sem_state, mac_snapshot)
            else:
                allocation = compute_allocation(
                    sem_state, mac_snapshot, rnti_map,
                    args.total_prb, args.reserve_prb,
                )

            if allocation != last_allocation:
                LOG.info("── PRB allocation updated (%d UE(s), %s) ──",
                         len(allocation), args.allocator)
                for ue_id, alloc in allocation.items():
                    mapped_rnti = rnti_map.get(ue_id)
                    ch_info = ""
                    if mapped_rnti and mapped_rnti in mac_snapshot:
                        ch = mac_snapshot[mapped_rnti]
                        ch_info = (f" SNR={ch.pusch_snr:.1f}dB"
                                   f" MCS={ch.ul_mcs1}"
                                   f" BLER={ch.ul_bler:.2f}"
                                   f" BSR={ch.bsr}")
                    LOG.info(
                        "  UE %s (rnti=%s): PRB[%d-%d] (%d) "
                        "status=%s level=%s highres=%s%s",
                        ue_id,
                        f"0x{mapped_rnti:04x}" if mapped_rnti else "N/A",
                        alloc["pos_low"], alloc["pos_high"], alloc["prb_count"],
                        alloc["status"], alloc["level"], alloc["need_highres"],
                        ch_info,
                    )

                if args.apply_slice and allocation:
                    apply_slice_allocation(ric, conn, node_idx, allocation, rnti_map)
                last_allocation = allocation

            time.sleep(args.poll_interval)
    finally:
        ric.rm_report_mac_sm(mac_hndlr)
        while ric.try_stop == 0:
            time.sleep(0.5)

    LOG.info("xApp exited.")


if __name__ == "__main__":
    main()
