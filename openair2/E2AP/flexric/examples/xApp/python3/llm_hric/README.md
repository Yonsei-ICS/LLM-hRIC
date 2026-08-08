# LLM-hRIC Prototype

This directory contains a minimal LLM-hRIC framework for the OAI/FlexRIC RC slice-control experiment.

## Flow

1. `xapp_mac_rlc_pdcp_gtp_moni.py` subscribes to FlexRIC MAC/RLC/PDCP/GTP SM indications and writes network state into SQLite; `xapp_kpm_moni` additionally stores standard E2SM-KPM measurements for audit.
2. `llm_guidance_service.py` reads recent state and writes LLM guidance.
3. `a1_policy_server.py` exposes an A1-like HTTP API:
   - `POST /a1-p/policies/{policy_id}`
   - `GET /a1-p/policies/{policy_id}`
4. `ddpg_rc_agent.py` reads A1 guidance + fresh network state and sends atomic PRB policies to the persistent `xapp_rc_slice_ctrl` actuator.
5. `grafana/` provides a read-only runtime monitor for LLM intent, A1 policy, UE/slice throughput, and applied PRB actions.

The first version applies only `control_type = "prb"`. Power, MCS, and handover are reserved in the schema for later controllers.

## First-time build

Run these commands once on a fresh machine or after cleaning the build directory. The paths below assume the repository is checked out at `/home/ics1/openairinterface5g`.

Install OAI build dependencies and build the gNB, nrUE, and E2 agent:

```bash
cd /home/ics1/openairinterface5g/cmake_targets
./build_oai -I --gNB --nrUE --build-e2 --ninja
```

Build the optional telnet/channel-control modules before running the dynamic
TDL-A robustness campaign:

```bash
cd /home/ics1/openairinterface5g/cmake_targets
CCACHE_DISABLE=1 ./build_oai --ninja -c --gNB --nrUE --build-lib telnetsrv
```

This produces `libtelnetsrv.so` and `libtelnetsrv_5Gue.so` in
`cmake_targets/ran_build/build`. The static AWGN campaign does not require
these modules.

If the dependency installation step has already been done, or if `ccache` hits `/run/user/.../ccache-tmp` permission errors, rebuild the binaries directly with ccache disabled:

```bash
cd /home/ics1/openairinterface5g/cmake_targets
CCACHE_DISABLE=1 ./build_oai --ninja --gNB --nrUE --build-e2 \
  --build-tool-opt '-j2'

cd /home/ics1/openairinterface5g
CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build \
  nr-softmodem nr-uesoftmodem params_libconfig rfsimulator
```

`nr-softmodem` and `nr-uesoftmodem` load `libparams_libconfig.so` and
`librfsimulator.so` at runtime. Building only the two executables after a clean
can therefore leave binaries that exist but exit before reading their config.
The launcher also requires `E2_AGENT:STRING=ON` in the OAI `CMakeCache.txt`;
building an optional library without `--build-e2` can reconfigure it to `OFF`.

Build FlexRIC, the nearRT-RIC, and the RC slice-control xApp:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
cmake -S . -B build
CCACHE_DISABLE=1 cmake --build build --target \
  nearRT-RIC xapp_rc_slice_ctrl xapp_kpm_moni pdcp_sm -j2

USE_SUDO=0 \
  examples/xApp/python3/llm_hric/run_e2e_rfsim.sh check-pdcp-plugin
```

The last command checks the exact `libpdcp_sm.so` selected for runtime use.
It prints its absolute path and SHA256 and refuses a stale plugin that still
contains one of the fatal packet/byte counter assertions.

Install Python runtime dependencies for LLM-hRIC services:

```bash
/home/ics1/anaconda3/bin/python -m pip install \
  torch transformers accelerate bitsandbytes sentencepiece matplotlib pytest
```

LLM-hRIC uses the Anaconda **base** environment at `/home/ics1/anaconda3`; it does not use `lm-eval`. The launcher fixes `sys.executable`, `CONDA_PREFIX`, `PATH`, user-site behavior, and the dynamic-library path for every Python service, even when the parent terminal has another Conda environment activated. Verify the base runtime with:

```bash
/home/ics1/anaconda3/bin/python -c \
  'import sys,torch,transformers,bitsandbytes; print(sys.executable); print(sys.prefix)'
```

`torch/transformers/accelerate/sentencepiece` are required only when `config.yaml` uses the local HuggingFace `transformers` provider. For offline smoke tests, set `llm.provider` to `mock`.

Optional quick checks:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
/home/ics1/anaconda3/bin/python -m unittest /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/tests/test_services.py

/home/ics1/openairinterface5g/cmake_targets/ran_build/build/nr-softmodem --help >/dev/null
/home/ics1/openairinterface5g/cmake_targets/ran_build/build/nr-uesoftmodem --help >/dev/null
/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/ric/nearRT-RIC -h >/dev/null || true
```

## Example

Terminal 1, start the FlexRIC SM monitor writer:

```bash
mkdir -p /tmp/llm_hric
sudo chown "$USER:$(id -gn)" /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
chmod a+rwx /tmp/llm_hric
chmod a+rw /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true

LD_PRELOAD=/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/xApp/c/monitor/RRC_MESSAGES/libasn1_nr_rrc_shared.so \
XAPP_DURATION=-1 \
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/xApp/python3:/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
/home/ics1/anaconda3/bin/python -u /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni.py \
  --config /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/config.yaml \
  --db-path /tmp/llm_hric/llm_hric.sqlite3
```

Terminal 2, start the A1-like server:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
/home/ics1/anaconda3/bin/python -m llm_hric.a1_policy_server
```

Terminal 3, run one DDPG deployment step:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
/home/ics1/anaconda3/bin/python -m llm_hric.ddpg_rc_agent --mode deploy --once
```

The FlexRIC SM monitor requires nearRT-RIC, gNB, and at least one connected E2 node. FlexRIC xApps connect to the nearRT-RIC E42 endpoint on SCTP `127.0.0.1:36422`; do not use TCP-only tools such as `nc`, `telnet`, or `curl` to decide whether this endpoint is reachable. The monitor uses the FlexRIC SDK to open the SCTP connection directly.

## Five-UE guidance ablation experiment

The reproducible experiment compares `static_equal`, `llm_only`, `ddpg_only`, and `llm_guided_ddpg`. FlexRIC indications are requested every 10 ms, the monitor produces 50 ms observations, DDPG uses an independent 100 ms RIC wall-clock timer, and the rAPP publishes an A1-like policy on an independent 10 s timer. The faster monitor cadence gives every controller tick a fresh-state margin without changing any RAN period. These timers control only RIC observation, inference, and policy publication. They never modify or gate the gNB slot, PHY, MAC scheduler, or any other RAN execution period. If an observation is missing or stale, the controller records and skips that RIC tick while the RAN continues normally.

DDPG uses an asynchronous Actor-Learner layout. The controller process never
calls an optimizer: it reads one fresh state per RIC tick, runs the current
read-only serving Actor, applies bounds/fallback, sends the RC command, and
writes the acknowledged transition to `ddpg_replay_transitions`. A separate
`ddpg_async_learner.py` process consumes valid rows, updates one Critic per new
transition and one Actor per two Critic updates, and periodically validates a
candidate target Actor. Accepted snapshots are SHA256-verified and preloaded
by a watcher thread; the controller swaps the object reference only at a tick
boundary. Rejected candidates or a failed learner do not replace the last
serving Actor. Controller startup does not wait for the learner's first
snapshot: it starts with the safe initial Actor (and the LLM teacher for the
guided arm), while the watcher accepts a validated snapshot in the background.
Evaluation disables training/publication and freezes the Actor version.

Runtime health checks execute in a separate watchdog thread. Intent changes are
also non-blocking: the controller keeps applying the previous active A1 policy
while the rAPP generates the replacement, then observes the newly activated
policy atomically. Intent A prioritizes `1:ffffff` and protects `1:123456`;
Intent B reverses those roles. Transition actions generated during an A1 update
are tagged `intent_switch` and are excluded from frozen evaluation metrics.

Every scheduled RIC tick is audited in `controller_ticks`, including scheduled
and wake-up timestamps, jitter, elapsed periods, state freshness, skip reason,
action timestamp, and policy version. Formal validation reports controller
tick jitter, fresh-state action interval, stale-skip rate, and RC latency as
four separate metrics.

A transition is trainable only when the RC command was acknowledged, active UE mappings and metric provenance are valid, counters did not reset, and observed KPI windows cover at least 80% of the configured post-command observation horizon. This horizon is used only for causal alignment of replay data; it is not an action hold command and does not block the next independent RIC decision tick. The pure-DDPG arm uses the same intent-aware objective but masks the A1 target, bounds, and teacher behavior.

All four arms share a non-LLM operational envelope of 10--90 PRB percent per
slice. This is a RAN liveness constraint, not LLM guidance: allowing an
untrained actor to emit 0/100 starves one slice, makes its UEs repeatedly run
Random Access and change C-RNTI, and invalidates the five-UE comparison. The
formal specification records the envelope as `operational_min_prb` and
`operational_max_prb`. `ddpg_only` still receives no LLM ratio, confidence,
teacher samples, or guidance bounds; `llm_guided_ddpg` intersects its narrower
LLM bounds with the same operational envelope.

Every 10 seconds the rAPP reads three ordered, non-overlapping 10-second
windows. Each window contains the end-of-window UE count and the mean DL
throughput, slice PRB share, and RLC TX buffer occupancy in bytes for each
S-NSSAI. The three windows stay separate in the prompt so Gemma can observe
load and queue trends. `txbuf_occ_bytes` is a gauge and is never differenced;
OAI's `txbuf_occ_pkts` is not implemented and is not used. Incomplete windows
are explicitly marked unavailable rather than filled with zero.

The rAPP resolves the natural-language intent into a machine-readable
`policy_context` containing the priority slice, protected slice, SLA floor,
calibrated cell capacity, and priority minimum ratio. DDPG consumes that
context instead of interpreting natural language in its near-RT loop. Legacy
policies without `policy_context` retain a compatibility parser. SLA is a gate:
while the protected slice is below its floor, positive throughput utility is
disabled and the transition receives only penalties. Once the floor is met,
higher priority-slice throughput increases reward:

```text
if protected_slice_th < sla_floor:
  reward =
    - 1.0
    - 2.0 * normalized_sla_deficit
    - 0.2 * mean_dl_bler
    - 0.1 * normalized_action_churn
else:
  reward =
    1.0 * normalized_total_dl_th
    + 0.5 * normalized_priority_slice_dl_th
    - 0.2 * mean_dl_bler
    - 0.1 * normalized_action_churn
```

Weights are configured under `reward` in `config.yaml` and copied into every run manifest and transition's `reward_components_json`. Both DDPG arms use this same reward; only the guided arm receives the projected A1 target ratio and guidance bounds in its state/action path. LLM confidence remains available for audit but is not an Actor feature.
Both RL arms receive the intent objective itself through shared
`priority_slice_one_hot`, `protected_slice_one_hot`, normalized SLA floor, and
current SLA deficit state features. Pure DDPG still receives no LLM action,
confidence, teacher target, or LLM bounds.

DDPG v4 uses one sigmoid Actor output in `[0,1]`. The output selects Slice
`1:ffffff` inside its feasible integer PRB interval; Slice `1:123456` receives
the remainder of the 106-PRB cell. Replay stores raw state, the acknowledged
commanded Slice-A fraction, raw reward, raw next state, A1 policy version, ACK
timestamp, effect coverage, and metric provenance. Actual PRB use remains a
next-state KPI and is not confused with the commanded quota.
The Actor input has 24 features: eleven per slice plus one normalized SLA
floor and one current SLA deficit. Per-slice features are DL throughput, UE
count, PRB usage share, `log1p` RLC TX buffer bytes, normalized WB-CQI, DL
BLER, channel-valid mask, projected A1 target ratio, currently applied ratio,
and priority/protected one-hot flags. Pure DDPG receives zero in the A1 target
positions. Only throughput, UE count, and transformed buffer occupancy use
checkpointed Welford running mean and variance; bounded ratios, channel values,
masks, and intent flags retain their deterministic scales. The Critic uses reward divided by the running standard deviation of
the discounted return; raw and scaled reward remain available in
`experiment_steps` and `ddpg_actions` for audit.

Older checkpoints are intentionally incompatible with the 24-feature replay
schema. Standalone runs use `/tmp/llm_hric/ddpg_prb_v4.pt`; loading a
checkpoint without `model_version=4`, `state_feature_version=3`, and
`replay_schema_version=3` fails
loudly instead of silently mixing normalized and raw transitions.

The rAPP retains Gemma's raw preferred ratio for audit, then projects it into
the calibrated bounds to produce the single executable A1 target ratio used by
DDPG. There is no fixed priority-gap rewrite. Guided DDPG starts
from the LLM action, keeps behavior-cloning weight at least 0.1, and can
contribute at most 0.4. Its weight increases only after rolling 30-window SLA
satisfaction reaches 95%; three consecutive violations force an immediate
fallback to the LLM action.

### Complete experiment workflow

The formal workflow compares all four arms over paired seeds:
`static_equal`, `llm_only`, `ddpg_only`, and
`llm_guided_ddpg`. Use the Anaconda base interpreter shown below. Do not
manually start another DDPG service or POST an intent while a formal run is in
progress; the runner owns the stack, traffic, intent changes, checkpoints, and
RC actuator.

#### 1. One-time readiness check

Complete the build described in **First-time build**, cache
`google/gemma-4-E2B-it`, and verify the base environment:

```bash
nvidia-smi

/home/ics1/anaconda3/bin/python - <<'PY'
import importlib.metadata
import torch

for package in ("torch", "transformers", "accelerate", "bitsandbytes"):
    print(package, importlib.metadata.version(package))
print("CUDA:", torch.cuda.is_available())
print("GPU:", torch.cuda.get_device_name(0) if torch.cuda.is_available() else None)
PY
```

The result must show CUDA available. Formal runs do not permit mock, CPU, or
silent LLM fallback.

### Static architecture smoke test

Before running the four-arm campaign, validate the complete control path in a
simple, fixed AWGN scenario. This smoke test does not enable RFSimulator
`chanmod` or the telnet channel controller. It runs only
`llm_guided_ddpg/seed1` and verifies:

```text
FlexRIC SM -> raw/summary DB -> Gemma rAPP -> A1-like policy
           -> asynchronous DDPG -> persistent RC xApp -> E2/gNB
```

Run it from the Anaconda base environment:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/architecture-smoke-static
set -o pipefail

mkdir -p "$RESULTS"
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_architecture_smoke_static.json \
  --results "$RESULTS" \
  --manage-stack \
  --seed 1 \
  --arm llm_guided_ddpg \
  --resume \
  --fail-fast \
  2>&1 | tee "$RESULTS/smoke.log"
```

The 100-second calibration intentionally collects at least 1000 real
transitions before the asynchronous learner can publish an Actor. The shorter
training and evaluation phases validate process separation and policy flow;
they are not used for statistical performance claims.

Monitor the smoke test from another terminal:

```bash
tail -F /tmp/llm_hric/experiments/architecture-smoke-static/smoke.log

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT run_id,phase,training_enabled,publishing_enabled,serving_version,
        replay_valid_count,learner_train_steps,learner_actor_updates,
        (strftime('%s','now')*1000-learner_heartbeat_ts_ms) heartbeat_age_ms
 FROM ddpg_runtime_state ORDER BY updated_ts_ms DESC LIMIT 1;

 SELECT version,accepted,reason,train_steps,actor_update_steps
 FROM ddpg_actor_versions ORDER BY created_ts_ms DESC LIMIT 5;

 SELECT phase,COUNT(*) ticks,
        ROUND(AVG(jitter_ms),3) mean_jitter_ms,
        SUM(skip_reason IN ('no_state','stale_age','state_not_advanced')) stale_skips,
        SUM(action_issued) actions
 FROM controller_ticks GROUP BY phase;"
```

After completion, generate the smoke report:

```bash
/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" \
  --output "$RESULTS/analysis"

sed -n '1,240p' "$RESULTS/analysis/REPORT.md"
```

Only proceed to the formal static campaign below after this run completes
with five active UEs, fresh SM/KPM summaries, RC acknowledgements, periodic
A1 policy updates, and at least one accepted serving Actor version.

#### 2. Prepare the shell

Run the following commands in one terminal. The sudo ticket is terminal-bound,
so execute `sudo -v` in the same terminal that launches the experiment:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-formal-v3-2-1
set -o pipefail

mkdir -p "$RESULTS"
sudo -v
```

With `--manage-stack`, the runner performs a full cleanup before every
arm/seed and starts core, nearRT-RIC, gNB, five UEs, both monitors, A1, Gemma,
and the persistent RC actuator. It deliberately sets `START_DDPG=0` so no
standalone controller competes with the selected experiment arm.

#### 3. Run the seed-1 pilot

Run all four arms for seed 1 before committing to the full campaign:

```bash
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_ablation_v3_2_1.json \
  --results "$RESULTS" \
  --manage-stack \
  --seed 1 \
  --resume \
  --fail-fast \
  2>&1 | tee "$RESULTS/pilot.log"
```

`--fail-fast` archives the failed run and stops at the first invalid result.
`--resume` skips only complete arm/seed manifests, so rerunning this command
retries failed pairs without deleting their diagnostic artifacts.

#### 4. Observe a running pilot

Use another terminal:

```bash
export RESULTS=/tmp/llm_hric/experiments/five-ue-formal-v3-2-1
tail -F "$RESULTS/pilot.log"
```

Check the active run and phase:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT run_id,arm,seed,status
 FROM experiment_runs ORDER BY start_ts_ms DESC LIMIT 3;

 SELECT phase,COUNT(*) steps,ROUND(AVG(reward),3) mean_reward,
        MAX(next_state_ts_ms) latest_state
 FROM experiment_steps GROUP BY phase;"
```

Check actual LLM, fused, and applied PRB actions:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT datetime(ts_ms/1000,'unixepoch','localtime') time,
        json_extract(action_json,'$.llm_action.prb_ratio') llm,
        json_extract(action_json,'$.ddpg_action.prb_ratio') ddpg,
        json_extract(action_json,'$.fused_action.prb_ratio') fused,
        json_extract(action_json,'$.fused_action.ddpg_weight') ddpg_weight,
        applied
 FROM ddpg_actions ORDER BY ts_ms DESC LIMIT 10;"
```

Check asynchronous learner progress and Actor publication:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT run_id,phase,training_enabled,publishing_enabled,serving_version,
        learner_cursor,replay_valid_count,pending_updates,
        learner_train_steps,learner_actor_updates,
        (strftime('%s','now')*1000-learner_heartbeat_ts_ms) heartbeat_age_ms
 FROM ddpg_runtime_state ORDER BY updated_ts_ms DESC LIMIT 4;

 SELECT version,accepted,reason,train_steps,actor_update_steps,
        json_extract(metrics_json,'$.saturation_rate') saturation,
        json_extract(metrics_json,'$.mean_action_shift') action_shift
 FROM ddpg_actor_versions ORDER BY created_ts_ms DESC LIMIT 10;"
```

Check five-UE throughput and monitor freshness:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT sd,ue_id,ROUND(dl_th_mbps,2) dl_mbps
 FROM ue_slice_throughput
 WHERE ts_ms=(SELECT MAX(ts_ms) FROM ue_slice_throughput)
 ORDER BY sd,ue_id;

 SELECT 'MAC' source,MAX(ts_ms) latest FROM mac_ue_stats_raw
 UNION ALL SELECT 'KPM',MAX(ts_ms) FROM kpm_measurements_raw
 UNION ALL SELECT 'summary',MAX(ts_ms) FROM network_state;"
```

The runner discovers PDU IPs from each namespace/TUN and RNTIs from each
`nrUE<N>.log`; it never assumes attach order equals UE identity.

#### 5. Validate the pilot

List archived outcomes:

```bash
find "$RESULTS" -mindepth 2 -maxdepth 2 -name manifest.json -print0 |
  xargs -0 -n1 jq -r '[.arm,.seed,.status,.failure] | @tsv'
```

The pilot passes only when seed 1 has one complete manifest for every arm.
Each complete manifest must report five valid iperf UEs, five mapped UEs,
phase counts of 1000 calibration, 6000 training, 1800 intent-1 evaluation, and
1800 intent-2 evaluation transitions. RC apply success must be at least 99%,
at least 95% of transitions must have post-command observation coverage >= 0.8, and RC latency
p99 must remain below 20 ms. Calibration sweeps Slice-A ratios
25/35/50/65/75, stores one shared profile per seed, and derives each protected
floor as 90% of the protected-slice p10 at the directional 65/35 target. Intent
success separately requires at least 55% PRBs for the priority slice.

Generate a pilot report:

```bash
/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" \
  --output "$RESULTS/analysis-pilot"

sed -n '1,240p' "$RESULTS/analysis-pilot/REPORT.md"
```

#### 6. Run the complete paired-seed campaign

After the pilot passes, omit `--seed`. Completed seed-1 pairs are skipped and
seeds 2-5 continue:

```bash
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_ablation_v3_2_1.json \
  --results "$RESULTS" \
  --manage-stack \
  --resume \
  --fail-fast \
  2>&1 | tee "$RESULTS/campaign.log"
```

There are 20 valid combinations: four arms times five paired seeds. Arm order
is deterministically randomized within each seed. A run takes about 17-20
minutes; the full campaign normally takes 7-9 hours including stack restarts
and Gemma loading. The v3.2.1 specification offers 40 Mbps per UE, reducing the
uninformative packet loss seen in the earlier 100 Mbps-per-UE campaign while
keeping both slices backlogged.

If a run fails, fix the cause and execute the same command again. Do not delete
the results directory: `--resume` preserves complete pairs and failed
diagnostics. Each run directory contains `manifest.json`, a DB backup,
checkpoint, five client and five server iperf JSON files,
`ue_receiver_counters.json`, `traffic_summary.json`, and process logs. Receiver
throughput uses each UE namespace's TUN RX byte delta so that an overloaded
slice cannot invalidate the measurement by dropping iperf's final TCP control
response. Server-side interval statistics remain available as an independent
cross-check.

After rebuilding a stale PDCP SM plugin, first validate the previously failing
long arm in the same results directory. This run must finish all 6000 training
steps before the full campaign is resumed:

```bash
sudo -v
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_ablation_v3_2_1.json \
  --results "$RESULTS" \
  --manage-stack --arm llm_guided_ddpg --seed 1 \
  --resume --fail-fast \
  2>&1 | tee "$RESULTS/pdcp-long-validation.log"
```

Only manifests whose status is `complete` are skipped by `--resume`; an older
failed manifest remains available for diagnosis and does not suppress this
retry. Once this arm completes, run the complete campaign command above.

If a failed manifest reports a low RC apply success rate, inspect
`logs/rc_slice_actuator.log`. The assertion below came from the old synchronous
CONTROL path creating and deleting a redundant pending-event timer for every
request:

```text
assoc_rb_tree_extract: Assertion `z_node != tree->dummy` failed
```

The current tree uses `cond_wait_sync_ui()` as the single synchronous timeout
and no longer inserts CONTROL requests into that timer bimap. Rebuild the
actuator after updating this source, then resume the same campaign:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
CCACHE_DISABLE=1 cmake --build build --target xapp_rc_slice_ctrl -j2
```

The runtime watchdog checks both the actuator PID and socket, so a future
actuator failure is archived immediately instead of being discovered only at
final validation.

At sustained traffic, an older FlexRIC xApp SQLite schema could terminate the
SM monitor when the 64-bit RLC `txsdu_bytes` counter crossed `2^32`. The current
schema keeps RLC SDU byte counters as non-negative SQLite integers, retries
`SQLITE_BUSY/SQLITE_LOCKED`, and prints the actual SQL error before aborting.
Campaign cleanup also removes the redundant `/tmp/xapp_db_*` SDK databases;
the archived LLM-hRIC raw and summary tables remain the experiment record.

#### 7. Generate and inspect final results

```bash
/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" \
  --output "$RESULTS/analysis"

ls -lh "$RESULTS/analysis"
sed -n '1,260p' "$RESULTS/analysis/REPORT.md"
column -s, -t < "$RESULTS/analysis/metrics.csv" | less -S
jq . "$RESULTS/analysis/paired_bootstrap.json"
```

Analysis outputs:

- `REPORT.md`: arm means, paired comparisons, confidence intervals, and the
  support/partial-support/no-support conclusion.
- `metrics.csv`: phase-specific metrics for every valid arm/seed.
- `paired_bootstrap.json`: paired-seed bootstrap results.
- `learning_and_policy_trajectories.svg`: reward, SLA violation, and PRB
  action trajectories; a PNG is also written when Matplotlib is usable.
- `controller_tick_jitter.svg`: controller timing independently of learner
  updates.
- `async_learner_diagnostics.svg`: TD error, Q-target gap, and learner queue
  depth over asynchronous update steps.
- `dynamic_channel_<arm>_seed<seed>.svg`: channel state, throughput, BLER,
  CQI, and PRB action trajectory for dynamic-channel runs.

Intent-1 and intent-2 frozen evaluation windows are analyzed separately.
Slice PRB share comes from synchronized summary windows. Cell PRB utilization
uses per-timestamp KPM `RRU.PrbTotDl` totals divided by 106 PRBs. The main
hypothesis is supported only when guided DDPG improves early SLA/AUC over pure
DDPG, does not reduce steady total throughput by more than 5%, and improves SLA
satisfaction. Paired confidence intervals use only seeds for which both
compared arms completed.

### Three traffic scenarios (DDPG v4)

The v4 traffic campaign compares `llm_guided_ddpg`, `llm_only`, and
`ddpg_only` under three static-AWGN traffic scenarios. Slice A contains
UE1-UE3 (`1:ffffff`) and Slice B contains UE4-UE5 (`1:123456`). Rates are per
UE and are equal inside a slice:

| Scenario | Phase 1 A/B | Phase 2 A/B | Switch period | Total offered load |
|---|---:|---:|---:|---:|
| `balanced` | 30/30 Mbps | none | none | 150 Mbps |
| `slice_a_heavy` | 36/21 Mbps | 44/9 Mbps | 5 s | 150 Mbps |
| `slice_b_heavy` | 28/33 Mbps | 12/57 Mbps | 5 s | 150 Mbps |

Dynamic profiles use one persistent peak-rate UDP stream per UE. The ext-dn
container installs an independent HTB class for every UE destination and
changes the class `rate/ceil` every five seconds. A traffic switch therefore
does not create a new iperf TCP control connection over the congested RAN
path. Every switch is stored in `traffic_events`; the same scenario definition
has the same `trace_hash` in all three arms.

The runner reads each HTB class byte counter at phase boundaries. A segment is
valid only when its duration covers at least 80% of the phase and its measured
ext-dn egress rate is within the configured coverage range of the requested
rate. At least 95% of dynamic segments must be valid. The detailed audit is
written to `traffic_shaper_segments.json`; `traffic_summary.json` exposes both
`dynamic_segment_success_rate` and the backward-compatible
`burst_segment_success_rate`. The root qdisc is removed when traffic stops.
Dynamic manifests created by the older repeated-burst implementation are
retained for audit but are not skipped by `--resume` and are excluded from
primary comparisons.

Validate the specification and run all unit tests before using the RAN:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"

/home/ics1/anaconda3/bin/python -m unittest discover \
  -s llm_hric/tests -p 'test*.py'

/home/ics1/anaconda3/bin/python - <<'PY'
import json
from pathlib import Path
from llm_hric.config import load_config
from llm_hric.experiments.traffic_controller import normalize_traffic_scenario

path = Path("llm_hric/experiments/five_ue_traffic_scenarios_v4.json")
spec = json.loads(path.read_text())
catalog = load_config()["ue_catalog"]
for raw in spec["traffic_scenarios"]:
    scenario = normalize_traffic_scenario(raw, 30, catalog)
    print(scenario["id"], scenario["total_offered_mbps"], scenario["trace_hash"])
PY
```

Before a full pilot, run a 30-second real dynamic-traffic smoke test. This
requires the five UE namespaces and TUN interfaces to be active:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
SMOKE=/tmp/llm_hric/traffic/tc-htb-smoke-$(date +%s)
mkdir -p "$SMOKE"

jq '.traffic_scenarios[] | select(.id=="slice_a_heavy")' \
  llm_hric/experiments/five_ue_traffic_scenarios_v4.json \
  > "$SMOKE/scenario.json"

sudo -v
/home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.traffic_controller \
  --scenario-json "$SMOKE/scenario.json" \
  --duration-s 30 --result-dir "$SMOKE"

jq '{traffic_backend,expected_dynamic_segments,valid_dynamic_segments,
     dynamic_segment_success_rate}' "$SMOKE/traffic_summary.json"
docker exec rfsim5g-oai-ext-dn tc qdisc show dev eth0
```

Acceptance requires `traffic_backend="tc_htb"`, dynamic segment success at
least `0.95`, and no leftover root HTB qdisc after the command exits.

Run the complete seed-1 pilot (three scenarios times three arms, nine runs):

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-traffic-v4
set -o pipefail
mkdir -p "$RESULTS"
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v4.json \
  --results "$RESULTS" \
  --manage-stack --seed 1 --resume --fail-fast \
  2>&1 | tee "$RESULTS/pilot.log"
```

If an earlier seed-1 pilot completed before burst-segment validation was
added, run the same command again. `--resume` skips the three valid `balanced`
runs and reruns the six dynamic runs:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-traffic-v4
set -o pipefail
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v4.json \
  --results "$RESULTS" \
  --manage-stack --seed 1 --resume --fail-fast \
  2>&1 | tee "$RESULTS/pilot-rerun.log"
```

For a shorter diagnosis, select one scenario and arm with, for example,
`--scenario slice_a_heavy --arm llm_guided_ddpg`. A completed or degraded
manifest is keyed by `scenario/arm/seed`; paths and calibration profiles never
collide across scenarios.

Check pilot outcomes and traffic trace identity:

```bash
find "$RESULTS" -name manifest.json -print0 | \
  xargs -0 -n1 jq -r \
  '[.scenario,.arm,.seed,.status,.validation.five_ue_coverage,
    .validation.dynamic_segment_success_rate,.failure] | @tsv'

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT scenario,phase_index,COUNT(*) ue_events,
        ROUND(AVG(offered_mbps),2) mean_per_ue_mbps,
        ROUND(AVG(applied_ts_ms-planned_ts_ms),2) apply_delay_ms,
        MIN(success) all_success,MIN(trace_hash) trace_hash
 FROM traffic_events GROUP BY scenario,planned_ts_ms ORDER BY planned_ts_ms DESC LIMIT 20;"
```

A run with less than 90% full-five-UE summary coverage is marked `degraded`.
It remains available for audit and plots, but is excluded from the primary
paired-seed comparison. UE churn does not stop the controller by itself.

After all nine seed-1 runs are acceptable, resume without `--seed` to run all
45 combinations:

```bash
sudo -v
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v4.json \
  --results "$RESULTS" \
  --manage-stack --resume --fail-fast \
  2>&1 | tee "$RESULTS/campaign.log"
```

Generate separate scenario reports and the macro-average report:

```bash
/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" --output "$RESULTS/analysis"

sed -n '1,260p' "$RESULTS/analysis/REPORT.md"
sed -n '1,220p' "$RESULTS/analysis/slice_a_heavy/REPORT.md"
column -s, -t < "$RESULTS/analysis/scenario_arm_summary.csv" | less -S
column -s, -t < "$RESULTS/analysis/paired_comparisons.csv" | less -S
```

The analysis directory contains `run_metrics.csv`,
`scenario_arm_summary.csv`, `paired_comparisons.csv`,
`ddpg_learning_curves.csv`, `traffic_phase_metrics.csv`, and
`traffic_event_aligned.csv`. For each scenario
it also generates performance, DDPG-learning, and traffic-event response plots
in PNG/SVG format. Raw windows are never pooled across traffic scenarios;
cross-scenario values are equal-weight macro averages of scenario means.

### v5 campaign: retraining and testing a saved agent

The v5 spec (`llm_hric/experiments/five_ue_traffic_scenarios_v5.json`) keeps
the v4 scenarios and arms but fixes the DDPG convergence problems observed in
v4:

- `training_s` 600 → 3600 (36,000 online steps; 18,000 per intent).
- Exploration noise decays over 40% of the training steps (derived by the
  runner from the spec), leaving a low-noise exploitation tail before the
  frozen evaluations; the initial noise is 0.2.
- TD3-style stabilizers in `ddpg_rc_agent.py` (all configured in
  `config.yaml` under `ddpg`): twin critic with clipped double-Q targets,
  target policy smoothing (`target_policy_noise`), a pre-activation actor
  saturation penalty (`actor_saturation_penalty_weight`), and `gamma` 0.95.
- Actor promotion: `require_initial_snapshot: true` and a forced-promotion
  fallback (`force_promotion_after_rejections`) so the serving actor can no
  longer starve behind the quality gate.
- Runtime fixes for the 6× longer runs: composite indexes on
  `network_state`/`applied_prb_policy` (controller tick jitter), and slim
  periodic learner checkpoints (the replay buffer is only serialized in the
  final save).

#### Retraining from scratch

Run inside `tmux` — the runner refreshes sudo credentials against the
controlling terminal, so a detached `nohup` run will fail. One DDPG run takes
about 95 minutes; a full seed (3 scenarios × 3 arms) takes 10–12 hours.

```bash
tmux new -s v5
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-traffic-v5
mkdir -p "$RESULTS"; sudo -v

# Smoke first: one scenario, one arm (~95 min).
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

# Scored comparison across arms:
/home/ics1/anaconda3/bin/python -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" --output "$RESULTS/analysis"
```

Convergence signatures to check in `analysis/*_ddpg_learning.png` and the
run's `ddpg_async_learner.log`: `predicted_q` plateaus instead of inflating,
`critic_loss`/`td_error` stop rising, most actor candidates are accepted, and
the applied PRB ratio spread narrows in late training. A failed run is not
skipped by `--resume`; rerunning the same command retries it.

#### Testing a saved agent

Every DDPG-bearing run archives its trained agent as `ddpg.pt` inside the run
directory (final learner save: actor, twin critics, targets, optimizers,
normalization statistics, replay). Three ways to use it, cheapest first:

1. **Offline replay (no RAN stack).** Feeds the run's recorded states through
   the deterministic policy; reports per-intent allocations, Q values, and
   agreement with the live actions, plus an optional histogram:

   ```bash
   RUN=$RESULTS/balanced/<run-directory>
   /home/ics1/anaconda3/bin/python -m llm_hric.experiments.eval_checkpoint \
     --checkpoint "$RUN/ddpg.pt" --db "$RUN/llm_hric.sqlite3" \
     --phase training --plot "$RUN/policy_response.png"
   ```

2. **Frozen-policy protocol re-evaluation (scored, ~12 min).** Loads the
   checkpoint and runs the standard protocol without any training: settle →
   calibration → `intent1_eval` → `intent2_eval`, exploration off, learner
   off. Requires `--arm` (the arm the checkpoint was trained for) and a
   dedicated results directory so eval runs never mix with training runs:

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

   The manifest records `mode: eval_checkpoint` and the source checkpoint
   path; the run id carries an `-evalck` marker. Such manifests are ignored
   by `--resume` bookkeeping, and the source checkpoint is copied into the
   new run directory before loading, so it is never modified. Run
   `analyze_results` on the eval results directory to score the frozen
   policy with the usual metrics.

3. **Live deployment demo (Grafana).** Start the stack with the trained
   checkpoint served in pure-inference mode, post an intent, and generate
   traffic; switching the intent live shows the allocation flip in Grafana:

   ```bash
   cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
   UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 \
   START_DDPG=1 DDPG_MODE=deploy DDPG_ARM=ddpg_only DDPG_APPLY=1 \
   DDPG_CHECKPOINT="$RUN/ddpg.pt" \
   ./run_e2e_rfsim.sh start
   ```

   `DDPG_APPLY=0` gives a shadow mode that records decisions without
   actuating them. Do not run this while an experiment campaign is active —
   they share the stack and the monitor database.

### Dynamic TDL-A robustness campaign

Run this only after the static async campaign passes. The runner starts every
nrUE with `chanmod` and an OAI telnet server inside its own network namespace,
discovers the channel model ID by name, calibrates good/medium/poor levels,
and applies a deterministic per-seed Markov trace every 5 s. It changes only
RFSimulator samples; it does not change RAN or RIC timing.

Seed-1 pilot:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-dynamic-tdla-v3-2-1
mkdir -p "$RESULTS"
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_ablation_v3_2_1_dynamic.json \
  --results "$RESULTS" \
  --manage-stack --seed 1 --resume --fail-fast \
  2>&1 | tee "$RESULTS/pilot.log"
```

Inspect channel commands and trace identity:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
"SELECT phase,event_index,ue_id,channel_state,noise_power_db,path_loss_db,
        command_success,applied_ts_ms-planned_ts_ms apply_delay_ms,trace_hash
 FROM channel_events ORDER BY planned_ts_ms DESC LIMIT 25;"
```

After all four seed-1 arms complete, rerun without `--seed` for five paired
seeds, then analyze it separately from the static campaign:

```bash
sudo -v
START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_ablation_v3_2_1_dynamic.json \
  --results "$RESULTS" \
  --manage-stack --resume --fail-fast \
  2>&1 | tee "$RESULTS/campaign.log"

/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" --output "$RESULTS/analysis"
```

The four arms of a paired seed reuse the same saved calibration profile and
trace files. Training and frozen evaluation use distinct deterministic trace
seeds. A run fails if any telnet command fails, fewer than five UEs are
covered, or good/medium/poor calibration cannot be found without RLF.

## Dynamic intent and DDPG modes

Run the A1-like server and LLM guidance service. The rAPP reads the latest active intent and recent network state every `period_ms.llm` interval, then writes a fresh guidance/A1 policy. Posting a new intent changes the objective without restarting the pipeline:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
python3 -u -m llm_hric.a1_policy_server

PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
python3 -u -m llm_hric.llm_guidance_service \
  --intent "prioritize slice 0xffffff while keeping slice 0x123456 above 30 Mbps"
```

Post a new active intent:

```bash
curl -X POST http://127.0.0.1:8088/a1-p/intents/slice-prb-intent \
  -H 'Content-Type: application/json' \
  -d '{"intent":"prioritize slice 0x123456 while keeping slice 0xffffff above 20 Mbps","valid_for_ms":1000}'
```

Train DDPG online from the SQLite state/action stream without applying exploratory RC actions:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
python3 -u -m llm_hric.ddpg_rc_agent --mode train --checkpoint /tmp/llm_hric/ddpg_prb.pt
```

Deploy a trained checkpoint and apply RC Style 2 / Action 6 policy through the C xApp:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
python3 -u -m llm_hric.ddpg_rc_agent --mode deploy --checkpoint /tmp/llm_hric/ddpg_prb.pt --apply
```

## LLM provider

`llm_client.py` supports three provider modes through `config.yaml`:

- `provider: "transformers"` loads a local HuggingFace model. The default config uses `google/gemma-4-E2B-it` on GPU with 4-bit quantization.
- `provider: "openai-compatible"` calls an API server with `/v1/chat/completions`.
- `provider: "mock"` keeps the offline deterministic guidance generator.

For local Gemma, install the model runtime first:

```bash
pip install torch transformers accelerate sentencepiece
```

The default config uses:

```json
"llm": {
  "provider": "transformers",
  "model": "google/gemma-4-E2B-it",
  "model_class": "AutoModelForCausalLM",
  "device_map": "auto",
  "torch_dtype": "auto",
  "allow_fallback": false,
  "require_real_model": true
}
```

`require_real_model: true` prevents accidental mock deployment. If the Transformers model cannot be loaded, the rAPP exits instead of silently falling back. For offline smoke tests, set `require_real_model: false` and `allow_fallback: true`.

To use another local model, change `model` and, if needed, `model_class` to a class available in your installed `transformers` package, for example `AutoModelForCausalLM`. To use an API provider, switch to:

```json
"llm": {
  "provider": "openai-compatible",
  "base_url": "http://127.0.0.1:8000/v1",
  "api_key_env": "OPENAI_API_KEY",
  "model": "your-api-model",
  "temperature": 0.0,
  "timeout_s": 10
}
```

## Gemma rAPP closed-loop run

Use this sequence after the core, nearRT-RIC, gNB, UEs, and iperf traffic are running. It verifies that the rAPP reads SQLite state, asks Gemma for an AI PRB action, publishes it through A1-like policy storage, and lets the DDPG controller deploy Gemma-first actions.

Check that the configured provider really loads Gemma:

```bash
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
python3 - <<'PY'
from llm_hric.config import load_config
from llm_hric.llm_client import make_llm_client
cfg = load_config('/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/config.yaml')
client = make_llm_client(cfg)
print(type(client).__name__)
PY
```

Expected output includes `TransformersClient`. If `require_real_model` is true, any mock fallback is treated as an error.

Start the A1 server, Gemma rAPP, and DDPG deploy/apply loop in separate terminals:

```bash
export PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
python3 -u -m llm_hric.a1_policy_server

export PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
python3 -u -m llm_hric.llm_guidance_service \
  --intent "prioritize slice 0xffffff while keeping slice 0x123456 above 30 Mbps"

export PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
python3 -u -m llm_hric.ddpg_rc_agent \
  --mode deploy --checkpoint /tmp/llm_hric/ddpg_prb.pt --apply --continue-training
```

Post a new dynamic intent without restarting services:

```bash
curl -X POST http://127.0.0.1:8088/a1-p/intents/slice-prb-intent \
  -H 'Content-Type: application/json' \
  -d '{"intent":"prioritize slice 0x123456 while keeping slice 0xffffff above 20 Mbps","valid_for_ms":1000}'
```

Inspect the A1 policy and DDPG action records:

```bash
python3 - <<'PY'
import json, sqlite3
conn = sqlite3.connect('/tmp/llm_hric/llm_hric.sqlite3')
conn.row_factory = sqlite3.Row
print(dict(conn.execute("SELECT policy_id, version, preferred_ratio_json, llm_action_json FROM a1_policy_summary WHERE active=1").fetchone()))
row = conn.execute("SELECT action_json FROM ddpg_actions ORDER BY ts_ms DESC LIMIT 1").fetchone()
print(json.dumps(json.loads(row[0]), indent=2, sort_keys=True))
PY
```

Confirm that the final fused action reached gNB MAC:

```bash
grep -Ei "RC slice policy|slice_prb|Style 2|Action ID 6|CONTROL" \
  /tmp/llm_hric/e2e_rfsim/logs/gnb.log | tail -100
```

During the first 512 valid transitions, `ddpg_weight` is `0.0` and the applied policy stays near Gemma's action with only bounded data-collection exploration. Afterwards the DDPG weight is SLA-gated rather than time-ramped: rolling satisfaction must reach 95%, the maximum contribution is 0.4, and three consecutive violations return it to zero. `ddpg_actions.action_json` records the LLM, Actor, fused, and applied actions.

## Grafana monitor

The Grafana monitor uses the SQLite datasource plugin and reads `/tmp/llm_hric/llm_hric.sqlite3`. The container mounts `/tmp/llm_hric` read-write so SQLite can create WAL/lock files. If panels show `No data` with SQLite open errors, make the local test DB writable by the Grafana container: `chmod a+rwx /tmp/llm_hric && chmod a+rw /tmp/llm_hric/llm_hric.sqlite3`. The dashboard itself is monitoring-only: it does not submit A1 policies or change RC control state.

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose up -d
```

Open `http://127.0.0.1:3000` and select `LLM-hRIC Runtime Monitor`. The dashboard refreshes every 500 ms and shows:

- current requested/active LLM intent;
- active A1 policy summary;
- DL throughput with legend `ue_id | sst:sd` and a latest UE/RNTI/S-NSSAI mapping table;
- latest DDPG/RC xApp PRB ratios.

The four top status panels have deliberately separate meanings:

- `A1 Update Age`: wall-clock age of the latest active `a1_policy_summary` row.
- `DDPG Decision Age`: wall-clock age of the latest `ddpg_actions` decision,
  regardless of whether the RC command was applied.
- `KPM Update Interval`: receive-time difference between the latest two
  distinct KPM indication timestamps. Multiple measurement rows belonging to
  one indication are grouped before calculating the interval.
- `KPM Last Seen Age`: wall-clock age of the latest grouped KPM indication.
  The interval remains the last observed sampling period if KPM stops, while
  last-seen age keeps increasing.

Restart
```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
docker compose -f grafana/docker-compose.yml restart
```
```bash
docker compose -f grafana/docker-compose.yml down
docker compose -f grafana/docker-compose.yml up -d
```




## Multi-UE rfsim test

The helper script can run the OAI multi-UE namespace layout from `doc/NR_SA_Tutorial_OAI_multi_UE.md`. In multi-UE mode it creates `ue1..ue5` namespaces with `tools/scripts/multi-ue.sh`, starts one local `nr-uesoftmodem` process per UE, and starts the FlexRIC SM monitor xApp. The monitor writes MAC/RLC/PDCP/GTP indication-derived data into the LLM-hRIC SQLite DB.

- UE1/UE2/UE3: S-NSSAI `1/0xffffff`, DNN `oai`, PDU IPs `12.1.1.2..12.1.1.4`
- UE4/UE5: S-NSSAI `1/0x123456`, DNN `openairinterface`, PDU IPs `12.1.2.2..12.1.2.3`

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
sudo -v
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
./run_e2e_rfsim.sh status
./run_e2e_rfsim.sh logs
```

If a previous launcher left an untracked gNB or nearRT-RIC process, use the explicit cleanup command before restarting. This is required when `gnb.log` contains `failed to bind socket ... 2152` or `nearRT-RIC.log` reports a duplicate E2 node:

```bash
sudo -v
UE_COUNT=5 START_GRAFANA=1 ./run_e2e_rfsim.sh cleanup
```

Normal `start` now refuses to continue when an unmanaged `nr-softmodem` or `nearRT-RIC` process exists, and aborts if the new gNB cannot bind N3/GTP-U.

`running pid` only proves that the launcher can still signal the recorded PID. The `status` command also prints database freshness; a healthy closed loop has advancing `network_state`, `kpm_measurements_raw`, `llm_guidance`, and `ddpg_actions` timestamps. Check the service logs when a table has no data or its `age_ms` keeps increasing:

```bash
tail -F \
  /tmp/llm_hric/e2e_rfsim/logs/flexric_sm_monitor.log \
  /tmp/llm_hric/e2e_rfsim/logs/kpm_monitor.log \
  /tmp/llm_hric/e2e_rfsim/logs/llm_hric_guidance.log \
  /tmp/llm_hric/e2e_rfsim/logs/llm_hric_ddpg.log
```

The internal MAC/RLC/PDCP/GTP SM subscriptions default to the largest interval supported by this FlexRIC API, `10 ms`. Raw indications are retained and summary tables are emitted every `monitor.summary_period_ms` (`50 ms` by default). Configure the subscription with `monitor.subscription_interval_ms`; valid values are `1`, `2`, `5`, and `10`.

Grafana refreshes every `500 ms`. The TH panels query only the latest two
minutes and aggregate 50 ms summaries into 100 ms display buckets. The intent
panel shows both the latest requested intent and the currently active A1 policy
intent; `policy_status=generating` is expected while Gemma is producing the
next policy. Recreate the Grafana container after dashboard changes:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose up -d --force-recreate
```

FlexRIC example xApps default to `XAPP_DURATION=20` seconds. The launcher sets `XAPP_DURATION=-1` for both monitor processes so they remain subscribed until `run_e2e_rfsim.sh stop`. When starting the C KPM monitor manually, preserve that environment explicitly:

```bash
XAPP_DURATION=-1 LLM_HRIC_DB_PATH=/tmp/llm_hric/llm_hric.sqlite3 \
  /home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/xApp/c/monitor/xapp_kpm_moni
```

`Test xApp run SUCCESSFULLY` about 20 seconds after startup means the duration override was missing. An internal monitor assertion in `find_act_proc` during shutdown indicates teardown occurred before subscription removal; the current Python monitor waits indefinitely and only calls `try_stop()` after removing all report handles.

The Python monitor must not block its main thread in the SWIG `xapp_wait()` call: that call retains the Python GIL and prevents MAC/RLC/PDCP/GTP director callbacks from running. The current implementation waits on a Python `Event`, which releases the GIL, and installs SIGINT/SIGTERM handlers for ordered subscription cleanup. A characteristic symptom of the old behavior is that all four raw-table timestamps advance only during subscription setup and then freeze while the monitor PID remains alive.

If Gemma reaches its generation limit, the guidance log reports a JSON parse failure and preserves the previous active A1 policy. Keep `llm.max_new_tokens` at `256` or higher and keep the requested `reason` short. The continuous rAPP retries on its next configured period instead of exiting.

After the UEs attach, check `/tmp/llm_hric/e2e_rfsim/logs/flexric_sm_monitor.log` for unknown RNTIs:

```bash
grep -Ei "unknown RNTI|MAC indication|RLC indication|PDCP indication|GTP indication" \
  /tmp/llm_hric/e2e_rfsim/logs/flexric_sm_monitor.log | tail -100
```

If the log prints unknown RNTIs, add them to `monitor.rnti_slice_map` in `config.yaml`, then restart the LLM-hRIC services. Example:

```json
"monitor": {
  "source": "flexric_sm",
  "rnti_slice_map": [
    {"rnti": "0x0001", "ue_id": "ue1", "plmn": "20899", "sst": 1, "sd": "0xffffff"},
    {"rnti": "0x0002", "ue_id": "ue2", "plmn": "20899", "sst": 1, "sd": "0xffffff"},
    {"rnti": "0x0003", "ue_id": "ue3", "plmn": "20899", "sst": 1, "sd": "0xffffff"},
    {"rnti": "0x0004", "ue_id": "ue4", "plmn": "20899", "sst": 1, "sd": "0x123456"},
    {"rnti": "0x0005", "ue_id": "ue5", "plmn": "20899", "sst": 1, "sd": "0x123456"}
  ]
}
```

Then restart only the LLM-hRIC monitor/control processes, or restart the full scenario:

```bash
./run_e2e_rfsim.sh stop
sudo -v
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```

Check the UE PDU addresses inside their namespaces:

```bash
sudo ip netns exec ue1 ip addr show oaitun_ue1
sudo ip netns exec ue2 ip addr show oaitun_ue1
sudo ip netns exec ue3 ip addr show oaitun_ue1
sudo ip netns exec ue4 ip addr show oaitun_ue1
sudo ip netns exec ue5 ip addr show oaitun_ue1
```

The stock rfsim compose adds an ext-dn route only for `12.1.1.0/24`. For the second DNN/slice, add the `12.1.2.0/24` route before downlink tests. `run_e2e_rfsim.sh start` does this automatically.

The experiment traffic controller also performs this setup automatically. It restores both ext-dn routes, sends an uplink ping from every UE TUN to warm up the user-plane path, and retries each ext-dn-to-PDU-IP ping before starting iperf:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
sudo -v
PYTHONPATH=$PWD /home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.traffic_controller \
  --duration-s 60 --offered-mbps 100 \
  --result-dir /tmp/llm_hric/traffic/guided-smoke
```

The ext-dn container/address, route gateway/device, and ping retry count are configured under `traffic` in `config.yaml`. A failed preflight now identifies the exact UE, namespace, TUN, and PDU IP instead of returning only a generic `CalledProcessError`.

```bash
docker exec rfsim5g-oai-ext-dn ip route replace 12.1.2.0/24 via 192.168.72.134 dev eth0
docker exec rfsim5g-oai-ext-dn ip route
```

Run one downlink iperf server per UE namespace, replacing the bind addresses with the addresses printed above if they differ:

```
for i in 1 2 3 4 5; do
  echo "=== ue$i ==="
  sudo ip netns exec ue$i ip -br addr show oaitun_ue1
done
```

```bash
sudo ip netns exec ue1 iperf3 -s -B 12.1.1.2 -p 5201 -i 1 --forceflush > /tmp/ue1_iperf_server.log 2>&1 &
sudo ip netns exec ue2 iperf3 -s -B 12.1.1.3 -p 5202 -i 1 --forceflush > /tmp/ue2_iperf_server.log 2>&1 &
sudo ip netns exec ue3 iperf3 -s -B 12.1.1.4 -p 5203 -i 1 --forceflush > /tmp/ue3_iperf_server.log 2>&1 &
sudo ip netns exec ue4 iperf3 -s -B 12.1.2.2 -p 5204 -i 1 --forceflush > /tmp/ue4_iperf_server.log 2>&1 &
sudo ip netns exec ue5 iperf3 -s -B 12.1.2.3 -p 5205 -i 1 --forceflush > /tmp/ue5_iperf_server.log 2>&1 &
```

From the external DN container, run simultaneous downlink UDP clients:

```bash
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.1.2 -p 5201 -u -b 40M -t 60 --forceflush
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.1.3 -p 5202 -u -b 40M -t 60 --forceflush
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.1.4 -p 5203 -u -b 40M -t 60 --forceflush
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.2.2 -p 5204 -u -b 40M -t 60 --forceflush
docker exec rfsim5g-oai-ext-dn iperf3 -c 12.1.2.3 -p 5205 -u -b 40M -t 60 --forceflush
```

The Grafana `DL TH by UE and Slice` panel should show the UE IDs configured in `monitor.rnti_slice_map`. The `Applied Dedicated PRB Ratio` panel shows the most recent DDPG/RC xApp PRB policy.

## Training
```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-traffic-v5
sudo -v

START_GRAFANA=1 /home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results "$RESULTS" --manage-stack --seed 1 --resume --fail-fast \
  --scenario balanced \
  2>&1 | tee "$RESULTS/balanced-all-arms.log"
```


## Troubleshooting

### Monitor says `nearRT-RIC xApp endpoint 127.0.0.1:36422 is not reachable`

That message came from an older monitor script that incorrectly checked the E42 endpoint with TCP. FlexRIC xApps use SCTP on port `36422`, so TCP-only checks can report a false failure. Restart the monitor after updating the script:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
oldpid=$(cat /tmp/llm_hric/e2e_rfsim/pids/flexric_sm_monitor.pid 2>/dev/null || true)
if [ -n "$oldpid" ]; then kill "$oldpid" 2>/dev/null || true; fi

LD_PRELOAD=/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/xApp/c/monitor/RRC_MESSAGES/libasn1_nr_rrc_shared.so \
PYTHONPATH=/home/ics1/openairinterface5g/openair2/E2AP/flexric/build/examples/xApp/python3:/home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3 \
nohup python3 -u /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni.py \
  --config /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/config.yaml \
  --db-path /tmp/llm_hric/llm_hric.sqlite3 \
  > /tmp/llm_hric/e2e_rfsim/logs/flexric_sm_monitor.log 2>&1 &
echo $! > /tmp/llm_hric/e2e_rfsim/pids/flexric_sm_monitor.pid
```

A healthy monitor log should show `E42 SETUP-RESPONSE rx`, `Successfully subscribed to RAN_FUNC_ID 142`, and then `MAC indication ue=...`.

### gNB exits in `check_pdcp_bearer`

An older FlexRIC PDCP SM treats a transient relationship between independently updated packet and byte counters as a fatal invariant. Under sustained multi-UE traffic, the gNB can therefore exit with:

```text
check_pdcp_bearer: Assertion `rb->txpdu_pkts <= rb->txpdu_bytes' failed
```

Rebuild and validate the corrected plugin before restarting an experiment:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
CCACHE_DISABLE=1 cmake --build build --target pdcp_sm -j2

USE_SUDO=0 \
  examples/xApp/python3/llm_hric/run_e2e_rfsim.sh check-pdcp-plugin
```

`run_e2e_rfsim.sh` creates `/tmp/llm_hric/e2e_rfsim/sm`, links the other installed SM plugins there, overrides `libpdcp_sm.so` with the local FlexRIC build artifact, and points the gNB, nearRT-RIC and xApps at that runtime directory. It checks all four obsolete packet/byte assertions before creating the link and logs the selected absolute path plus SHA256. No write to `/usr/local/lib/flexric` is required. Verify the active link with:

```bash
readlink -f /tmp/llm_hric/e2e_rfsim/sm/libpdcp_sm.so
```

It must resolve to
`openair2/E2AP/flexric/build/src/sm/pdcp_sm/libpdcp_sm.so`, not the stale
`/usr/local/lib/flexric` copy.

### `sudo: a terminal is required to read the password`

This happens when a sudo command is launched in the background without a controlling terminal. Refresh sudo credentials before starting the helper script:

```bash
sudo -v
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```

If sudo is still a problem, run the script itself as root and disable internal sudo wrapping:

```bash
sudo -E env UE_MODE=multi UE_COUNT=5 USE_SUDO=0 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```

### `Device "oaitun_ue1" does not exist`

The UE has not completed PDU session setup, or the nrUE process exited after creating the tunnel. Check the UE logs first:

```bash
grep -Hn "PDU Session Establishment Accept\|TUN Interface\|unknown option\|Exiting" /tmp/llm_hric/e2e_rfsim/logs/nrUE*.log
```

Expected good lines are:

```text
Received PDU Session Establishment Accept, UE IPv4: 12.1.1.2
TUN Interface oaitun_ue1 successfully configured
```

If the log contains `unknown option`, remove the unsupported option from the nrUE command line. In this setup, telnet options are intentionally not passed to `nr-uesoftmodem`.

### iperf server says `Address already in use`

An iperf server is already listening on that UE IP and port. Check and clean it inside the relevant namespace:

```bash
sudo ip netns exec ue1 ss -lntup | grep 5201
sudo ip netns exec ue2 ss -lntup | grep 5202

sudo ip netns exec ue1 pkill -f "iperf3 -s" || true
sudo ip netns exec ue2 pkill -f "iperf3 -s" || true
```

Then start the two servers again with log redirection:

```bash
sudo ip netns exec ue1 iperf3 -s -B 12.1.1.2 -p 5201 -i 1 --forceflush > /tmp/ue1_iperf_server.log 2>&1 &
sudo ip netns exec ue2 iperf3 -s -B 12.1.2.2 -p 5202 -i 1 --forceflush > /tmp/ue2_iperf_server.log 2>&1 &
```

### ext-dn can ping UE1 but not UE2

The stock rfsim core compose only adds a route for `12.1.1.0/24`. Add the second DNN route:

```bash
docker exec rfsim5g-oai-ext-dn ip route replace 12.1.2.0/24 via 192.168.72.134 dev eth0
docker exec rfsim5g-oai-ext-dn ping -c 3 12.1.2.2
```

If downlink still fails, warm up the user plane from the UE side and retry:

```bash
sudo ip netns exec ue2 ping -I oaitun_ue1 -c 3 192.168.72.135
docker exec rfsim5g-oai-ext-dn ping -c 3 12.1.2.2
```

### iperf client appears to hang with no output

For UDP mode, iperf3 still opens a TCP control connection first. If that control connection cannot reach the UE-side server, the command can look stuck. Use a short timeout while debugging:

```bash
timeout 10 docker exec rfsim5g-oai-ext-dn \
  iperf3 -c 12.1.2.2 -p 5202 -u -b 5M -t 5 -i 1 --connect-timeout 3000 --forceflush
```

Verify the server and downlink path:

```bash
sudo ip netns exec ue2 ss -lntup | grep 5202
docker exec rfsim5g-oai-ext-dn ping -c 3 12.1.2.2
```

### `sqlite3.OperationalError: attempt to write a readonly database`

Grafana or a sudo-launched monitor may leave `/tmp/llm_hric/llm_hric.sqlite3*` owned by `nobody` or `root`. Fix ownership before running Python services manually:

```bash
mkdir -p /tmp/llm_hric
sudo chown "$USER:$(id -gn)" /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
chmod a+rwx /tmp/llm_hric
chmod a+rw /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
```

### A1 server exits with `duplicate column name: reward_running_std`

This was caused by A1, guidance, and monitor services racing while adding the
DDPG v3 schema columns. Current `schema.py` makes column migration
concurrently idempotent and configures a SQLite busy timeout. After updating
the code, rerun the same campaign command with `--resume`; completed manifests
are skipped and no database column should be removed manually.

### Grafana shows `52 years` or `56 years`

That means an old dashboard query treated a missing timestamp as Unix epoch `0`. Restart Grafana after updating the dashboard JSON:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose restart grafana
```

Missing A1/DDPG data should display as `No data`, not as a decades-old age.

### Grafana shows only one `value` line instead of UE names

Restart Grafana so it reloads the dashboard provisioned from JSON:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose restart grafana
```

The `DL TH by UE and Slice` panel should show separate series such as `1:ffffff / ue1` and `1:123456 / ue2`. Confirm the DB has both UE rows:

```bash
sqlite3 /tmp/llm_hric/llm_hric.sqlite3 \
  "select datetime(ts_ms/1000,'unixepoch','localtime'), sd, ue_id, dl_th_mbps from ue_slice_throughput order by ts_ms desc limit 10;"
```

### Grafana shows fewer UE curves than active iperf flows

The UE throughput panel reads the real users present in `ue_slice_throughput`; it does not invent UE series from the configured UE count. It groups by `ue_id`, while the slice aggregate panel shows slice-level totals. First check what the database actually contains:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select sd, ue_id, count(*) rows, round(max(dl_th_mbps),2) max_dl from ue_slice_throughput where ue_id != 'slice-total' and ts_ms > ((julianday('now') - 2440587.5) * 86400000 - 300000) group by sd, ue_id order by sd, ue_id;"
```

### Grafana shows five UEs but all throughput values are zero

The monitor continues to write zero-throughput summaries while UEs are attached
without user traffic. Confirm that the experiment runner reached calibration or
training and that five iperf clients exist:

```bash
ps -ef | grep -E 'experiment_runner|iperf3' | grep -v grep
tail -100 /tmp/llm_hric/experiments/five-ue-formal-v3-2-1/pilot.log
```

If the runner failed with `LLM guidance did not update`, inspect
`llm_hric_guidance.log` for truncated JSON. The current prompt asks Gemma only
for a compact action object; preferred ratios and PRB bounds are derived and
validated locally before A1 publication.

The summary tables are derived from raw FlexRIC SM indication tables. If summary rows are missing or stale, check that raw MAC/RLC/PDCP/GTP samples are still arriving:

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select 'mac' sm, count(*) rows, max(ts_ms) latest_ts from mac_ue_stats_raw union all select 'rlc', count(*), max(ts_ms) from rlc_rb_stats_raw union all select 'pdcp', count(*), max(ts_ms) from pdcp_rb_stats_raw union all select 'gtp', count(*), max(ts_ms) from gtp_tunnel_stats_raw;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select * from mac_ue_stats_raw order by ts_ms desc limit 5;"
```

The monitor uses MAC indication timestamps to align each RIC-side observation window. `ue_slice_throughput.ts_ms`, `network_state.ts_ms`, and `ue_metric_provenance.ts_ms` therefore refer to the same observed RAN interval. This timestamp alignment does not control the gNB clock: the RAN runs continuously, while the monitor independently samples and summarizes the indications it receives.

```bash
sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select ts_ms, sd, ue_id, dl_th_mbps, ul_th_mbps from ue_slice_throughput order by ts_ms desc limit 10;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select ts_ms,sd,ue_count,dl_th_mbps,prb_used,dl_buffer_bytes,wb_cqi,bler,channel_valid from network_state order by ts_ms desc limit 10;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select ts_ms,rnti,rbid,txbuf_occ_bytes from rlc_rb_stats_raw order by ts_ms desc limit 10;"
```

If this query shows fewer than 5 UE IDs, first check whether the FlexRIC SM monitor is receiving indication data and whether every RNTI is mapped:

```bash
grep -Ei "unknown RNTI|MAC indication|RLC indication|PDCP indication|GTP indication" \
  /tmp/llm_hric/e2e_rfsim/logs/flexric_sm_monitor.log | tail -100
```

Add any unknown RNTI to `monitor.rnti_slice_map` in `config.yaml`. Then restart with the current 5 UE config:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
./run_e2e_rfsim.sh stop
sudo -v
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```

If the DB has all UE IDs but Grafana still does not show them, restart Grafana:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana
docker compose restart grafana
```

If the same `ue_id` appears under two different `sd` values, an old monitor process or stale DB content is mixing two UE-to-slice mappings. Stop duplicate monitors and clear the local DB before restarting:

```bash
pkill -f "llm_hric.monitor_bridge" || true
pkill -f "xapp_mac_rlc_pdcp_gtp_moni.py" || true
sudo rm -f /tmp/llm_hric/llm_hric.sqlite3*
sudo -v
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 DDPG_APPLY=1 ./run_e2e_rfsim.sh start
```
