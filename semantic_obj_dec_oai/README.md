# Semantic Object Detection over OAI 5G

Two-level (coarse + fine) object detection system over OpenAirInterface 5G NR,
with FlexRIC-based dynamic PRB allocation.

## Architecture

```
User: "Find the blue ball"
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  Server (oai-ext-dn container)                      │
│                                                     │
│  ① Task Decomposer (LLM / rule-based)               │
│     → Robot: detect "sports ball" (YOLO)            │
│     → Server: verify "blue" + "scratch" (VLM)      │
│                                                     │
│  ② VLM Verifier (GPT-4o / Ollama / CV heuristic)   │
│     → Receives crops from UE                         │
│     → If uncertain → request high-res frame          │
│                                                     │
│  ③ Writes shared state → xApp                        │
└────────────────┬────────────────────────────────────┘
                 │ TCP (port 9770)
                 │ via OAI 5G data plane (USRP)
                 ▼
┌─────────────────────────────────────────────────────┐
│  UE / Robot (oai-nr-ue machine)                     │
│                                                     │
│  ④ Camera capture (1280x720)                         │
│  ⑤ Local YOLOv8n detection                           │
│     → No target: heartbeat only (~1KB/s)             │
│     → Target found: send crop + metadata (~5KB)      │
│     → High-res requested: send full frame (~150KB)   │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│  xApp (server host, FlexRIC)                        │
│                                                     │
│  ⑥ 3-level PRB allocation:                           │
│     idle           → 10% PRB (heartbeat)             │
│     verifying      → 30% PRB (crop upload)           │
│     highres        → 60% PRB (full frame upload)     │
└─────────────────────────────────────────────────────┘
```

## Directory Structure

```
semantic_obj_dec_oai/
├── server/
│   ├── requirements.txt
│   ├── task_decomposer.py      # LLM / rule-based task decomposition
│   ├── vlm_verifier.py         # VLM fine-grained verification
│   └── semantic_server.py      # Main server process
├── ue/
│   ├── requirements.txt
│   ├── yolo_detector.py        # Local YOLO detection wrapper
│   └── semantic_ue.py          # Main UE process
└── xapp/
    └── xapp_semantic_prb.py    # FlexRIC xApp for PRB control
```

## Deployment

### 1. Server and xApp (on server host)
cd ~/oai-cn5g
docker compose up -d

#### USRP
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210.conf --gNBs.[0].min_rxtxtime 6 -E --continuous-tx

#### RF
cd ~/openairinterface5g/cmake_targets/ran_build/build
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210.conf --gNBs.[0].min_rxtxtime 6 --rfsim

cd ~/openairinterface5g/openair2/E2AP/flexric
./build/examples/ric/nearRT-RIC

docker exec -it oai-ext-dn bash

apt update
apt install python3 -y
apt install python3-pip -y
apt install -y libgl1 libglib2.0-0
cd /opt/semantic_obj_dec_oai/server
pip install -r requirements.txt
python3 semantic_server.py   --listen 0.0.0.0:9770   --task "Find a blue ball"   --shared-state-path /tmp/semantic_detection_state.json   -v



### 2. UE (on oai-nr-ue machine)
#### USRP
cd openairinterface5g/cmake_targets/ran_build/build/
sudo ./nr-uesoftmodem -r 106 --numerology 1 --band 78 -C 3619200000 --ue-fo-compensation -E --uicc0.imsi 001010000000001

#### RF
cd openairinterface5g/cmake_targets/ran_build/build/
sudo ./nr-uesoftmodem -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 001010000000001 --rfsim

cd /home/ics1/openairinterface5g/semantic_obj_dec_oai/ue
python3 semantic_ue.py   --server 192.168.70.135:9770   --camera 0   --capture-width 1280   --capture-height 720   --detect-fps 5   --weights yolov8n.pt   --device cpu   --heartbeat-interval 5   -v


### Network Prerequisites

UPF iptables rules (same as previous project):

```bash
# On oai-upf container
iptables -t nat -A PREROUTING -i tun0 -p tcp --dport 9770 \
  -j DNAT --to-destination 192.168.70.135:9770
iptables -A FORWARD -p tcp -d 192.168.70.135 --dport 9770 -j ACCEPT
iptables -t nat -A POSTROUTING -o tun0 -p tcp -s 192.168.70.135 --sport 9770 \
  -j SNAT --to-source 10.0.0.1
```

## Protocol

TCP JSON-line protocol (newline-terminated) over OAI 5G data plane:

| Direction | Type | Payload | Bandwidth |
|-----------|------|---------|-----------|
| UE→Server | heartbeat | `{ue_id}` | ~100 B |
| UE→Server | report | `{detections, crop_b64}` | ~5-50 KB |
| UE→Server | highres_frame | `{image_b64}` | ~100-200 KB |
| Server→UE | task_config | `{detect_classes, min_confidence}` | ~200 B |
| Server→UE | request_highres | `{frame_id}` | ~50 B |
| Server→UE | verified | `{match, results}` | ~200 B |

## VLM Backend Priority

1. **OpenAI API** (GPT-4o-mini) — best accuracy, requires API key
2. **Ollama local** (llava) — good accuracy, runs on server GPU
3. **CV heuristic** — color histogram + edge density, no model needed

---

## RL-based PRB Allocation

An alternative to the heuristic 3-level policy: train a **deep reinforcement learning
agent** (PPO or SAC) that learns to allocate PRBs based on channel conditions and
semantic detection state.

### Directory structure

```
rl/
├── prb_env.py       # Gymnasium environment (simulates UEs, channel, detection)
├── train.py         # PPO/SAC training with Stable-Baselines3
├── rl_agent.py      # Inference wrapper (drop-in replacement in xApp)
├── evaluate.py      # Compare RL vs heuristic vs equal baselines
├── requirements.txt # Python dependencies (torch, sb3, gymnasium)
└── checkpoints/     # Saved models (created during training)
```

### 1. Install dependencies

```bash
cd semantic_obj_dec_oai/rl
pip install -r requirements.txt
```

### 2. Train

```bash
# PPO (recommended start — stable, easy to tune)
python3 train.py --algo ppo --total-timesteps 500000 --num-ues 2 --eval-baselines

# SAC (better sample efficiency for continuous action)
python3 train.py --algo sac --total-timesteps 300000 --num-ues 2

# Monitor training in real-time
tensorboard --logdir tb_logs/
```

Key hyperparameters to tune:
- `--detection-prob` — how often UEs detect objects (higher = more contention)
- `--highres-deadline-ms` — time budget for high-res upload (tighter = harder)
- `--num-ues` — number of UEs (scales state/action space)
- Reward weights in `prb_env.py` (`w_success`, `w_delay`, `w_fairness`, `w_waste`)

### 3. Evaluate

```bash
python3 evaluate.py --model checkpoints/best_model.zip --episodes 200 --save-csv results.csv
```

### 4. Deploy in xApp

```bash
# Use the trained RL model instead of heuristic
python3 xapp/xapp_semantic_prb.py \
  --allocator rl \
  --rl-model rl/checkpoints/best_model.zip \
  --num-ues 2 \
  --apply-slice -v
```

The `--allocator rl` flag loads the trained model. If the model file is missing,
it automatically falls back to the heuristic policy.

### MDP Formulation

| Component | Description |
|-----------|-------------|
| **State** | Per-UE: [SNR, BLER, CQI, MCS, BSR, status (one-hot), need_highres, queue, wait_time] |
| **Action** | Continuous [0,1]^N — PRB share weights (softmax-normalised) |
| **Reward** | α × success_rate - β × avg_delay + γ × Jain_fairness - δ × waste_ratio |
| **Episode** | 1000 decision epochs × 100ms = 100s simulated time |

### Environment calibration

The simulator (`prb_env.py`) uses:
- 3GPP-approximate MCS→TBS mapping
- Rayleigh-like SNR random walk
- Stochastic detection state machine (idle → verifying → highres → verified)

For best real-world performance, calibrate `_MCS_EFFICIENCY`, SNR drift parameters,
and detection probabilities to match your actual OAI deployment measurements.
