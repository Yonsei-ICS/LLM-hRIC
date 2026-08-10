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

LLM-hRIC path:

```text
/openair2/E2AP/flexric/examples/xApp/python3/llm_hric
```

---

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

Please set up your RAN environment, which include **5G core functions**, **OAI gNB**, **Flexric**, **grafana**, **five nrUEs**

## Running the Prototype



### 1. Start the OAI/FlexRIC testbed

The provided launcher starts the OAI 5G Core, nearRT-RIC, gNB, five UEs, LLM-hRIC services, and Grafana.

```bash
cd ~/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric

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
cd ~/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/llm_hric/grafana

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