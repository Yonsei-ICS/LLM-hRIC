# LLM-hRIC for O-RAN Network Slicing on OpenAirInterface + FlexRIC

This repository is the implementation artifact for **LLM-hRIC: LLM-empowered Hierarchical RAN
Intelligent Control for O-RAN** ([arXiv:2504.18062](https://arxiv.org/abs/2504.18062), Bao, Yun, Lee
and Quek). The magazine paper establishes the hierarchical architecture — a non-real-time LLM rApp
that translates operator intent into an A1-like policy, and a near-real-time reinforcement-learning
xApp that acts inside the envelope that policy defines — using a power-allocation use case in IAB
networks. **This repository extends that architecture to a different control problem: downlink PRB
allocation across two S-NSSAIs on a single 5G NR gNB**, and documents exactly what had to be changed
in OpenAirInterface (OAI) and FlexRIC to make it run end to end on real RAN software. The companion
appendix paper in [`doc/llm_hric_slicing_demo_paper/`](doc/llm_hric_slicing_demo_paper) describes the
design; this README is the operational counterpart that lets you build, run and reproduce it.

Concretely, the loop is: a locally hosted Gemma rApp reads three 10-second slice-state windows every
10 s and publishes an A1-like policy carrying a target PRB split plus calibrated feasible bounds; a
100 ms near-real-time controller (a TD3-style agent, an LLM-only projector, or a fusion of the two)
picks an integer PRB split inside those bounds; a persistent FlexRIC xApp converts it into an
E2SM-RC Control Style 2 / Action 6 (*Slice-level PRB quota*) message; and a new OAI MAC downlink
scheduler policy enforces the resulting per-S-NSSAI PRB budget slot by slot.

---

## What this is / what this is not

**This is:**

- A research prototype that runs a complete, unmodified-protocol O-RAN control loop against real OAI
  RAN software: E2AP v2, E2SM-RC (ASN.1/PER), E2SM-KPM v2.03, and the FlexRIC near-RT RIC.
- Standards-aligned where stated: the RC control message reproduces the O-RAN E2SM-RC *RRM Policy
  Ratio List* parameter hierarchy (RAN Parameter IDs 1–12), and the KPM audit path uses 3GPP
  TS 28.552 measurement names and units.
- A reproducible experiment harness: a spec-driven campaign runner, per-run SQLite artifacts,
  automatic validity gates, and offline re-scoring tools.

**This is not:**

- An O-RAN certified or conformance-tested product. Several encodings are deliberately
  prototype-specific (see [Known limitations](#known-limitations)).
- A multi-cell, multi-gNB, or disaggregated (CU/DU split) deployment. Everything below is a **single
  monolithic gNB** on the **RFSimulator**, **5 UEs**, **2 S-NSSAIs**, **downlink only**, 106 PRB in
  band n78.
- Production-hardened. The actuator, the RIC and the monitor each have documented single points of
  failure that did not trigger in our runs but would matter in a long-lived deployment.

---

## Architecture

```
 ┌──────────────────────────── non-real-time (10 s) ────────────────────────────┐
 │                                                                              │
 │   operator intent  ──POST /a1-p/intents/{id}──►  A1-like HTTP server         │
 │   (free text)                                    127.0.0.1:8088              │
 │                                                        │                     │
 │                                                        ▼                     │
 │                                             ┌────────────────────┐           │
 │   3 × 10 s slice-state windows  ──────────► │  Gemma rApp        │           │
 │   (from network_state)                      │  transformers,     │           │
 │   + calibrated policy_constraints           │  4-bit NF4, greedy │           │
 │                                             └─────────┬──────────┘           │
 │                                                       │ JSON {prb_ratio,     │
 │                                                       │       confidence}    │
 │                                     project into calibrated bounds           │
 │                                                       ▼                      │
 │                                      A1-like policy (versioned, active=1)    │
 └───────────────────────────────────────────┬──────────────────────────────────┘
                                             │ read from shared SQLite
 ┌───────────────────────────── near-real-time (100 ms) ────────────────────────┐
 │                                             ▼                                │
 │   24-D state ──►  controller  ──►  b_A ∈ [a_min, a_max] PRB of 106           │
 │                   arm ∈ {llm_only | ddpg_only | llm_guided_ddpg}             │
 │                        │                          ▲                          │
 │                        │                          │ Actor snapshot           │
 │                        │              ┌───────────┴────────────┐             │
 │                        │              │ asynchronous learner   │             │
 │                        │              │ TD3-style, own process │             │
 │                        │              │ 1 update / valid txn   │             │
 │                        │              └───────────▲────────────┘             │
 │                        │                          │ ddpg_replay_transitions  │
 │                        ▼                          │                          │
 │             current_prb_policy.json ──────────────┘                          │
 │                        │                                                     │
 │                        │ {request_id, policy_file} over AF_UNIX              │
 │                        ▼                                                     │
 │        ┌───────────────────────────────┐                                     │
 │        │ xapp_rc_slice_ctrl (--serve)  │  persistent RC actuator             │
 │        └───────────────┬───────────────┘                                     │
 └────────────────────────┼─────────────────────────────────────────────────────┘
                          │ E2SM-RC Style 2 / Action 6, RRM Policy Ratio List
                          ▼
              ┌──────────────────────┐   E2AP v2 / SCTP   ┌────────────────────┐
              │  FlexRIC nearRT-RIC  │◄──────────────────►│  OAI gNB E2 agent  │
              └──────────┬───────────┘                    └─────────┬──────────┘
                         │ E42                                      │
        ┌────────────────┴─────────────────┐                        ▼
        │                                  │              nr_dl_slice_prb_policy()
        ▼                                  ▼              per-S-NSSAI PRB budget
  SM monitor xApp                   KPM collector          in the DL scheduler
  MAC/RLC/PDCP/GTP                  E2SM-KPM, 1000 ms                │
  10 ms sub / 50 ms sample          styles 1 and 4                   │
        │                                  │                         │
        └──────────────┬───────────────────┘                         │
                       ▼                                             ▼
        /tmp/llm_hric/llm_hric.sqlite3  (WAL, 6 concurrent writers)   106 PRB
        raw tables → 50 ms windowing → network_state, ue_slice_throughput
                       │
                       └──► Grafana (SQLite datasource, :3000)  ──►  paper tables
```

Clock separation, all verified in code: E2 report interval **10 ms** → monitor admits one indication
per service model per **50 ms** → derived summary windows **50–60 ms** → controller tick **100 ms** →
rApp **10 s** → KPM **1000 ms** (compile-time constant) → learner free-running (100 ms poll).

---

## Repository layout

Paths are relative to the OAI checkout root (`$OAI` below). See the
[Publishing note](#publishing-note) for how this maps onto a public repository.

### OAI modifications

| Path | Status | Role |
|---|---|---|
| `openair2/E2AP/RAN_FUNCTION/O-RAN/rc_ctrl_service_style_2.c` | **new**, 216 lines | Decodes the RRM Policy Ratio List, validates it, installs the table. Body is `#if defined(NGRAN_GNB_DU)`. |
| `openair2/E2AP/RAN_FUNCTION/O-RAN/rc_ctrl_service_style_2.h` | **new**, 37 lines | RAN Parameter ID enum (1–12) and Control Action ID enum (1–6). |
| `openair2/E2AP/RAN_FUNCTION/O-RAN/ran_func_rc.c` | modified | Advertises a 2nd control style (`sz_seq_ctrl_style = 2`) and dispatches Style 2 / Action 6. |
| `openair2/E2AP/RAN_FUNCTION/CMakeLists.txt` | modified | Adds the new source to both `e2_ran_func_cuup` and `e2_ran_func_du_cucp_cuup`. |
| `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch_default_policies.c` | modified | New allocator `nr_dl_slice_prb_policy()` + installer `nr_mac_set_dl_slice_policies()`; stock PF refactored into reusable passes. |
| `openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch_default_policies.h` | modified | Exports the two new functions. |
| `openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h` | modified | `nr_dl_slice_policy_t` and the five new `gNB_MAC_INST` fields. |
| `openair2/LAYER2/NR_MAC_gNB/main.c` | modified | Default `dl_rb_alloc` becomes `nr_dl_slice_prb_policy`. |
| `radio/rfsimulator/simulator.cpp` | modified | **Build portability only.** The option table is rewritten with helper functions because GCC 10's C++ front end rejects the designated-initialiser parameter macros. No behaviour change. |
| `ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf` | modified | Adds the 2nd S-NSSAI, an `e2_agent` block, and changes the local N2/N3 address. |
| `ci-scripts/conf_files/nrue.uicc.conf` | modified | Explicit PDU session id, DNN `oai`, SD `0xFFFFFF`. |
| `ci-scripts/conf_files/nrue.uicc.slice2.conf` | **new** | Single-UE convenience config on the second slice. |
| `ci-scripts/conf_files/nrue.uicc.2slice.conf` | **new** | Two-UE convenience config. **Not used by the 5-UE campaign.** |

The MAC-side change is **not** behind any compile flag, but it delegates to stock proportional fair
whenever no policy is installed, so a build without `--build-e2` is behaviourally identical to
upstream OAI. The E2 control path requires `-DE2_AGENT=ON` (default `OFF`).

### FlexRIC additions and fixes

`$OAI/openair2/E2AP/flexric`, submodule base revision `340c36bc`.

| Path | Status | Role |
|---|---|---|
| `examples/xApp/c/rc_slice_ctrl/xapp_rc_slice_ctrl.c` | **new**, 573 lines | The RC actuator. Builds the Style 2 / Action 6 message; runs one-shot (`--policy-file … --once`) or persistent (`--serve <socket>`). |
| `examples/xApp/c/rc_slice_ctrl/CMakeLists.txt` | **new** | Build target `xapp_rc_slice_ctrl`. |
| `examples/xApp/c/monitor/xapp_kpm_moni.c` | modified, +150 | Adds a SQLite sink (`kpm_measurements_raw`) and fixes the indication decoder (records were indexed by *label* instead of by *measurement*, an out-of-bounds read for multi-label measurements). |
| `examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni.py` | rewritten, +234/−161 | Subscribes MAC/RLC/PDCP/GTP, applies the 50 ms RIC-side throttle, writes via `ric_sm_db_writer`, survives decode errors, clean SIGTERM shutdown. |
| `src/lib/msg_hand/reg_e2_nodes.c` | modified | A duplicate E2 SETUP for an already-registered node aborted the RIC on an assertion; the stale entry is now extracted, freed and replaced. |
| `src/sm/pdcp_sm/ie/pdcp_data_ie.c` | modified | Removed four fatal counter-invariant asserts. This code runs **inside the gNB**, so the assert aborted the RAN under sustained traffic. |
| `src/xApp/sync_ui.{c,h}` | modified | New `prepare_sync_ui()` fixes a lost-wakeup race; `cond_wait_sync_ui()` returns `bool` instead of asserting on timeout. |
| `src/xApp/e42_xapp.c` | modified | Calls `prepare_sync_ui()` before every send; timeouts release the procedure and report failure. |
| `src/xApp/msg_handler_xapp.c` | modified | Late CONTROL ACK/FAILURE are ignored instead of aborting; the redundant 10 s pending-event timerfd is removed from the CONTROL path (it corrupted the bimap after tens of thousands of controls). |
| `src/xApp/db/sqlite3/sqlite3_wrapper.c` | modified | Removed a 32-bit CHECK on cumulative RLC SDU byte counters; bounded BUSY/LOCKED retry (50 × 10 ms); 5 s busy timeout; real error text on failure. |

> **None of the FlexRIC edits touch the RC service-model encoder.** The new control style rides on
> stock FlexRIC RC SM machinery.

### The LLM-hRIC Python package

`$OAI/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/`

| Module | Role |
|---|---|
| `config.py` | `load_config`, `normalize_sd`, `slice_key`. **`config.yaml` is parsed with `json.load` — it is JSON despite the extension.** |
| `config.yaml` | The single deployed configuration (JSON). See [Configuration reference](#configuration-reference). |
| `schema.py` | 23 tables + 30 indexes; `connect_db` (WAL, `synchronous=NORMAL`, 5 s busy timeout); concurrency-tolerant column migration. |
| `policy.py` | `validate_prb_policy` (0 ≤ min ≤ dedicated ≤ max ≤ 100, Σdedicated ≤ 100) and atomic `write_prb_policy_file` (tmp → fsync → `os.replace`). |
| `rc_actuator.py` | Unix-socket client for the persistent actuator; verifies the echoed `request_id`, measures `client_latency_ms`. |
| `a1_policy_server.py` | A1-like `ThreadingHTTPServer` on `127.0.0.1:8088`; versioned, atomically activated policies and intents. |
| `llm_client.py` | Provider abstraction: `mock`, OpenAI-compatible HTTP, local HuggingFace `transformers`. Tolerant JSON extraction (strips fences, recovers the first object, closes containers truncated by the token cap). |
| `llm_guidance_service.py` | **The rApp.** 10 s scheduler + 200 ms intent poll, window builder, prompt builder, bound projection, A1 publication, fail-safe hold. |
| `ai_policy.py` | Ratio/bounds validation, `project_prb_ratios`, intent regexes. |
| `ddpg_rc_agent.py` | Actor/twin critics, replay, running normalization, reward, the 24-D state vector, **and the 100 ms controller loop** (`run_controller`). |
| `ddpg_async_learner.py` | The learner process: replay ingest, gradient steps, the six-gate promotion check, snapshot publication. |
| `async_ddpg.py` | SQLite hand-off helpers, `SnapshotPublisher`, `ActorSnapshotWatcher`, `config_fingerprint`. |
| `ric_sm_db_writer.py` | Raw-table ingest, 50 ms windowing, throughput derivation, RNTI→UE→S-NSSAI discovery. |
| `gui_state.py` | `upsert_a1_policy_summary` for the dashboard. |
| `monitor_bridge.py` | **Legacy / dev only.** Not launched by `run_e2e_rfsim.sh`; produces no rows in campaign runs. |
| `run_e2e_rfsim.sh` | The stack launcher. Subcommands: `start`, `gui`, `stop`, `cleanup`, `status`, `logs`, `check-pdcp-plugin`. |
| `sample_prb_policy.json` | Reference policy file (80/20 split) accepted by the actuator. |
| `grafana/` | `docker-compose.yml`, SQLite datasource, and the `LLM-hRIC Runtime Monitor` dashboard. |
| `tests/test_services.py` | ~2100 lines of unit tests pinning state layout, normalization mask, RNTI identity, replay hand-off, snapshot verify/swap/freeze, projection math. |
| `README.md` | The internal ~1600-line operational runbook. Deeper than this file; read it when something breaks. |
| `OAI_MODIFICATIONS_AND_COMPLIANCE.md` | Chinese-language implementation notes. Useful context, **not authoritative** — verify against source. |

### The experiments package

`llm_hric/experiments/`

| Module | Role |
|---|---|
| `experiment_runner.py` | Campaign driver: preflight, stack restart, calibration, training, frozen evaluations, validity gates, manifests, `--eval-checkpoint` mode. |
| `five_ue_traffic_scenarios_v5.json` | The campaign spec used for the reported results (sha256 `7f1d5c03…`). |
| `traffic_controller.py` | iperf3 UDP clients/servers per UE; for dynamic scenarios, a tc-HTB shaper on the ext-DN egress with per-UE classes and per-segment byte-counter validation. |
| `channel_controller.py` | Optional dynamic TDL-A channel control over the OAI telnet server. |
| `analyze_results.py` | Post-hoc metric extraction into `run_metrics.csv` and plots. |
| `eval_checkpoint.py` | Offline deterministic-policy replay against an archived run database. |

### This paper directory

`doc/llm_hric_slicing_demo_paper/`: `main.tex`, `sections/`, `figures/`, `references.bib`,
`generated/` (committed CSV/TeX artifacts), `scripts/build_results.py` (results → TeX),
`scripts/check_sources.py` (structural lint), `tests/`, `Makefile`.

---

## Requirements

### Reference host

These are the values probed on the testbed host; they are the configuration the reported results
came from, not a hard minimum.

| Component | Value |
|---|---|
| OS | Ubuntu 22.04.5 LTS, kernel 6.5.0-18-generic |
| CPU | 13th Gen Intel Core i7-13650HX, 14 cores / 20 threads |
| RAM | 31 GiB |
| GPU | NVIDIA GeForce RTX 4060 Laptop, 8188 MiB, driver 580.173.02 |
| Toolchain | gcc/g++ 10.5.0, cmake 3.22.1, ninja, SWIG 4.1.1 |
| Python | Anaconda **base** at `/home/ics1/anaconda3` (the launcher aborts if `sys.prefix` differs) |
| PyTorch | 2.7.0+cu118 (a separate probe recorded 2.11.0+cu130 for the rApp environment — see TODO 6) |

> **TODO 1 (authors):** the ~8 GB GPU hosts the Gemma rApp *and* the learner. If you intend others to
> reproduce on a smaller GPU, state the minimum VRAM you have actually tested. We have not.

### System packages and services

- **Docker + Docker Compose** for the 5G core (`ci-scripts/yaml_files/5g_rfsimulator/docker-compose.yaml`,
  services `mysql oai-amf oai-smf oai-upf oai-ext-dn`) and optionally Grafana.
- **iproute2 with network-namespace support.** Each UE runs in its own namespace `ue1..ue5`, created
  by `tools/scripts/multi-ue.sh`. Each namespace gets an `oaitun_ue1` TUN device after PDU session
  establishment; preflight fails without it.
- **iperf3** inside the ext-DN container and inside each UE namespace.
- **sqlite3** CLI for inspection.
- `libsctp-dev` (FlexRIC links `-lsctp`), `libsqlite3-dev`.

### Privileges (be explicit about this)

The RAN processes and the traffic generator **require root**:

- creating and entering network namespaces (`ip netns exec ue1 …`);
- creating TUN devices for the UE PDU sessions;
- installing the tc-HTB qdisc/classes/filters for dynamic scenarios;
- binding the gNB's N2/N3 sockets.

`run_e2e_rfsim.sh` wraps these in `sudo` by default (`USE_SUDO=1`). **The campaign runner refreshes
sudo credentials against the controlling terminal**, so a detached `nohup` run will fail once the
credential cache expires. Run campaigns inside `tmux` and call `sudo -v` first, or run the whole
thing as root with `USE_SUDO=0`.

### Python packages

```bash
/home/ics1/anaconda3/bin/python -m pip install \
  torch transformers accelerate bitsandbytes sentencepiece matplotlib pytest
```

`torch`/`transformers`/`accelerate`/`bitsandbytes`/`sentencepiece` are needed only for the local
`transformers` LLM provider. For offline smoke tests set `llm.provider` to `mock` (but note the
campaign config sets `require_real_model: true`, which makes `mock` a hard error).

The Gemma checkpoint must be present locally — `local_files_only: true` means no network fetch:

```
~/.cache/huggingface/hub/models--google--gemma-4-E2B-it/
```

Preflight checks for exactly this directory, and the campaign config additionally sets
`require_real_model: true` with no fallback, so the rApp **refuses to start** without it.

> **⚠️ TODO(authors) — blocking for third-party reproduction.**
> The cache directory above encodes the Hugging Face repo id `google/gemma-4-E2B-it`,
> which does **not** resolve to a public repository (the closest published model is
> `google/gemma-3n-E2B-it`). As shipped, nobody outside the original host can obtain
> these weights, and therefore nobody can run the guidance path. Before release,
> record the true upstream checkpoint identifier and revision hash here, adjust
> `llm.model` / `llm.processor_model` in `config.yaml` accordingly, and state the
> expected file digests. If the deployed weights cannot be redistributed, document
> the `mock` provider as a supported degraded mode and publish which results, if any,
> it can reproduce.

---

## Build

Adjust `/home/ics1/openairinterface5g` to your checkout path.

### 1. OAI gNB, nrUE and the E2 agent

```bash
# First time on a fresh machine (also installs OAI build dependencies).
cd /home/ics1/openairinterface5g/cmake_targets
./build_oai -I --gNB --nrUE --build-e2 --ninja
```

Rebuild without dependency installation (and with ccache disabled, which avoids
`/run/user/.../ccache-tmp` permission errors):

```bash
cd /home/ics1/openairinterface5g/cmake_targets
CCACHE_DISABLE=1 ./build_oai --ninja --gNB --nrUE --build-e2 --build-tool-opt '-j2'

cd /home/ics1/openairinterface5g
CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build \
  nr-softmodem nr-uesoftmodem params_libconfig rfsimulator
```

Two traps that cost real debugging time:

- `nr-softmodem` and `nr-uesoftmodem` `dlopen` `libparams_libconfig.so` and `librfsimulator.so` at
  runtime. Building only the two executables after a clean leaves binaries that start and exit
  before reading their config. Always build all four targets.
- Building an optional library **without** `--build-e2` can reconfigure `E2_AGENT` back to `OFF` in
  `CMakeCache.txt`. Verify before every campaign:

```bash
grep -E 'E2_AGENT|E2AP_VERSION|KPM_VERSION' \
  /home/ics1/openairinterface5g/cmake_targets/ran_build/build/CMakeCache.txt
# expect: E2_AGENT:STRING=ON, E2AP_VERSION:STRING=E2AP_V2, KPM_VERSION:STRING=KPM_V2_03
```

Optional, only for the dynamic TDL-A channel campaign:

```bash
cd /home/ics1/openairinterface5g/cmake_targets
CCACHE_DISABLE=1 ./build_oai --ninja -c --gNB --nrUE --build-lib telnetsrv
# → libtelnetsrv.so, libtelnetsrv_5Gue.so in cmake_targets/ran_build/build
```

### 2. FlexRIC, the nearRT-RIC and the C xApps

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
cmake -S . -B build -DXAPP_MULTILANGUAGE=ON
CCACHE_DISABLE=1 cmake --build build --target \
  nearRT-RIC xapp_rc_slice_ctrl xapp_kpm_moni pdcp_sm xapp_sdk -j2
```

> **`-DXAPP_MULTILANGUAGE=ON` is mandatory and the internal runbook omits it.** FlexRIC declares the
> option `OFF` by default, and `src/xApp/CMakeLists.txt` guards `add_subdirectory(swig)` behind it.
> Without it there is no `xapp_sdk` target and no
> `build/examples/xApp/python3/_xapp_sdk.so` — and `run_e2e_rfsim.sh` hard-requires that file plus
> `build/src/xApp/libe42_xapp_shared.so`, so the Python SM monitor cannot run.

Then validate the PDCP SM plugin actually selected at runtime:

```bash
USE_SUDO=0 examples/xApp/python3/llm_hric/run_e2e_rfsim.sh check-pdcp-plugin
readlink -f /tmp/llm_hric/e2e_rfsim/sm/libpdcp_sm.so
```

This prints the absolute path and SHA256 of the plugin, and refuses to proceed if it still contains
any of the four fatal packet/byte counter assertions. It must resolve to your local FlexRIC build,
not a stale `/usr/local/lib/flexric` copy.

### Pinned build configuration

Verified in `build/CMakeCache.txt` for the reported runs:

| Key | Value |
|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` |
| `E2AP_VERSION` / `E2AP_ENCODING` | `E2AP_V2` / `ASN` |
| `KPM_VERSION` | `KPM_V2_03` |
| `SM_ENCODING_RC` / `SM_ENCODING_KPM` | `ASN` |
| `SM_ENCODING_MAC/RLC/PDCP/GTP/SLICE/TC` | `PLAIN` |
| `BUILDING_LIBRARY` | `STATIC` |
| `XAPP_MULTILANGUAGE` | `ON` |

Installed SM plugins in `/usr/local/lib/flexric`: `libgtp_sm.so libkpm_sm.so libmac_sm.so
libpdcp_sm.so librc_sm.so librlc_sm.so libslice_sm.so libtc_sm.so`.

---

## Quick start

### 1. Bring up the stack

```bash
sudo -v
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric

UE_MODE=multi UE_COUNT=5 \
START_LLM_HRIC=1 START_GUIDANCE=1 START_DDPG=1 START_KPM_MONITOR=1 START_GRAFANA=1 \
./run_e2e_rfsim.sh start
```

Launch order, waits and log locations are all fixed by the script: 5G core (up to
`CORE_WAIT_TIMEOUT_S=120`) → nearRT-RIC (`RIC_START_WAIT_S=3`) → gNB (`GNB_START_WAIT_S=15`) → 5 UEs
(`UE_START_WAIT_S=20`) → RC actuator → SM monitor → KPM monitor → A1 server → rApp → controller.
Logs land in `/tmp/llm_hric/e2e_rfsim/logs/`, PIDs in `/tmp/llm_hric/e2e_rfsim/pids/`.

The five UE configurations are **generated at launch** into
`/tmp/llm_hric/e2e_rfsim/ue_confs/nrue.uicc.ue{1..5}.conf` with IMSIs
`208990100001100`–`208990100001104`. UE1–UE3 get DNN `oai` / SD `0xFFFFFF`; UE4–UE5 get DNN
`openairinterface` / SD `0x123456`. Each runs in namespace `ue<N>` against RFSimulator server
address `10.20<N>.1.100`.

Other subcommands: `stop`, `cleanup`, `status`, `logs`, `gui`, `check-pdcp-plugin`.

Key environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `UE_COUNT` | `5` | >1 selects `UE_MODE=multi` and generates per-UE configs |
| `START_GUIDANCE` | `1` | start the A1 server + Gemma rApp |
| `START_DDPG` | `1` | start the near-RT controller |
| `START_KPM_MONITOR` | `1` | start the E2SM-KPM audit collector |
| `START_LLM_HRIC` | `0` | start the SM monitor xApp (needed for any closed loop) |
| `START_GRAFANA` | `0` | bring up the Grafana container |
| `DDPG_MODE` / `DDPG_ARM` | `deploy` / `llm_guided_ddpg` | controller mode and arm |
| `DDPG_APPLY` | `0` | `1` actuates; `0` is shadow mode (decisions recorded, not applied) |
| `DDPG_CHECKPOINT` | `/tmp/llm_hric/ddpg_prb_v4.pt` | checkpoint to serve in deploy mode |
| `LLM_INTENT` | `prioritize slice 0xffffff while keeping slice 0x123456 above 30 Mbps` | boot-seed intent |
| `USE_SUDO` | `1` | set `0` when running the script as root |
| `RFSIM_DYNAMIC_CHANNEL` | `0` | `1` enables TDL-A + telnet channel control |

### 2. Verify readiness

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
PYTHONPATH="$PWD" /home/ics1/anaconda3/bin/python -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json --preflight
```

Preflight asserts, among other things: exactly 5 UEs in the catalog with unique identities; an
`oaitun_ue1` in every namespace; the RC actuator socket exists; the A1 server answers on
`127.0.0.1:8088`; `nvidia-smi` works and PyTorch sees CUDA; `torch/transformers/accelerate/bitsandbytes`
are importable; the local Gemma snapshot exists; the monitor DB has **exactly 5 mapped UEs**, fresh
summaries (<5 s), fresh MAC raw data (<5 s), fresh KPM data (<15 s), and a runtime RNTI/UE/slice
mapping that matches the configured one. Add `--allow-no-gpu` to skip the GPU checks.

Manual liveness check on the raw tables:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select 'mac' sm, count(*) rows, max(ts_ms) latest_ts from mac_ue_stats_raw
   union all select 'rlc', count(*), max(ts_ms) from rlc_rb_stats_raw
   union all select 'pdcp', count(*), max(ts_ms) from pdcp_rb_stats_raw
   union all select 'gtp', count(*), max(ts_ms) from gtp_tunnel_stats_raw;"
```

### 3. Post an intent

```bash
curl -X POST http://127.0.0.1:8088/a1-p/intents/slice-prb-intent \
  -H 'Content-Type: application/json' \
  -d '{"intent":"prioritize slice 0x123456 while keeping slice 0xffffff above 20 Mbps","valid_for_ms":1000}'
```

The rApp polls the active intent every 200 ms and re-runs inference on any version change without a
restart. Confirm the published policy:

```bash
sqlite3 /tmp/llm_hric/llm_hric.sqlite3 \
  "SELECT policy_id, version, preferred_ratio_json, llm_action_json
   FROM a1_policy_summary WHERE active=1;"

sqlite3 /tmp/llm_hric/llm_hric.sqlite3 \
  "SELECT ts_ms, model, prompt_hash, guidance_json FROM llm_guidance ORDER BY ts_ms DESC LIMIT 1;"
```

Verify the provider actually resolved to the local model (must print `TransformersClient`):

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
/home/ics1/anaconda3/bin/python -c \
"from llm_hric.config import load_config; from llm_hric.llm_client import make_llm_client; \
print(type(make_llm_client(load_config('/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/config.yaml'))).__name__)"
```

### 4. Generate traffic

The campaign runner drives traffic itself. For a manual demo, one UDP server per UE inside its
namespace and one client from the ext-DN container:

```bash
sudo ip netns exec ue1 iperf3 -s -B 12.1.1.2 -p 5201 -i 1 --forceflush > /tmp/ue1_iperf_server.log 2>&1 &
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.1.2 -p 5201 -u -b 30M -t 600 -i 1
```

The stock rfsim core only routes `12.1.1.0/24`; add the second DNN route once:

```bash
docker exec rfsim5g-oai-ext-dn ip route replace 12.1.2.0/24 via 192.168.72.134 dev eth0
```

### 5. Observe

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose up -d
```

Open `http://127.0.0.1:3000` → `LLM-hRIC Runtime Monitor` (500 ms refresh). It shows the active
intent, the active A1 policy summary, DL throughput per UE with an RNTI/S-NSSAI mapping table, and
the latest applied PRB ratios. It is monitoring-only — it never submits policies or changes control
state.

Ground truth straight from the gNB log:

```bash
grep -E 'RC CONTROL rx|RC slice policy|slice_prb' /tmp/llm_hric/e2e_rfsim/logs/gnb.log | tail
# [E2-Agent]: RC CONTROL rx, RIC Style Type 2, Action ID 6
# RC slice policy SST 1 SD ffffff min 0 dedicated 80 max 100
# slice_prb sst=1 sd=ffffff dedicated=80 used=<cumulative PRBs>     (every 100 frames ≈ 1 s)
```

---

## Reproducing the paper results

### Honest status of the reported results

**The published numbers are a single-seed pilot, not a completed campaign.** The full design is
3 scenarios × 3 arms × 5 seeds = **45 runs**. The artifact contains **3**: scenario `balanced`,
**seed 1**, all three arms, each validated `primary`. One further `ddpg_only` seed-1 run is recorded
as excluded (`status_not_complete`). No statistical superiority claim can be made from this, and the
paper does not make one. The paper's `\ifFullCampaign` conditional gates the campaign-wide tables and
is currently false.

### Cost

| Unit | Wall clock |
|---|---|
| One run (nominal phase budget) | 4090 s = 68 min |
| One run (measured pilot) | **85–88 min** (5113–5276 s) |
| Full 45-run campaign | **≈ 65 h** of continuous testbed time, plus stack restarts |

The ~27 % overhead over the nominal budget is A1/rApp policy regeneration during the 64 intent
switches per run (2 calibration + 60 training blocks + 2 evaluations), each waiting on the 10 s rApp
period. The internal runbook quotes ~95 min/run and 10–12 h per full seed; treat those as the
planning figures and 85–88 min as the measured pilot figure.

### Run the campaign

Run inside `tmux`. A detached `nohup` run **will** fail when sudo credentials expire.

> **⚠️ Put `RESULTS` on persistent storage — never under `/tmp`.**
> Each run writes a 1.0–1.3 GB SQLite database plus its trained checkpoints into
> `$RESULTS`, and `/tmp` is cleared on reboot on most distributions. This is not
> hypothetical: the three pilot run databases and every trained agent behind the
> published numbers were destroyed exactly this way, when the host had to be
> rebooted to repair an unrelated NVIDIA driver/library version mismatch. The
> committed derived metrics under `doc/llm_hric_slicing_demo_paper/generated/`
> are all that survived, so those results can be re-read but never re-derived.
> While a campaign is running, also consider pinning the GPU driver packages
> (`sudo apt-mark hold` on the `nvidia-driver*`/`nvidia-dkms*` packages) so an
> unattended upgrade cannot force the reboot in the first place.

```bash
tmux new -s v5
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS="$HOME/llm_hric_results/five-ue-traffic-v5"   # persistent, NOT /tmp
mkdir -p "$RESULTS"; sudo -v

# Smoke first: one scenario, one arm (~90 min).
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results "$RESULTS" --manage-stack --seed 1 --resume --fail-fast \
  --scenario balanced --arm ddpg_only \
  2>&1 | tee "$RESULTS/smoke.log"

# Then widen scope; --resume skips every completed (scenario, arm, seed).
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results "$RESULTS" --manage-stack --seed 1 --resume --fail-fast \
  2>&1 | tee "$RESULTS/seed1.log"
```

`--manage-stack` is **not optional for a valid campaign**: it is the only path that tears down and
relaunches the RFSimulator stack and deletes the shared `/tmp/llm_hric/llm_hric.sqlite3` (plus
`-wal`/`-shm` and the actuator socket) before each arm/seed. Without it, runs append to whatever
database is already there — and one validity gate counts distinct UEs across the *whole* database
with no run filter, so results become unreliable.

`--resume` is idempotent per `(scenario, arm, seed)` keyed on archived manifests; a *failed* run is
not treated as complete and is retried. `--fail-fast` aborts after archiving the first failure.

### Per-run protocol

At the 100 ms control period, each run executes exactly:

| Phase | Duration | Steps | Train | Apply |
|---|---|---|---|---|
| settle | 30 s | — | — | — |
| calibration | 100 s | 1 000 | no | yes |
| training | 3600 s | 36 000 (60 blocks × 600, intent alternating) | yes | yes |
| intent1_eval | 180 s | 1 800 | no (frozen) | yes |
| intent2_eval | 180 s | 1 800 | no (frozen) | yes |
| **total counted** | | **40 600** | | |

Runs are rejected if any per-phase transition count differs from these values. An additional
`intent_switch` phase runs between blocks while the A1 policy is regenerated; those steps are logged
and replayed but are excluded from the validated budget (≈ 8.7k–9.3k per run).

### Analyse and regenerate the paper tables

```bash
/home/ics1/anaconda3/bin/python -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" --output "$RESULTS/analysis"

cd /home/ics1/openairinterface5g/doc/llm_hric_slicing_demo_paper
make results     # scripts/build_results.py --results $RESULTS --spec $SPEC --output generated
make check       # scripts/check_sources.py
make pdf
```

`build_results.py` reads only `manifest.json` files. It excludes runs with `status != complete` or
`validation.data_quality != primary` *before opening any database*, then keeps the latest run per
`(scenario, arm, seed)` and records the losers as superseded. Every exclusion is written to
`generated/artifact_manifest.json`. When all 45 keys are present it sets `\FullCampaigntrue`, which
un-gates the campaign-wide tables.

> **TODO 2 (authors) — result-generator integrity gap.** `discover_runs` never consults the manifest
> `mode` field. An `--eval-checkpoint` run writes `"arm": "<arm>"` with `"mode": "eval_checkpoint"`,
> validates against the reduced phase set, can legitimately reach `status=complete` /
> `data_quality=primary`, and — being newer — would **supersede the real training run** in the same
> results directory. Its metrics row would silently have no training phase. Fix by filtering
> `payload.get("mode") not in (None, "experiment")` in `discover_runs`, or by adding `mode` to
> `run_key`. Until then, **always use a dedicated `--results` directory for eval-checkpoint runs**
> (the runner advises this but does not enforce it).

> **TODO 3 (authors) — archive the run databases.** The three pilot databases (1.03–1.33 GB each)
> lived under `/tmp/llm_hric/experiments/five-ue-traffic-v5/` and **no longer exist on the testbed
> host**. Only `doc/llm_hric_slicing_demo_paper/generated/` survives. The pilot is therefore not
> currently reproducible *from data*. Archive future run directories outside `/tmp` and publish or
> deposit them alongside the paper.

> **TODO 4 (authors) — `kpm_dl_prb_utilization` is mis-scaled.** `analyze_results.py` divides the
> summed per-UE `RRU.PrbTotDl` by `cell_prbs = 106`, but that KPM measurement is already an integer
> *percentage*. The correct divisor is 100. Reported values 0.5096 / 0.5125 / 0.5449 correspond to
> true utilisations 0.540 / 0.543 / 0.578. Either fix the divisor and regenerate, or do not cite this
> metric.

> **TODO 5 (authors) — arm naming is inconsistent across artifacts.** `performance_table.tex` and
> `campaign_table.tex` say "LLM-guided TD3 / LLM only / TD3 only"; `guided_vs_ddpg_table.tex` says
> "DDPG-only (TD3-style)"; the prose says "guided / DDPG-only". Pick one convention in
> `ARM_LABELS` (`scripts/build_results.py`) and state the mapping from the code arm ids
> `llm_only` / `ddpg_only` / `llm_guided_ddpg` once.

---

## Reusing a trained agent

Every learning run archives its agent as `ddpg.pt` in the run directory: Actor, twin critics, all
three targets, both Adam states, state and reward normalization statistics, the replay buffer, the
learner counters, and the Python/PyTorch RNG states. Loading enforces exact `model_version` (4),
`state_feature_version` (3), `replay_schema_version` (3), `state_dim` and `action_dim`; any mismatch
is a hard failure.

### Offline deterministic evaluation (no RAN stack)

```bash
RUN=$RESULTS/balanced/<run-directory>
/home/ics1/anaconda3/bin/python -m llm_hric.experiments.eval_checkpoint \
  --checkpoint "$RUN/ddpg.pt" --db "$RUN/llm_hric.sqlite3" \
  --phase training --limit 20000 --plot "$RUN/policy_response.png"
```

Options: `--phase {training,calibration,all}`, `--limit N` (default 20000), `--config`, `--plot`.

It reads valid transitions from `ddpg_replay_transitions`, runs the Actor deterministically (no
exploration noise), re-applies each row's recorded `low`/`high`/`teacher`/`execution_weight`, and
reports: mean and p95 of |deterministic − live commanded| in PRBs, Actor saturation, and per-intent
allocation mean/std/min/max, mean critic Q and mean recorded reward. `--plot` writes a per-intent
allocation histogram with reference lines at 45 % and 55 %.

### Frozen-policy protocol re-evaluation (~12 min, scored)

```bash
sudo -v
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results /tmp/llm_hric/experiments/five-ue-traffic-v5-eval \
  --manage-stack --seed 1 --fail-fast \
  --scenario balanced --arm ddpg_only \
  --eval-checkpoint "$RUN/ddpg.pt" \
  2>&1 | tee /tmp/llm_hric/experiments/five-ue-traffic-v5-eval/evalck.log
```

This forces `ddpg.async_training.enabled = False` (no learner is spawned), takes the synchronous
controller path with `train=False` (no exploration, no gradients), copies the checkpoint into the new
run directory so the source is never modified, and runs settle → calibration → `intent1_eval` →
`intent2_eval`. The manifest records `mode: eval_checkpoint` and the source path; the run id carries
an `-evalck` marker; `--resume` bookkeeping ignores such manifests. Score it with `analyze_results.py`
on the dedicated results directory. **See TODO 2** — use a separate `--results` directory.

### Live inference demo

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 \
START_DDPG=1 DDPG_MODE=deploy DDPG_ARM=ddpg_only DDPG_APPLY=1 \
DDPG_CHECKPOINT="$RUN/ddpg.pt" \
./run_e2e_rfsim.sh start
```

`DDPG_APPLY=0` gives shadow mode. Do not run this while a campaign is active — they share the stack
and the monitor database.

---

## Configuration reference

`llm_hric/config.yaml` — **JSON, not YAML.** Editing it with YAML syntax breaks the rApp.

### Timing and paths

| Key | Default | Meaning |
|---|---|---|
| `db_path` | `/tmp/llm_hric/llm_hric.sqlite3` | Shared SQLite file (WAL, up to 6 concurrent writers) |
| `period_ms.ddpg` | `100` | Near-RT controller tick |
| `period_ms.llm` | `10000` | rApp inference period |
| `monitor.subscription_interval_ms` | `10` | E2 report interval for **all four** internal SMs (only 1/2/5/10 accepted) |
| `monitor.summary_period_ms` | `50` | RIC-side sampling throttle **and** nominal window width |
| `monitor.auto_rnti_from_gnb_log` | `true` | Enable runtime RNTI discovery |
| `monitor.nrue_log_dir` | `/tmp/llm_hric/e2e_rfsim/logs` | Where `nrUE{1..5}.log` are read from (authoritative RNTI source) |
| `monitor.gnb_log` | `…/logs/gnb.log` | Cross-check only, never a source |
| `monitor.rnti_slice_map` | 5 entries | Static fallback when discovery yields nothing |

### A1 and control

| Key | Default | Meaning |
|---|---|---|
| `a1.host` / `a1.port` / `a1.transport` | `127.0.0.1` / `8088` / `http` | A1-like server; non-`http` makes the rApp write SQLite directly |
| `control.policy_id` | `slice-prb-guidance` | A1 policy id |
| `control.policy_file` | `/tmp/llm_hric/current_prb_policy.json` | Atomically rewritten each tick; the actuator re-reads it from disk |
| `control.actuator_socket` | `/tmp/llm_hric/rc_slice_ctrl.sock` | Persistent AF_UNIX actuator; unset falls back to one-shot subprocesses |
| `control.actuator_timeout_s` | `5` | Client socket timeout (**shorter than the ≈6 s C-side E2 CONTROL deadline** — see limitations) |
| `control.min_prb` / `max_prb` | `10` / `90` | Operational envelope in percent → integer PRB interval `[11, 95]` of 106 |

### LLM (rApp)

| Key | Default | Meaning |
|---|---|---|
| `llm.provider` | `transformers` | `mock` \| `openai`-compatible \| `transformers` |
| `llm.model` / `processor_model` | `google/gemma-4-E2B-it` | HF repo id, loaded from local cache |
| `llm.model_class` | `AutoModelForCausalLM` | Overrides the code default |
| `llm.device_map` | `{"": 0}` | Whole model on GPU 0 |
| `llm.torch_dtype` / `quantization` | `float16` / `4bit` | bitsandbytes NF4 + double quantization, fp16 compute |
| `llm.temperature` | `0.0` | ⇒ `do_sample=False`, greedy decoding |
| `llm.max_new_tokens` | `256` | Generation cap (why the JSON extractor repairs truncation) |
| `llm.max_time_s` | `30` | Wall-clock generation cap |
| `llm.local_files_only` | `true` | Fully offline load |
| `llm.allow_fallback` / `require_real_model` | `false` / `true` | No mock substitution; a load failure is fatal |
| `llm.intent_poll_ms` | `200` | Intent-change poll |
| `llm.state_window_ms` / `state_window_count` | `10000` / `3` | Three ordered, non-overlapping windows, oldest first |
| `llm.guidance_bound_tolerance_prb` | `10` | ±envelope derived around returned ratios (then replaced by calibrated bounds) |
| `llm.priority_min_gap_prb` | `0` | Accepted but **unused** in the production path |

### Reward weights

| Key | Default |
|---|---|
| `reward.total_throughput` | `1.0` |
| `reward.priority_throughput` | `0.5` |
| `reward.sla_violation` | `1.0` (constant penalty when the floor is missed) |
| `reward.sla_deficit` | `2.0` |
| `reward.bler` | `0.2` |
| `reward.action_churn` | `0.1` |
| `reward.priority_shortfall` | `1.0` |

The reward is **piecewise**: a satisfied step earns the throughput utility minus the common cost; a
violated step earns *no* utility and additionally the fixed violation penalty. It is discontinuous at
the floor by design.

### RL hyperparameters

| Key | Default | | Key | Default |
|---|---|---|---|---|
| `ddpg.hidden_width` | `128` | | `ddpg.gamma` | `0.95` |
| `ddpg.action_dim` | `1` | | `ddpg.tau` | `0.005` |
| `ddpg.cell_prbs` | `106` | | `ddpg.actor_lr` | `1e-4` |
| `ddpg.batch_size` | `128` | | `ddpg.critic_lr` | `5e-4` |
| `ddpg.replay_size` | `100000` | | `ddpg.actor_update_interval` | `2` (TD3 delay) |
| `ddpg.critic_learning_starts` | `128` | | `ddpg.target_policy_noise` | `0.1` (clip `0.25`) |
| `ddpg.learning_starts` | `512` | | `ddpg.gradient_clip_norm` | `1.0` |
| `ddpg.action_bins` | `10` (balanced sampling) | | `ddpg.exploration_noise_start/end` | `0.2` → `0.02` |
| `ddpg.normalization_freeze_transitions` | `1000` | | `ddpg.actor_saturation_logit_limit` | `2.944` (weight `0.01`) |
| `ddpg.max_state_age_ms` | `1000` | | `ddpg.min_effect_coverage` | `0.8` |
| `ddpg.transition_observation_ms` | `100` | | `ddpg.sla_window` / `sla_fallback_windows` | `30` / `5` |
| `ddpg.max_ddpg_weight` | `0.4` | | `ddpg.guided_weight_update_interval` | `10` |
| `ddpg.bc_full_weight_actor_steps` | `500` | | `ddpg.bc_decay_actor_steps` / `min_bc_weight` | `1000` / `0.1` |
| `ddpg.state_normalization` | `{epsilon 1e-8, clip 5.0, min_std 0.05}` | | `ddpg.reward_scaling` | `{gamma 0.95, clip 10.0}` |

> `ddpg.exploration_noise_decay_steps` is `6000` in `config.yaml` but the runner **overrides** it to
> `round(0.4 × training_steps)` = **14 400** for the v5 spec. The runner likewise overrides
> `cell_prbs`, `model_version`, `control.min_prb`/`max_prb`, `period_ms.ddpg`, `period_ms.llm` and
> `monitor.summary_period_ms` from the spec. Read `<run_dir>/effective_config.json` to see what a run
> actually used.

### Asynchronous learner and promotion gates

| Key | Default | Meaning |
|---|---|---|
| `async_training.enabled` | `true` | Separate learner process |
| `async_training.learner_poll_ms` | `100` | Replay-cursor poll |
| `async_training.ingest_batch_size` | `2048` | Rows per poll |
| `async_training.max_catchup_updates` | `32` | Gradient steps per poll (ratio is 1 update per valid transition) |
| `async_training.min_replay_transitions` | `1000` | Training starts here |
| `async_training.publish_every_actor_updates` | `50` | Candidate cadence |
| `async_training.publish_min_interval_s` | `5` | Candidate cadence |
| `async_training.promotion_holdout_size` | `1024` | Most recent replay rows scored |
| `async_training.max_promotion_saturation` | `0.4` | Gate: fraction of outputs ≤0.05 or ≥0.95 |
| `async_training.max_promotion_q_drop` | `0.02` | Gate: serving_q − candidate_q (critic 1 only) |
| `async_training.max_promotion_action_shift` | `0.2` | Gate: mean executed-action shift |
| `async_training.force_promotion_after_rejections` | `6` | Escape hatch; waives saturation/Q/shift only |
| `async_training.require_initial_snapshot` | `true` | Controller will not act before a validated Actor |
| `async_training.initial_snapshot_timeout_s` | `120` | |
| `async_training.snapshot_poll_ms` | `500` | Watcher period; swap happens at a tick boundary |
| `async_training.checkpoint_every_updates` | `250` | Slim periodic saves (replay omitted) |

Six promotion gates are evaluated in order: finiteness → saturation ≤ 0.40 → Q drop ≤ 0.02 → action
shift ≤ 0.20 → outputs in [0,1] → projection legal on every holdout row. The forced path **never**
waives finiteness, output range, or projection legality.

---

## Tests and validation

```bash
# LLM-hRIC package unit tests (~2100 lines; state layout, normalization mask, RNTI identity,
# replay hand-off, snapshot verify/swap/freeze, projection math, training gate).
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
PYTHONPATH="$PWD" python -m unittest llm_hric.tests.test_services

# Paper-side tests and structural source check.
cd /home/ics1/openairinterface5g/doc/llm_hric_slicing_demo_paper
make test     # python -m unittest discover -s tests -p 'test_*.py'
make check    # scripts/check_sources.py

# Binaries load their runtime libraries.
/home/ics1/openairinterface5g/cmake_targets/ran_build/build/nr-softmodem   --help >/dev/null
/home/ics1/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem --help >/dev/null
/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/ric/nearRT-RIC -h >/dev/null || true
```

`make check` (`scripts/check_sources.py`) verifies LaTeX brace/environment balance, BibTeX key
presence, and the existence of the generated `.tex` files. It does **not** validate any number in the
prose against `generated/*.csv`.

> **TODO 6 (authors):** add a numeric-assertion test that greps the results section for hard-coded
> literals and compares them to `generated/run_metrics.csv` within tolerance, and a
> `build_results.py` test for eval-checkpoint isolation (TODO 2). Neither exists today.

Runtime validity gates enforced by the campaign runner (a failure marks the run `failed` and excludes
it): exact per-phase step counts; apply success ≥ 0.99; mapped UEs; ≥ 95 % of transitions with effect
coverage ≥ 0.8; RC control latency p99 < 20 ms; controller tick jitter p95 ≤ 120 ms / p99 ≤ 150 ms;
fresh-state action interval p95 ≤ 180 ms; stale-skip rate ≤ 0.5 (i.e. 50 % of ticks); traffic event success ≥ 0.99;
dynamic segment success ≥ 0.95. `five_ue_coverage < 0.90` marks the run `degraded` (non-fatal, but
excluded from the paper tables).

> **TODO 7 (authors):** the spec keys `max_fresh_action_interval_p99_ms` (300) and
> `max_summary_age_ms` (3000) are **computed/declared but never enforced** — the runner only gates the
> p95 interval. Either add the assertions or document them as diagnostics.

---

## Troubleshooting

### 1. GPU works but PyTorch cannot see CUDA after an unattended NVIDIA upgrade

An automatic driver upgrade replaces `libcuda.so` while the running kernel module stays at the old
version, producing `Failed to initialize NVML: Driver/library version mismatch` or a preflight
failure at *"PyTorch in the selected Python environment cannot access CUDA"* even though `nvidia-smi`
looks fine. Reboot (or reload `nvidia*` kernel modules) before starting a campaign, and re-check:

```bash
nvidia-smi
/home/ics1/anaconda3/bin/python -c 'import torch; print(torch.__version__, torch.cuda.is_available())'
```

Never start a 65-hour campaign without confirming this — the rApp fails hard
(`require_real_model: true`, `allow_fallback: false`) rather than silently degrading.

### 2. `sudo: a terminal is required to read the password` in a detached run

The launcher and runner refresh sudo credentials against the controlling terminal. Use `tmux`:

```bash
sudo -v
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```

Or run as root and disable internal sudo wrapping:

```bash
sudo -E env UE_MODE=multi UE_COUNT=5 USE_SUDO=0 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 \
  ./run_e2e_rfsim.sh start
```

### 3. gNB dies with `check_pdcp_bearer: Assertion 'rb->txpdu_pkts <= rb->txpdu_bytes' failed`

A stale `libpdcp_sm.so` is loaded. That check runs inside the gNB's SM agent plugin, so it aborts the
**RAN**, not the xApp. Rebuild and re-validate:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
CCACHE_DISABLE=1 cmake --build build --target pdcp_sm -j2
USE_SUDO=0 examples/xApp/python3/llm_hric/run_e2e_rfsim.sh check-pdcp-plugin
readlink -f /tmp/llm_hric/e2e_rfsim/sm/libpdcp_sm.so
```

It must resolve to your FlexRIC build, not `/usr/local/lib/flexric`.

### 4. `sqlite3.OperationalError: attempt to write a readonly database`, or stale WAL files

Grafana or a sudo-launched monitor can leave `/tmp/llm_hric/llm_hric.sqlite3*` owned by `root` or
`nobody`:

```bash
mkdir -p /tmp/llm_hric
sudo chown "$USER:$(id -gn)" /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
chmod a+rwx /tmp/llm_hric
chmod a+rw  /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
```

If a previous run was killed, delete the database **and** its `-wal`/`-shm` companions before
restarting; a half-written WAL will otherwise be replayed into the new run. `--manage-stack` does
this for you.

### 5. `Device "oaitun_ue1" does not exist` / a UE namespace is missing

The UE never completed PDU session setup, or `nr-uesoftmodem` exited after creating the tunnel:

```bash
grep -Hn "PDU Session Establishment Accept\|TUN Interface\|unknown option\|Exiting" \
  /tmp/llm_hric/e2e_rfsim/logs/nrUE*.log
```

Good lines are `Received PDU Session Establishment Accept, UE IPv4: 12.1.1.2` and
`TUN Interface oaitun_ue1 successfully configured`. An `unknown option` line means an unsupported
flag reached `nr-uesoftmodem` (telnet options are deliberately not passed to the UE). Namespaces are
created by `tools/scripts/multi-ue.sh`; re-run `./run_e2e_rfsim.sh cleanup` then `start`.

### 6. iperf server says `Address already in use`

A server is still listening in that namespace from a previous run:

```bash
sudo ip netns exec ue1 ss -lntup | grep 5201
sudo ip netns exec ue1 pkill -f "iperf3 -s" || true
sudo ip netns exec ue1 iperf3 -s -B 12.1.1.2 -p 5201 -i 1 --forceflush > /tmp/ue1_iperf_server.log 2>&1 &
```

### 7. ext-DN can ping UE1 but not UE4/UE5

The stock rfsim core compose only routes the first DNN:

```bash
docker exec rfsim5g-oai-ext-dn ip route replace 12.1.2.0/24 via 192.168.72.134 dev eth0
docker exec rfsim5g-oai-ext-dn ping -c 3 12.1.2.2
```

### 8. Grafana shows `No data`, `52 years`, or a single unnamed `value` series

`No data` with SQLite open errors is a permissions problem — see item 4. A decades-old age means an
old dashboard treated a missing timestamp as epoch 0; a single unnamed series means the provisioned
dashboard JSON was not reloaded. In both cases restart the container:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose restart grafana
```

Then confirm the database really has per-UE rows:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select sd, ue_id, count(*) rows, round(max(dl_th_mbps),2) max_dl
   from ue_slice_throughput where ue_id != 'slice-total' group by sd, ue_id order by sd, ue_id;"
```

### 9. Controller tick jitter grows without bound in long runs

The controller runs two per-slice queries every 100 ms against tables that grow for the whole run.
Without the composite indexes `network_state(sst, sd, ts_ms)` and
`applied_prb_policy(sst, sd, applied, ts_ms)`, SQLite full-scans them and jitter drifts until the
p95/p99 gates fail. These indexes are in `schema.py`; if you are running against a database created
by an older schema, delete it and let `init_schema` recreate it. Check live:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select phase, count(*) ticks, round(avg(jitter_ms),2) mean_jitter, max(jitter_ms) max_jitter,
          sum(skip_reason is not null) skips from controller_ticks group by phase;"
```

### 10. The actuator dies partway through a very long run

RIC request identifiers come from a monotonically increasing registry that never recycles keys, and
`generate_ric_gen_id()` asserts `req_id < 1 << 16`. At the 100 ms control period a single persistent
actuator process therefore supports at most **65 536 CONTROL procedures ≈ 109 minutes**. The reported
runs are ~87 min, about 80 % of that ceiling. The experiment runner restarts the actuator per run;
any longer continuous operation must do the same.

### 11. `assoc_rb_tree_extract: Assertion 'z_node != tree->dummy' failed` in the actuator

The pre-fix FlexRIC CONTROL path inserted a redundant 10 s timerfd into the pending-event bimap on
every request; removing an already-reaped entry corrupted the container after tens of thousands of
control cycles. Rebuild the actuator against the patched FlexRIC:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
CCACHE_DISABLE=1 cmake --build build --target xapp_rc_slice_ctrl -j2
```

### 12. rApp logs `LLM guidance cycle failed; preserving the last active A1 policy`

The model output could not be parsed even after fence-stripping and truncation repair, or the action
was semantically invalid. **This is the designed fail-safe: no new policy version is written and the
previous active policy remains in force.** Inspect the guidance log for truncated JSON; if it recurs,
`max_new_tokens` (256) may be too small for your prompt, or the model is not following the compact
shape. A `Rejecting invalid LLM action: …` line means the action parsed but violated the catalog,
sum, or bounds constraints.

---

## Citation

```bibtex
% TODO(authors): replace journal/volume/number/pages/doi with the published
% magazine record once available. Keep this entry in sync with
% doc/llm_hric_slicing_demo_paper/references.bib.
@article{bao2026llmhric,
  author        = {Lingyan Bao and Sinwoong Yun and Jemin Lee and Tony Q. S. Quek},
  title         = {{LLM-hRIC}: {LLM}-empowered Hierarchical {RAN} Intelligent Control for {O-RAN}},
  journal       = {IEEE Network},
  year          = {2026},
  note          = {To appear},
  eprint        = {2504.18062},
  archiveprefix = {arXiv},
  primaryclass  = {cs.NI}
}
```

Please also cite this implementation artifact:

```bibtex
@misc{llmhric_slicing_artifact,
  author       = {TODO: author list},
  title        = {{LLM-hRIC} for {O-RAN} Network Slicing on {OpenAirInterface} and {FlexRIC}:
                  Implementation Artifact},
  year         = {TODO},
  howpublished = {TODO: repository URL},
  note         = {Companion implementation artifact and appendix to arXiv:2504.18062.
                  OAI revision cb0e501293a7a4664f09322136d7ff29a39343dc,
                  FlexRIC revision 340c36bc8385dc0b9f5d8b2d51d16ff288acee79},
  doi          = {TODO: Zenodo DOI, if deposited}
}
```

> **TODO 8 (authors):** fill in the author list, year, repository URL and DOI above, and decide
> whether to deposit the run databases (TODO 3) under the same DOI.

---

## License and acknowledgements

This work builds directly on two upstream projects, each carrying its **own** license, confirmed from
the `LICENSE` files in this tree:

| Component | License |
|---|---|
| OpenAirInterface (`$OAI/LICENSE`) | **Collaborative Standards Software License v1.0 (CSSL)** — <https://openairinterface.org/oai-cssl/> |
| FlexRIC (`$OAI/openair2/E2AP/flexric/LICENSE`) | **OAI Public License v1.1** (OpenAirInterface Software Alliance) |

Files we modified in place remain under the license of the file they modify. See also
`$OAI/LICENSES/` for the per-file license texts used across the OAI tree.

> **TODO 9 (authors) — choose a license for your own code.** The genuinely new code — the LLM-hRIC
> Python package, the experiments package, `xapp_rc_slice_ctrl.c`, `rc_ctrl_service_style_2.{c,h}`,
> and the paper tooling — has **no license header today**. Pick one, add `LICENSE` at the repository
> root and SPDX headers to the new files, and verify compatibility with the CSSL / OAI Public License
> for the files that live inside the OAI and FlexRIC trees.

> **TODO 10 (authors) — acknowledgements and funding.** Add the funding statement, institutional
> acknowledgements, and any required disclosure. Also acknowledge the OpenAirInterface Software
> Alliance and the Mosaic5G/FlexRIC team, and Google for the Gemma weights (subject to the Gemma
> Terms of Use, which you should link).

> **TODO 11 (authors) — publishing form.** This code currently lives *inside* an OAI fork with
> **absolute paths hard-coded** in `run_e2e_rfsim.sh` (`/home/ics1/openairinterface5g`),
> `config.yaml` (`control.rc_xapp`, `db_path`, log dirs, `/home/ics1/anaconda3`), and the paper
> `Makefile`. Decide whether to publish (a) a full fork, (b) a patch series against the pinned OAI and
> FlexRIC revisions plus a standalone `llm_hric` package, or (c) both — and parameterise those paths
> before release. Also decide how to ship the site-specific network values
> (`traffic.ext_dn_ip = 192.168.72.135`, `route_gateway = 192.168.72.134`, AMF `192.168.71.132`,
> gNB N2/N3 `192.168.71.129`).

---

## Known limitations

Deliberately stated in full; the paper carries the same list.

**Evaluation**

- **The reported results are a single-seed pilot: 3 of 45 planned runs**, one scenario (`balanced`),
  seed 1, all three arms. No statistical superiority claim is supportable and none is made.
- The three run databases (1.0–1.3 GB each) **and every trained checkpoint** were lost when the host
  was rebooted to repair an NVIDIA driver/library version mismatch, because `RESULTS` pointed under
  `/tmp` (TODO 3). Only the committed `generated/` artifacts survive: derived per-run metrics,
  generated tables and figures, and the run manifests. The published numbers can therefore be
  re-read and re-verified against those files, but not re-derived from raw measurements, the result
  generator cannot be re-run for this pilot, and no trained agent from it can be re-evaluated.
- **Channel-quality features carried no information.** In the RFSimulator AWGN configuration the gNB
  reports a constant zero wideband CQI and a numerically zero BLER, while the validity mask remains
  asserted (it detects absent or out-of-range reports, not degenerate constant ones). Three of the 24
  state features were therefore uninformative. Do not read the CQI/BLER columns as measured radio
  quality.
- The guided arm's policy was effectively **pinned to the guidance boundary**: 90.7 % of guided
  training actions and 92.1 % / 78.2 % of its evaluation actions sat on an edge of the active feasible
  interval, versus 0.34 % / 0 % / 0 % for the unguided arm.
- The count of **forced Actor promotions** in the guided run is not recoverable — the metric is stored
  only in `ddpg_actor_versions.reason` / `metrics_json.forced` inside the lost databases. 62 of 257
  accepted candidates is an *upper bound* on those that passed the gates outright.

**Standards conformance**

- **A1-like, not A1-PMS.** `POST /a1-p/{policies,intents}/{id}` instead of the A1AP
  `PUT /A1-P/v2/policytypes/{id}/policies/{id}`; no policy-type registry and therefore no
  schema-typed policy objects; no `DELETE`, no `/status`, no enforcement feedback, no notification
  callback; no TLS and no authentication (loopback bind only). The `/a1-p/intents/` resource has no
  A1-P counterpart at all. Policy lifetime (`valid_for_ms`) is recorded but never enforced — the last
  activated policy remains authoritative until superseded.
- **SST and SD are transported as ASCII-text OCTET STRINGs** (e.g. `"1"`, `"0xffffff"`), not the 3GPP
  binary octet encoding. An absent SD defaults to `0xffffff`.
- **The PLMN Identity element is required to be present but is never interpreted** by the gNB.
- **A rejected control is still acknowledged as success.** The RIC Control Acknowledge is returned
  unconditionally; a decode or validation failure is visible only as a `LOG_E` line in the gNB log.
  The E2 outcome carries no failure code.
- The KPM audit path persists only the **first** of 768 distribution bins of `CARR.PDSCHMCSDist`,
  unlabelled, and `RRU.PrbTot*` is an integer percentage rather than a count.
- No **NSSF**, no network-slice selection signalling. Slice membership is static: one PDU session and
  one DRB per UE, fixed at UE configuration time.

**Scheduler and control semantics**

- **No cross-slice reclamation.** Each policy gets a per-slot new-data budget of
  `floor(n_rb_avail × dedicated_prb_ratio / 100)` PRBs; unused budget is not redistributed to another
  slice within the pass. Retransmissions and TA/beam-switch MAC CEs are allocated *before* any slice
  budget and are **outside** it, so a slice's realised share can differ from its ratio.
- A policy with `dedicated_prb_ratio == 0` receives **zero** new-data PRBs: its UEs match a policy and
  are therefore also excluded from the unmatched pass.
- Slice budgets are summed over all beams while the allocator is invoked per beam group — exact only
  for `num_beams == 1`, which is the case here.
- At 106 PRB the DCI/CORESET limit permits at most **4 of the 5 UEs per downlink slot**.
- `nr_mac_set_dl_slice_policies()` memsets the table before re-validating each entry, so a MAC-level
  validation failure would wipe the previously active quotas. Unreachable in practice because the E2
  decoder applies identical bounds first and installs nothing on failure.

**Measurement pipeline**

- **RNTI-to-UE/S-NSSAI mapping is reconstructed at runtime from the per-UE `nrUE{i}.log` files**, with
  the gNB log as a consistency check only, and a static `monitor.rnti_slice_map` fallback. The map is
  accepted only all-or-nothing. The gNB cross-check relies on OAI's periodic MAC statistics print,
  which OAI disables above `stats_max_ue` (default **8**) UEs — it works at 5 UEs and silently
  degrades above 8.
- **The PDCP and GTP service models of the monolithic gNB report an RRC UE identity in the RNTI
  field**, which never matches a C-RNTI. Those rows are permanently unresolvable, are stored with null
  slice identity, are excluded from every derived table, and trigger a once-per-second re-scan of up
  to 6 × 8 MiB of log tails. In practice the reported throughput is the **RLC-tier** rate. This is a
  property of the stock OAI E2 agent, not of the controller.
- Derived windows are event-driven, anchored on MAC indication timestamps — contiguous and half-open,
  typically **50–60 ms** wide, not a fixed 50 ms wall-clock grid.

**Operational**

- **The persistent actuator is single-threaded with no per-connection receive timeout**, so a stalled
  client blocks all subsequent PRB actions; and the client socket timeout (5 s) is **shorter** than
  the C-side E2 CONTROL deadline (≈6 s), so a CONTROL timeout would leave the actuator writing to a
  closed socket. FlexRIC never sets `SIGPIPE` to `SIG_IGN`, so that would terminate the actuator.
  Neither condition occurred in the reported runs (100 % of actions were acknowledged).
- **The actuator resolves the connected E2 nodes and the RC RAN function identifier once, at
  start-up.** A node that attaches later is never controlled — the gNB must be up before the actuator.
- The run is **not bitwise reproducible**. Python and torch RNGs are seeded per run in both the
  controller and the learner, and channel-trace seeds are derived as `seed`, `seed+10^4`,
  `seed+2×10^4`; but numpy, `torch.cuda` and cuDNN determinism are not forced, and the wall-clock-driven
  asynchronous learner makes runs statistically rather than bitwise reproducible.
- `config.yaml` is JSON despite its extension. The rApp stores only `sha256(prompt)`, not the prompt
  text, so prompts cannot be replayed from a run database. No LLM inference latency, token count or
  GPU-memory figure is instrumented — only the enforced 30 s generation cap and the 10 s period.

---

## Publishing note

The repository currently mirrors an OAI checkout. Until TODO 11 is resolved, treat `$OAI` in this
document as `/home/ics1/openairinterface5g` and expect to edit absolute paths in
`run_e2e_rfsim.sh`, `llm_hric/config.yaml` and `doc/llm_hric_slicing_demo_paper/Makefile` for any
other host.
