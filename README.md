# LLM-hRIC Prototype

This repository provides a prototype implementation of **LLM-hRIC**, an LLM-empowered hierarchical RAN intelligent control framework for O-RAN.

The prototype is built on **OpenAirInterface (OAI)** and **FlexRIC** and demonstrates a closed-loop RAN control workflow in which:

- a large language model (LLM) interprets high-level network intents and generates control guidance;
- an A1-like interface transfers the guidance from the non-RT control layer to the near-RT control layer;
- a DDPG-based controller combines the LLM guidance with real-time RAN measurements;
- a FlexRIC RC xApp applies PRB allocation policies to the OAI gNB;
- Grafana visualizes network states, intents, policies, and control actions.

The current prototype focuses on **PRB allocation between network slices**.

Power, MCS, and handover are reserved in the schema for later controllers.

### Result showing
<video width="640" height="360" controls>
  <source src="/home/ics1/openairinterface5g/Recording 2026-07-24 170949.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

Project path:

```text
/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
```

---

## Architecture and Workflow

The closed-loop control workflow is:

```text
                    High-Level Network Intent
                              │
                              ▼
                    ┌───────────────────┐
                    │   LLM-based rAPP  │
                    │      (Gemma)      │
                    └─────────┬─────────┘
                              │
                         A1 Guidance
                              │
                              ▼
                    ┌───────────────────┐
                    │  A1-like Policy   │
                    │      Server       │
                    └─────────┬─────────┘
                              │
                              ▼
RAN Measurements ──► ┌───────────────────┐
                     │   DDPG Controller │
                     │   near-RT Logic   │
                     └─────────┬─────────┘
                               │
                          PRB Policy
                               │
                               ▼
                     ┌───────────────────┐
                     │ FlexRIC RC xApp   │
                     └─────────┬─────────┘
                               │ E2
                               ▼
                     ┌───────────────────┐
                     │      OAI gNB      │
                     └─────────┬─────────┘
                               │
                         RAN Execution
                               │
                               └──────────► New Measurements
```

The main components are:

- `xapp_mac_rlc_pdcp_gtp_moni.py`  
  Collects MAC/RLC/PDCP/GTP measurements from FlexRIC and stores the network state in SQLite.

- `xapp_kpm_moni`  
  Collects standard E2SM-KPM measurements.

- `llm_guidance_service.py`  
  Reads recent RAN states and network intents and generates LLM-based control guidance.

- `a1_policy_server.py`  
  Provides an A1-like interface for publishing and retrieving policies.

- `ddpg_rc_agent.py`  
  Uses RAN state and high-level guidance to determine PRB allocation.

- `xapp_rc_slice_ctrl`  
  Applies PRB policies to the OAI gNB through FlexRIC E2SM-RC control.

- `grafana/`  
  Visualizes network measurements, active intents, A1 policies, and PRB control actions.

The control loop operates across two time scales:

```text
LLM/rAPP        : long-term intent interpretation and guidance
DDPG/near-RT   : fast RAN state-based control
```

This allows the LLM to provide high-level semantic guidance while the near-RT controller reacts to rapidly changing network conditions.

---

## First-time build

Please follow OAI turitor to build your RAN environemtn

## Running the Prototype



### 1. Start the OAI/FlexRIC testbed

The provided launcher starts the OAI 5G Core, nearRT-RIC, gNB, five UEs, LLM-hRIC services, and Grafana.

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric

sudo -v

UE_MODE=multi \
UE_COUNT=5 \
START_LLM_HRIC=1 \
START_GRAFANA=1 \
DDPG_APPLY=1 \
./run_e2e_rfsim.sh start
```

Check the system status:

```bash
./run_e2e_rfsim.sh status
```

The five-UE setup contains two network slices:

```text
Slice A: S-NSSAI 1:ffffff
  UE1
  UE2
  UE3

Slice B: S-NSSAI 1:123456
  UE4
  UE5
```

---

### 2. Submit a network intent

For example, prioritize Slice A while protecting the throughput of Slice B:

```bash
curl -X POST http://127.0.0.1:8088/a1-p/intents/slice-prb-intent \
  -H 'Content-Type: application/json' \
  -d '{
        "intent":
        "prioritize slice 0xffffff while keeping slice 0x123456 above 30 Mbps",
        "valid_for_ms":1000
      }'
```

The LLM-based rAPP converts the natural-language intent and recent RAN measurements into structured guidance.

The near-RT DDPG controller then determines the executable PRB allocation and applies it through the FlexRIC RC xApp.

A new intent can be submitted without restarting the RAN.

---

### 3. Monitor the closed loop

Grafana provides real-time visualization of the LLM-hRIC control process.

Start Grafana with:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana

docker compose up -d
```

Open:

```text
http://127.0.0.1:3000
```

and select:

```text
LLM-hRIC Runtime Monitor
```

The dashboard displays:

- active network intent;
- LLM-generated A1 policy;
- UE and slice throughput;
- RAN KPI measurements;
- DDPG decisions;
- applied PRB allocation;
- controller and measurement timing.

The complete runtime path can therefore be observed as:

```text
Intent
  ↓
LLM Guidance
  ↓
A1 Policy
  ↓
DDPG Decision
  ↓
RC PRB Action
  ↓
RAN KPI Change
```

---

## Experiment

The prototype includes a five-UE network slicing experiment for evaluating hierarchical LLM/RL control.

Four control schemes can be compared:

| Method | Description |
|---|---|
| `static_equal` | Fixed equal PRB allocation |
| `llm_only` | PRB allocation directly guided by the LLM |
| `ddpg_only` | DDPG control without LLM guidance |
| `llm_guided_ddpg` | DDPG control initialized and constrained by LLM guidance |

The main comparison investigates whether high-level LLM guidance can improve the learning and adaptation of the near-RT RL controller while maintaining network performance and SLA requirements.

### Traffic scenarios

The prototype also supports multiple traffic conditions:

| Scenario | Description |
|---|---|
| `balanced` | Similar traffic demand across the two slices |
| `slice_a_heavy` | Higher and dynamically changing demand in Slice A |
| `slice_b_heavy` | Higher and dynamically changing demand in Slice B |

The traffic conditions change independently of the RIC control loop, allowing the controller to be evaluated under varying RAN loads.

---

### Run an experiment

For example, the v5 experiment configuration can be executed with:

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3

export PYTHONPATH="$PWD"
export RESULTS=/tmp/llm_hric/experiments/five-ue-traffic-v5

mkdir -p "$RESULTS"
sudo -v

START_GRAFANA=1 \
/home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results "$RESULTS" \
  --manage-stack \
  --seed 1 \
  --resume \
  --fail-fast
```

A specific traffic scenario and controller can also be selected:

```bash
START_GRAFANA=1 \
/home/ics1/anaconda3/bin/python -u \
  -m llm_hric.experiments.experiment_runner \
  --spec llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --results "$RESULTS" \
  --manage-stack \
  --seed 1 \
  --scenario balanced \
  --arm llm_guided_ddpg \
  --fail-fast
```

After the experiment, generate the results with:

```bash
/home/ics1/anaconda3/bin/python \
  -m llm_hric.experiments.analyze_results \
  --results "$RESULTS" \
  --output "$RESULTS/analysis"
```

The analysis includes metrics such as:

- slice and UE throughput;
- SLA satisfaction;
- PRB allocation;
- RL reward;
- controller timing;
- RC control latency;
- policy trajectories;
- performance under different traffic scenarios.

This experiment demonstrates the complete closed-loop workflow:

```text
OAI/FlexRIC RAN
      ↓
RAN Measurements
      ↓
LLM Intent Interpretation
      ↓
A1 Guidance
      ↓
DDPG near-RT Control
      ↓
E2SM-RC PRB Allocation
      ↓
RAN Performance
```

---

## Reference

If you use this prototype, please cite the original LLM-hRIC paper:

> L. Bao, S. Yun, J. Lee, and T. Q. S. Quek,  
> “LLM-hRIC: LLM-empowered Hierarchical RAN Intelligent Control for O-RAN,”  
> *IEEE Communications Magazine*, 2026.  
> DOI: `10.1109/MCOM.001.2500315`  
> arXiv: `2504.18062`

BibTeX:

```bibtex
@article{bao2026llmhric,
  author  = {Lingyan Bao and Sinwoong Yun and Jemin Lee and Tony Q. S. Quek},
  title   = {{LLM-hRIC}: {LLM}-empowered Hierarchical {RAN} Intelligent Control for {O-RAN}},
  journal = {IEEE Communications Magazine},
  year    = {2026},
  doi     = {10.1109/MCOM.001.2500315}
}
```