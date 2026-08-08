# OAI / FlexRIC / LLM-hRIC 修改与合规边界

本文档描述当前工作树相对基础版 OAI/FlexRIC 的实际修改、修改原因、实现方案、运行语义和标准边界。它以源码为准，不把某一次实验日志当作永久事实，也不声称当前原型已经通过 3GPP 或 O-RAN conformance test。

## 1. 审计基线与范围

本文档审计时的基线为：

- OAI branch：`develop`
- OAI HEAD：`cb0e501293a7a4664f09322136d7ff29a39343dc`
- FlexRIC submodule HEAD：`340c36bc8385dc0b9f5d8b2d51d16ff288acee79`
- FlexRIC version description：`v2.0.0-204-g340c36bc`

这里的“修改”包括未提交的 tracked diff 和当前 LLM-hRIC 目录中的新增文件。可用以下命令重新审计：

```bash
cd /home/ics1/openairinterface5g
git status --short
git diff --stat
git submodule status openair2/E2AP/flexric

cd openair2/E2AP/flexric
git status --short
git diff --stat
```

本文档覆盖四个边界：

1. OAI gNB 的 E2SM-RC 接收和 DL MAC slice scheduler。
2. FlexRIC RC actuator、monitor xApp、KPM collector 和可靠性修复。
3. LLM-hRIC 的 rAPP、A1-like policy、异步 RL controller/learner、数据库和 GUI。
4. 5 UE rfsim/core/traffic 实验配置。

`semantic_obj_dec_oai/` 和 52 PRB USRP 示例配置不属于当前 LLM-hRIC PRB slicing 主链路，不在本文档中作为核心修改说明。

## 2. 修改后的端到端架构

当前控制与观测链路为：

```text
OAI gNB MAC/RLC/PDCP/GTP counters
        | FlexRIC internal SM indications
        v
xapp_mac_rlc_pdcp_gtp_moni.py
        | raw counters -> 50 ms slice summaries
        v
SQLite: raw tables + network_state + ue_slice_throughput
        |                         |
        |                         +--> Grafana monitor
        v
Gemma/API rAPP (10 s) -> A1-like HTTP/SQLite policy
                                  |
                                  v
near-RT controller (100 ms) -> serving Actor + policy guardrails
                                  |
                                  v
persistent xapp_rc_slice_ctrl -> E42/SCTP -> nearRT-RIC
                                  |
                                  v
E2AP/E2SM-RC Style 2 Action 6 -> OAI gNB DL MAC scheduler

controller -> raw replay table -> asynchronous learner
                                      |
                                      v
                              validated Actor snapshot
                                      |
                                      v
                              atomic serving-Actor swap
```

并行存在一条审计链路：

```text
OAI E2SM-KPM RAN function -> xapp_kpm_moni -> kpm_measurements_raw
```

这条 KPM 链路用于标准 measurement 审计和独立 KPI 校验。当前 RL 的主状态仍由 FlexRIC MAC/RLC/PDCP/GTP internal SM 派生，不应把两者混称为同一个 KPM monitor。

RAN 与 RIC 时间周期相互独立：50 ms summary、100 ms controller tick 和 10 s rAPP 周期都只是 RIC 侧读取/决策周期，不改变 gNB slot、PHY、MAC scheduler 或 RLC 处理周期。

## 3. 修改清单总览

| 层级 | 主要文件 | 修改原因 | 实现结果 |
|---|---|---|---|
| OAI RC RAN function | `ran_func_rc.c`, `rc_ctrl_service_style_2.c/.h` | 基础版没有可应用 S-NSSAI PRB policy 的 RC handler | 暴露并解析 Style 2 / Action 6，写入 MAC policy |
| OAI DL scheduler | `gNB_scheduler_dlsch_default_policies.c/.h`, `nr_mac_gNB.h`, `main.c` | 基础 PF scheduler 不按 slice 限制 new-data PRB | 新增按 S-NSSAI dedicated ratio 的 DL scheduler |
| OAI test config | gNB/UE rfsim config | core、gNB、UE 的 PLMN/DNN/S-NSSAI 必须一致 | 支持 `1:ffffff` 和 `1:123456` 两个 slice |
| RFSimulator build | `radio/rfsimulator/simulator.cpp` | `paramdef_t` 宏初始化的 C++ 编译兼容问题 | 改为显式 helper 构造；不改变 RF/channel 算法 |
| FlexRIC RC xApp | `examples/xApp/c/rc_slice_ctrl/` | 去除硬编码 slice，避免每 100 ms 重建 E42 session | JSON policy + persistent Unix-socket actuator |
| FlexRIC monitor | `xapp_mac_rlc_pdcp_gtp_moni.py`, `ric_sm_db_writer.py` | 原示例只打印 indication，不能供 AI 重算状态 | raw tables、时间对齐 summary、provenance |
| E2SM-KPM collector | `xapp_kpm_moni.c` | 原 monitor 不写 LLM-hRIC DB | 长格式 `kpm_measurements_raw` |
| FlexRIC reliability | E42 sync、node registry、PDCP、SQLite files | 长运行中的 duplicate node、late ACK、counter snapshot、DB lock 会 abort | 超时返回失败、忽略 late ACK、替换重复节点、移除错误断言、DB 重试 |
| LLM-hRIC | `llm_hric/*.py` | 建立 non-RT guidance 与 near-RT control 闭环 | Gemma/API、A1-like、24 维状态、异步 Actor-Learner |
| 实验与 GUI | `experiments/`, `grafana/`, launcher | 需要可重复 5 UE paired-seed 实验和可观测性 | traffic scenarios、manifest、分析报告、Grafana |

## 4. OAI gNB：E2SM-RC Slice Control

### 4.1 修改原因

基础版 `ran_func_rc.c` 支持已有的 RC control 路径，但没有把 slice-level RRM policy ratio 连接到 NR MAC scheduler。xApp 即使构造 S-NSSAI 和 PRB ratio，也不能改变 gNB 的实际调度状态。

因此需要完成两个连接点：

1. 在 RC RAN Function Definition 中声明控制能力。
2. 在收到 control request 后解析 RAN parameter tree，并以线程安全方式更新 MAC policy。

### 4.2 RAN Function Definition

修改文件：

- `openair2/E2AP/RAN_FUNCTION/O-RAN/ran_func_rc.c`
- `openair2/E2AP/RAN_FUNCTION/O-RAN/rc_ctrl_service_style_2.c`
- `openair2/E2AP/RAN_FUNCTION/O-RAN/rc_ctrl_service_style_2.h`
- `openair2/E2AP/RAN_FUNCTION/CMakeLists.txt`

`fill_rc_control()` 的 control style 数量由 1 扩展为 2，并新增：

```text
RIC Style Type: 2
Style Name: Radio Resource Allocation Control
Control Header: Format 1
Control Message: Format 1
Action ID: 6
Action Name: Slice-level PRB quota
Top-level parameter: RRM Policy Ratio List
```

使用的 RAN parameter ID 为：

```text
1  RRM Policy Ratio List
3  RRM Policy
4  RRM Policy Member List
6  PLMN Identity
7  S-NSSAI
8  SST
9  SD
10 Min PRB Policy Ratio
11 Max PRB Policy Ratio
12 Dedicated PRB Policy Ratio
```

构建系统把 `rc_ctrl_service_style_2.c` 加入 `e2_ran_func_cuup` 和 `e2_ran_func_du_cucp_cuup`。实际应用函数只在包含 `NGRAN_GNB_DU` 的构建中更新 MAC；非 DU 构建会记录 warning 并忽略调度更新。

### 4.3 Control 解析和校验

收到 RC control 后，`write_ctrl_rc_sm()` 保留原有 Style 1 / Action 2 路径，并增加以下分派：

```text
ric_style_type == 2 && ctrl_act_id == 6
  -> apply_rc_ctrl_style_2_slice_level_prb_quota()
```

解析器验证：

- control header/message 必须为 Format 1。
- top-level message 必须恰好包含一个 `RRM Policy Ratio List`。
- policy 数量不得超过 `MAX_NUM_SLICES`。
- 每个 group 必须包含 RRM policy member、min、max、dedicated 四项。
- SST 必须为 `0..255`，SD 必须为 `0..0xffffff`。
- ratio 必须满足 `0 <= min <= dedicated <= max <= 100`。

SST/SD 同时接受 integer 和 octet-string 表示。合法 policy 被转换为 `nr_dl_slice_policy_t`，再通过 `nr_mac_set_dl_slice_policies()` 在 scheduler lock 下原子替换整组 policy。

### 4.4 当前 RC 实现的实际边界

- PLMN 在 xApp policy 中被校验并编码，但 gNB parser 当前只读取 member 中的 S-NSSAI；MAC matching 只比较 SST/SD。
- gNB parser 校验每条 policy，但不校验所有 `dedicated` 之和；当前 C xApp 会拒绝总和大于 100 的 policy。
- control outcome 只返回 FlexRIC 的通用 success 类型，没有返回逐 slice 的详细执行 outcome。
- 当前 handler 使用 `RC.nrmac[0]`，即只更新第一个 NR MAC instance。
- Style/Action 和 RAN parameter tree 对齐 E2SM-RC 的资源分配控制思路，但这是 OAI/FlexRIC prototype profile，尚未经过第三方互操作或 O-RAN conformance 认证。

## 5. OAI gNB：DL MAC Slice Scheduler

### 5.1 修改原因

基础版 `nr_dl_proportional_fair()` 在所有候选 UE 间统一按 PF weight 调度，无法表达“某个 S-NSSAI 可使用多少 DL PRB”。仅接收 RC policy 而不修改 scheduler，不会产生真实吞吐差异。

### 5.2 Policy 状态

`nr_mac_gNB.h` 新增：

```c
typedef struct {
  bool active;
  nssai_t nssai;
  uint8_t min_prb_ratio;
  uint8_t max_prb_ratio;
  uint8_t dedicated_prb_ratio;
} nr_dl_slice_policy_t;
```

`gNB_MAC_INST` 保存：

- policy array 和 count。
- enable flag。
- 每个 policy 的累计 scheduled PRB counter。
- 周期日志所需的 last-log frame。

`mac_top_init_gNB()` 把默认 DL RB allocator 从 `nr_dl_proportional_fair` 切换为 `nr_dl_slice_prb_policy`。当没有 active policy 时，新函数回退到原 PF 行为。

### 5.3 调度顺序

当前每次 DL scheduling 的顺序为：

1. 先建立原 PF priority order。
2. HARQ retransmission 优先，不受 slice new-data budget 限制。
3. TA/beam-switch 等无 RLC payload 的 MAC CE 继续处理。
4. 对每个 active slice，按 `available_rb * dedicated_prb_ratio / 100` 计算 new-data budget。
5. 只在匹配 SST/SD 的候选 UE 中按 PF 顺序分配该 budget。
6. 未匹配任何 controlled slice 的 UE 走 best-effort PF。

因此控制的是 **DL new-data scheduled PRB budget**，不是 HARQ 重传、UL PRB、固定物理资源切片或硬吞吐保证。

### 5.4 min/max/dedicated 的真实语义

当前实现中：

- `dedicated_prb_ratio` 实际参与每次调度预算计算。
- `min_prb_ratio` 和 `max_prb_ratio` 被解析、保存和校验，但不直接驱动 scheduler。
- policy ratio 是目标预算，不等同于实际 PRB usage，也不等同于 Mbps SLA。
- 受 MCS、CQI、BLER、HARQ、RLC backlog、最小 RB block、TDD slot 和其他控制开销影响，TH 不会严格按 ratio 线性变化。
- 某个 controlled slice 没有 backlog 时，其未用 budget 当前不会自动借给另一个 controlled slice；最终 best-effort 阶段也会排除所有匹配 active policy 的 UE。
- policy 按数组顺序执行；在重传或资源碎片较多时，后面的 policy 可能看到更少 free RB。

这些限制是解释实验结果时必须保留的实现事实。

### 5.5 可观测性

每次实际 new-data allocation 会累计 `rbSize`，并周期输出：

```text
slice_prb sst=1 sd=ffffff dedicated=60 used=...
slice_prb sst=1 sd=123456 dedicated=40 used=...
```

counter 在新 policy 整组替换时清零。该日志用于审计，不改变 scheduler 行为。

## 6. OAI rfsim 与 Slice 配置

### 6.1 gNB 配置

`ci-scripts/conf_files/gnb.sa.band78.106prb.rfsim.conf` 的改动为：

- PLMN 保持 `208/99`。
- `snssaiList` 从单个 `1:ffffff` 扩展为 `1:ffffff` 和 `1:123456`。
- N2/N3 gNB address 调整为当前 host/container topology 使用的 `192.168.71.129`。
- 增加 nearRT-RIC address 和 FlexRIC SM directory 配置。

该修改原因是让 gNB advertised/supported slice、Docker core、UE request 和 E2 agent 地址一致。IP 是本机实验拓扑值，不可直接复制到其他机器或商业 RU 环境。

### 6.2 UE 配置

`nrue.uicc.conf` 明确 PDU session ID、DNN、SST 和 SD。新增的双 slice 示例配置把：

- Slice A：`sst=1, sd=0xffffff`, DNN `oai`
- Slice B：`sst=1, sd=0x123456`, DNN `openairinterface`

关联到不同 subscriber/PDU subnet。

`run_e2e_rfsim.sh` 在运行时为 5 UE 生成独立配置：

| UE | IMSI | DNN | S-NSSAI | 预期 PDU subnet |
|---|---|---|---|---|
| ue1 | `208990100001100` | `oai` | `1:ffffff` | `12.1.1.0/24` |
| ue2 | `208990100001101` | `oai` | `1:ffffff` | `12.1.1.0/24` |
| ue3 | `208990100001102` | `oai` | `1:ffffff` | `12.1.1.0/24` |
| ue4 | `208990100001103` | `openairinterface` | `1:123456` | `12.1.2.0/24` |
| ue5 | `208990100001104` | `openairinterface` | `1:123456` | `12.1.2.0/24` |

每个 nrUE 在独立 Linux network namespace 中运行。namespace、TUN、ext-dn route 和 iperf/tc shaping 都是测试设施，不是 OAI RAN 协议修改。

### 6.3 RFSimulator 修改

`radio/rfsimulator/simulator.cpp` 把 `STRINGPARAM`、`DOUBLEPARAM` 等宏式 `paramdef_t` 初始化替换为显式 C++ helper 构造函数。修改原因是解决当前 C++ toolchain 对 union/aggregate 初始化的编译兼容问题。

该 diff 不改变：

- IQ sample transport。
- AWGN/TDL channel calculation。
- gNB/UE socket timing。
- RAN scheduler 周期。

动态 channel controller 和 telnet `chanmod` 是可选实验功能，默认 `rfsim_channel.enabled=false`，不属于当前静态 AWGN 主实验。

## 7. FlexRIC RC xApp 与 Persistent Actuator

### 7.1 修改原因

早期示例把 PLMN/S-NSSAI 和 60/40 policy 写死在 C 文件中，并且每次 decision 启动一次 xApp。这样既不能从外部动态传入 slice，也会在 100 ms loop 中反复创建 E42/SCTP session，导致 timeout、late ACK 和控制间隔抖动。

### 7.2 External JSON policy

新增 `examples/xApp/c/rc_slice_ctrl/xapp_rc_slice_ctrl.c`，接受：

```json
{
  "policies": [
    {
      "plmn": "20899",
      "sst": 1,
      "sd": "0xffffff",
      "min_prb": 10,
      "max_prb": 90,
      "dedicated_prb": 60
    },
    {
      "plmn": "20899",
      "sst": 1,
      "sd": "0x123456",
      "min_prb": 10,
      "max_prb": 90,
      "dedicated_prb": 40
    }
  ]
}
```

校验包括 PLMN digits、SST/SD range、ratio ordering 和 `sum(dedicated_prb) <= 100`。xApp 使用 RC Header Format 1、Message Format 1、Style 2、Action 6 构造相同的 RRM Policy Ratio List tree。

一次性模式：

```bash
xapp_rc_slice_ctrl --policy-file /tmp/llm_hric/current_prb_policy.json --once
```

### 7.3 Persistent mode

持久模式只建立一次 E42 session：

```bash
xapp_rc_slice_ctrl --serve /tmp/llm_hric/rc_slice_ctrl.sock
```

Python controller 通过 Unix socket 发送：

```json
{"request_id":"...","policy_file":"/tmp/llm_hric/current_prb_policy.json"}
```

返回 request ID、success、send timestamp 和 response timestamp。只有成功响应才把 action 标记为 applied。policy file 由 Python 使用临时文件加 atomic rename 写入，避免 C actuator 读取半个 JSON。

Unix socket 是同机 controller 到 xApp 的内部接口；真正跨 RIC/gNB 的控制仍通过 E42/E2AP/E2SM-RC。Unix socket 本身不是 O-RAN interface。

## 8. FlexRIC Monitor 与数据库

### 8.1 Internal SM 主状态链

基础 `xapp_mac_rlc_pdcp_gtp_moni.py` 只打印 callback latency。当前版本：

- 通过 FlexRIC Python SDK 订阅 MAC/RLC/PDCP/GTP internal SM。
- 默认 SM indication interval 为 10 ms。
- RIC 侧按配置节流并由 `RicSmDbWriter` 生成 50 ms summary。
- callback 捕获异常，避免 SWIG director exception 直接终止进程。
- 保持 callback reference，避免被 Python GC 回收。
- 使用 SCTP/E42 SDK 直接连接，不使用 TCP `connect()` 误判端口 36422。

### 8.2 Raw tables

数据先写 raw，再从 raw 派生 summary：

- `mac_ue_stats_raw`：RNTI、TBS/SDU byte counters、PRB、BLER、SNR、WB-CQI、MCS、BSR、PHR。
- `rlc_rb_stats_raw`：RNTI/RBID、TX/RX SDU bytes 和 `txbuf_occ_bytes`。
- `pdcp_rb_stats_raw`：RNTI/RBID 和 TX/RX SDU bytes。
- `gtp_tunnel_stats_raw`：RNTI、QFI、gNB/UPF TEID。
- `ue_metric_provenance`：窗口边界、吞吐来源、sample count、counter reset 和 mapping validity。

每条 raw row 同时保存 indication `ts_ms` 和本机 callback `recv_ts_ms`。未知 RNTI 仍可保留 raw 数据，但不会被错误归入某个 slice summary。

### 8.3 Summary derivation

`ue_slice_throughput` 保存 UE/S-NSSAI/RNTI 的 DL/UL throughput；`network_state` 保存 slice aggregate：

```text
ts_ms, plmn, sst, sd, ue_count,
dl_th_mbps, ul_th_mbps, prb_used, bler,
dl_buffer_bytes, wb_cqi, channel_valid
```

派生规则：

- counter delta 只使用同一 RNTI/RBID、时间递增的样本。
- counter reset/wrap 不生成负 throughput。
- DL/UL throughput source 优先级为 PDCP > RLC > MAC SDU > MAC TBS fallback。
- `txbuf_occ_bytes` 是 RLC gauge，取窗口最新 bearer 值后求和，不计算 delta。
- PRB 使用由 MAC `dl_aggr_prb` delta 聚合。
- BLER 和 CQI 取每 UE 最新有效 MAC value，再按 slice 平均。
- 所有 UE 和 slice summary 使用统一窗口结束 timestamp。
- 任一 active UE 缺少 channel sample 时，slice `channel_valid=0`，避免把缺失 CQI/BLER 当成真实 0。

当前 RNTI 到 UE/S-NSSAI 的关联优先从 gNB/nrUE attach log 自动发现，并以 `config.yaml` mapping 作为 fallback。这是实验映射机制，不是 E2SM-KPM 原生携带的 S-NSSAI relation。

### 8.4 E2SM-KPM 审计链

`xapp_kpm_moni.c` 增加 SQLite writer，把收到的 E2SM-KPM Format 1/3 measurement 写入长格式表：

```text
kpm_measurements_raw(
  ts_ms, recv_ts_ms, node_id, scope,
  ue_id_type, ue_id, measurement,
  value_type, value, unit, labels_json, reliable
)
```

OAI 当前 RAN function 可暴露的主要 measurement 包括：

- `DRB.PdcpSduVolumeDL/UL`
- `DRB.RlcSduDelayDl`
- `DRB.UEThpDl/Ul`
- `RRU.PrbTotDl/Ul`
- `CARR.PDSCHMCSDist`

这些是 E2SM-KPM measurement；MAC/RLC/PDCP/GTP raw tables 则来自 FlexRIC internal SM。当前 `network_state` 不直接由 `kpm_measurements_raw` 生成。

### 8.5 业务队列和 throughput 的含义

下行 iperf packet 到达 gNB 后，未被 MAC 调度的数据主要在 PDCP/RLC 路径排队。当前能直接观察的是 RLC `txbuf_occ_bytes`：

- RLC queue 有 backlog 不代表本 slot 一定得到 PRB。
- 没有 RLC new-data backlog 时，scheduler 通常不会为该 UE 分配 new-data PRB，但 HARQ retransmission、TA/MAC CE 等仍可能占用资源。
- UE throughput 由 PDCP/RLC/MAC cumulative byte counter 的相邻时间 delta 计算，不由 commanded PRB ratio 推算。

## 9. FlexRIC 长时间运行可靠性修改

这些修改不增加算法能力，但用于避免正式长实验因监控或控制边界条件 abort。

### 9.1 Duplicate E2 node

`reg_e2_nodes.c` 原先对重复 E2 Setup 直接 assert。当前在同一 global E2 node ID 重连时释放旧 registry entry 并替换，原因是 gNB restart/reconnect 不应终止 nearRT-RIC。

### 9.2 PDCP counter snapshot

`pdcp_data_ie.c` 移除了 `packet_count <= byte_count` 的 fatal assertions。packet/byte counter 可独立更新，snapshot 可能短暂不一致，32-bit byte counter 也可能先 wrap；monitor data 不应因此终止 gNB。下游通过 delta/provenance 丢弃 reset sample。

launcher 会检查运行时 `libpdcp_sm.so` 是否仍含旧断言字符串，并记录实际 plugin path 和 SHA256，防止 `/usr/local` 的 stale plugin 覆盖本地修复。

### 9.3 E42 sync timeout 和 late ACK

`sync_ui` 从 timeout assert 改为 bool result；subscription/control/delete 超时会返回失败并清理 active procedure。迟到的 CONTROL ACK/FAILURE 若 request ID 已过期会被忽略，而不是 assert。

同时移除了 synchronous control path 上重复的 pending-event timer，避免长时间高频 control 破坏 timer/registry 状态。

### 9.4 SQLite contention

FlexRIC SQLite wrapper 增加 5 s busy timeout，并对 `SQLITE_BUSY/LOCKED` 做有限重试。RLC TX/RX SDU byte columns 去掉错误的 32-bit upper bound，以容纳长运行 cumulative counters。Python DB 使用 WAL、`busy_timeout=5000` 和 schema migration retry。

这些修复提高可用性，但不意味着当前系统已经具备生产级 HA、transaction recovery 或 multi-node database 能力。

## 10. LLM rAPP 与 A1-like Policy

### 10.1 修改原因

near-RT controller 不应在 100 ms 路径调用大模型。rAPP 的作用是理解自然语言 intent、聚合较长时间尺度状态并生成 machine-readable guardrail；near-RT controller 在 guardrail 内根据即时状态决策。

### 10.2 LLM input

当前 rAPP 每 10 s 运行一次，并读取三个互不重叠的 10 s slice 窗口：

```text
[T-30s,T-20s], [T-20s,T-10s], [T-10s,T]
```

每个 slice/window 包含：

- S-NSSAI。
- window-end UE count。
- average DL throughput。
- average slice PRB usage share。
- average RLC DL buffer bytes。
- data availability/coverage。

prompt 还包含 active intent、priority/protected slice、SLA floor、calibrated cell capacity 和 operational/calibration PRB bounds。窗口不完整时标记 unavailable，不把 missing value 伪装成 0。

### 10.3 Provider 和 output

`LLMClient` 支持：

- 本地 Transformers Gemma E2B，当前默认 `google/gemma-4-E2B-it`、GPU、FP16/4-bit。
- OpenAI-compatible `/v1/chat/completions` API。
- 仅用于测试的 mock provider。

正式配置为 `require_real_model=true`、`allow_fallback=false`，真实模型加载失败不能静默降级为 mock/CPU。

期望输出为：

```json
{
  "control_type": "prb",
  "action": {
    "prb_ratio": {"1:ffffff": 70, "1:123456": 30},
    "confidence": 0.8,
    "reason": "short reason"
  }
}
```

parser 会提取 JSON、校验 slice key/range/sum，并将 raw Gemma ratio 投影到 calibration/operational bounds。raw action 用于审计，投影后的 `a1_target_ratio` 是 guided controller 的唯一 LLM ratio feature。

### 10.4 A1-like API

HTTP endpoints：

```text
POST/GET /a1-p/intents/{intent_id}
POST/GET /a1-p/policies/{policy_id}
```

每次更新增加 version，旧 row 设为 inactive，新 row 原子成为 active。controller 在新 policy 生成期间继续使用上一 active policy；新 policy 完整提交后才切换，不暂停控制 loop。

该 API 借用了 A1 policy management 的架构角色，但 payload、resource model、lifecycle 和 transport 没有实现 O-RAN A1-PMS/OpenAPI specification，因此只能称为 **A1-like HTTP/SQLite interface**。

## 11. Near-RT RL Controller 和异步 Learner

### 11.1 Controller input

固定两个 slice，当前 state 为 24 维：每 slice 11 个 feature，加 2 个 global feature。

每 slice：

1. 当前 DL throughput。
2. 当前 UE count。
3. 当前 slice PRB usage share。
4. `log1p(RLC DL buffer bytes)`。
5. normalized WB-CQI。
6. DL BLER。
7. channel-valid mask。
8. projected A1 target ratio；`ddpg_only` 为 0。
9. 当前 applied PRB ratio。
10. priority slice one-hot。
11. protected slice one-hot。

Global：

1. `SLA floor / calibrated cell capacity`。
2. protected slice normalized SLA deficit。

raw state 保存在 replay/audit；throughput、UE count 和 log-buffer 使用训练期 RunningMeanStd，CQI/BLER/ratio/mask/one-hot 使用固定语义缩放。统计冻结后 evaluation 不再更新。

### 11.2 Action

Actor 输出一个 `[0,1]` scalar，表示 Slice A 在可行区间中的位置。adapter 映射到 106 PRB 的互补分配：

```text
slice_a_prb + slice_b_prb = 106
ratio_a + ratio_b = 100
```

guided arm 使用 LLM/calibration bounds；pure DDPG 只使用 non-LLM operational bounds。replay 保存 RC ACK 后实际 commanded action，而不是 actor logit 或未执行 candidate。

### 11.3 Intent-aware reward

当前 reward 是 SLA-gated utility：

```text
if protected SLA satisfied:
    reward = normalized total DL TH
             + 0.5 * normalized priority-slice DL TH
             - 0.2 * mean DL BLER
             - 0.1 * action churn
             - 1.0 * priority-ratio shortfall
else:
    reward = -(1.0 + 2.0 * normalized SLA deficit)
             - BLER/churn/priority-shortfall costs
```

因此 protected slice 违反 SLA 时，不给予 total/priority throughput 正奖励；满足保护条件后，priority slice throughput 越高，reward 越高。protected slice 没有 UE 时 SLA 不适用。

raw reward 写 replay，训练时使用 discounted-return running standard deviation 缩放且不减均值。reward scaler 与 state normalizer 在达到配置 transition 数后冻结。

### 11.4 Asynchronous Actor-Learner

controller 每 100 ms 最多消费一个 fresh 50 ms summary；没有新状态时记录 stale skip，不伪造 transition。RuntimeWatchdog 在独立线程运行，不位于 controller critical path。

有效 transition 要求：

- RC ACK success。
- action-effect window coverage 达标。
- metric provenance 有效且没有 counter reset。
- policy version/action timestamp 可追溯。

controller 只写 `ddpg_replay_transitions`。独立 learner process 消费 raw transition、更新网络并周期生成 candidate Actor snapshot；candidate 通过 finite value、saturation、predicted Q、action shift 和 bounds validation 后，watcher 在 tick 边界原子替换 serving Actor。learner crash 或 candidate 被拒绝不会阻塞 RAN/control loop。

### 11.5 当前算法实现与命名偏差

代码和配置把模型标记为 `DDPG model_version=4`，Actor 确实是单 deterministic sigmoid Actor；但当前 `DDPGAgent` 同时包含：

- `critic` 和 `critic2`。
- target Q 取两个 target critic 的较小值。
- configurable target-policy noise/clipping。
- delayed Actor update。

这些是 TD3-style stabilizers。因而从算法严格定义看，当前实现不是纯粹的单 Critic DDPG。论文、报告或图表应写成“DDPG-derived actor-critic with TD3-style stabilizers”，或者移除第二 Critic/target smoothing 后再声称标准 DDPG。

## 12. GUI、Traffic 和实验框架

Grafana 使用 SQLite datasource，只读展示：

- Current intent 和 active A1 policy。
- UE ID、RNTI、S-NSSAI mapping。
- UE/slice DL throughput。
- applied PRB ratio、LLM target、RL candidate/fused action。
- A1/DDPG age、KPM update interval 和 KPM last-seen age。
- recent guidance、actions 和 learner diagnostics。

GUI 不提供 policy POST/control button。

实验 runner 支持独立 run DB/checkpoint/log/manifest、resume、paired seeds、watchdog 和结果分析。traffic scenarios 使用 ext-dn 中持续 iperf UDP 流，并通过 per-UE Linux HTB class 动态改变 rate，避免每 5 s 重启 iperf session。该 shaping 改变 offered load，不改变 RAN scheduler timer。

实验数据表包括 `experiment_runs/steps`、`traffic_events`、`controller_ticks`、`ddpg_replay_transitions`、`ddpg_runtime_state`、`ddpg_actor_versions` 和 `ddpg_learner_updates`。

这些框架代码用于研究可重复性，不属于 OAI upstream protocol stack，也不构成 O-RAN certification evidence。

## 13. 3GPP 和 Core 边界

### 13.1 对齐部分

当前测试使用标准概念：

- PLMN。
- S-NSSAI = SST + SD。
- DNN。
- UE registration 和 PDU session。
- NGAP/NAS 中已有的 slice/session selection 信息。

gNB/UE config 使用这些标准字段，未新增私有 NAS、NGAP、RRC 或 F1AP message。

### 13.2 非 3GPP 标准部分

3GPP 不规定本文实现的 `dedicated_prb_ratio` PF scheduler，也不保证 PRB ratio 对应固定吞吐。该算法是 gNB vendor/internal implementation policy。

准确表述：

> The prototype uses 3GPP S-NSSAI, DNN, and PDU-session concepts and applies an implementation-specific OAI gNB DL MAC policy per S-NSSAI.

### 13.3 Core slice 与 NSSF

当前 Docker rfsim core 通过 AMF/SMF/subscriber/DNN 配置支持两个 S-NSSAI 和两个 PDU subnet，但没有独立运行的 NSSF network function。

因此“slice 在 core 中注册/可用”准确含义是：

- subscriber 可请求该 S-NSSAI/DNN。
- AMF/SMF 配置可接受并建立相应 PDU session。
- UE 获得预期 subnet 的 PDU IP。

它不代表已经测试独立 NSSF discovery/selection procedure，也不代表完整 5GC network slicing lifecycle/orchestration。

## 14. O-RAN 合规边界

| 能力 | 当前实现 | 结论 |
|---|---|---|
| gNB 与 nearRT-RIC | 使用 FlexRIC E2AP/E42/SCTP 和 E2 agent | 架构对齐，未认证 |
| Slice control | E2SM-RC Format 1、Style 2、Action 6、RRM Policy Ratio tree | prototype profile，未做完整互操作/错误 outcome |
| Standard KPM | OAI E2SM-KPM RAN function + `xapp_kpm_moni` | 标准 measurement 路径的实现，支持集合有限 |
| RL primary state | FlexRIC MAC/RLC/PDCP/GTP internal SM | 非标准 O-RAN E2SM |
| A1 | HTTP/SQLite active policy API | A1-like，不是 A1-PMS |
| RNTI-to-slice mapping | attach log discovery + config fallback | 实验机制，不是标准 E2 identity mapping service |
| Unix actuator socket | controller 到 persistent C xApp | 本地内部接口，不是 O-RAN interface |
| Grafana/SQLite | 本地观测和实验事实库 | 非 O-RAN management plane |

推荐表述：

> We implemented an OAI/FlexRIC research prototype for E2SM-RC-style slice-level DL PRB control, with a parallel E2SM-KPM audit path and an A1-like LLM/RL policy pipeline.

不应表述为：

> The system is fully O-RAN compliant, O-RAN certified, or a complete A1-PMS/non-RT RIC implementation.

## 15. 当前已知限制和风险

- 只控制 DL new-data PRB，不控制 UL、power、MCS、handover 或 admission。
- `min/max` 不直接驱动 gNB scheduler，只有 `dedicated` 生效。
- PLMN 尚未参与 MAC policy matching。
- controlled slice 的空闲 budget 不自动跨 slice 回收。
- policy 顺序可能影响资源不足时的结果。
- RC outcome 不是逐 policy detailed result。
- RNTI/S-NSSAI mapping 依赖实验日志和 catalog。
- KPM 是并行 audit source，RL summary 仍依赖 FlexRIC internal SM。
- SQLite 是单机研究数据库，不是高可用 data platform。
- A1-like server 没有标准 A1 authentication、authorization、policy type registration 和 lifecycle。
- 当前 RL 代码包含 TD3-style 双 Critic特性，与“纯 DDPG”命名不一致。
- 106 PRB、两 slice、互补 action 是当前实验假设，不是通用多 slice allocator。
- Gemma GPU 4-bit 依赖 CUDA、PyTorch、Transformers、Accelerate 和 bitsandbytes 版本匹配。
- 测试配置中的 IP、namespace、PDU address 和 container name 不可直接移植到其他机器。

## 16. 构建与验证

### 16.1 构建 OAI gNB/UE/E2

```bash
cd /home/ics1/openairinterface5g/cmake_targets
CCACHE_DISABLE=1 ./build_oai --ninja -c --gNB --nrUE --build-e2
```

如需可选 RFSimulator telnet channel model：

```bash
CCACHE_DISABLE=1 ./build_oai --ninja -c --gNB --nrUE --build-lib telnetsrv
```

### 16.2 构建 FlexRIC

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric
CCACHE_DISABLE=1 cmake --build build --target \
  nearRT-RIC xapp_rc_slice_ctrl xapp_kpm_moni pdcp_sm -j2
```

检查运行时 PDCP plugin：

```bash
examples/xApp/python3/llm_hric/run_e2e_rfsim.sh check-pdcp-plugin
readlink -f /tmp/llm_hric/e2e_rfsim/sm/libpdcp_sm.so
```

### 16.3 Python tests

```bash
cd /home/ics1/openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3
export PYTHONPATH="$PWD"
/home/ics1/anaconda3/bin/python -m unittest discover -s llm_hric/tests -v
```

### 16.4 验证 DB 数据链

```bash
sqlite3 /tmp/llm_hric/llm_hric.sqlite3 ".tables"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select count(*), max(ts_ms) from mac_ue_stats_raw;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select count(*), max(ts_ms) from kpm_measurements_raw;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select * from network_state order by ts_ms desc limit 10;"

sqlite3 -header -column /tmp/llm_hric/llm_hric.sqlite3 \
  "select * from ue_metric_provenance order by ts_ms desc limit 10;"
```

### 16.5 验证 RC policy

应同时检查：

```text
E2 SETUP RESPONSE rx
E42 SETUP-RESPONSE rx
CONTROL ACK rx
RC Slice-level PRB quota control sent
RC slice policy SST ...
slice_prb sst=... sd=... dedicated=... used=...
```

```bash
grep -aEi "E2 SETUP|CONTROL ACK|RC slice policy|slice_prb|Slice-level PRB" \
  /tmp/llm_hric/e2e_rfsim/logs/gnb.log \
  /tmp/llm_hric/e2e_rfsim/logs/nearRT-RIC.log \
  /tmp/llm_hric/e2e_rfsim/logs/rc_slice_actuator.log
```

仅看到 A1 policy 或 JSON file 变化不能证明 RAN 已应用；必须看到 RC success/ACK 和 gNB scheduler log，并结合实际 PRB/TH 观测。

## 17. 可移植性与真实 RU

源码可以打包到其他机器重新编译，但必须同时保存 OAI commit、FlexRIC submodule commit、未提交 patch/new files 和 Python requirements。只复制 build artifact 或单独复制 `llm_hric/` 不足以重现系统。

接入商业 RU 后，理论上仍位于 gNB 侧并可继续工作的部分是：

- E2 agent 和 nearRT-RIC connection。
- E2SM-RC parser。
- gNB DL MAC slice scheduler。
- RIC-side monitor/LLM/RL/control architecture。

必须替换或重新验证的部分是：

- RFSimulator gNB/UE startup。
- Linux namespace nrUE 和 TUN traffic path。
- rfsim channel/telnet controller。
- Docker core routing 和 ext-dn HTB/iperf traffic generator。
- RU/DU fronthaul、PTP/GPS timing、RF calibration、bandwidth/numerology 和 real UE provisioning。
- commercial UE/RAN 中可靠的 UE identity、bearer 和 S-NSSAI association。

因此当前结果只能说明该架构在 OAI rfsim research environment 中可实现和评估，不能直接外推为商业 RU/真实信道性能结论。

## 18. 推荐论文表述

> We extended OAI/FlexRIC with a research prototype for S-NSSAI-aware DL PRB control. A persistent RC xApp sends RRM policy ratios through an E2SM-RC-style control path, and the OAI gNB applies the dedicated ratio in its internal DL MAC new-data scheduler. FlexRIC internal MAC/RLC/PDCP/GTP indications are stored as timestamped raw counters and derived slice summaries, while E2SM-KPM measurements are collected in parallel for audit. A Gemma/API-backed rAPP produces 10-second A1-like guidance, and an asynchronous actor-learner controller performs 100-ms near-RT decisions without changing RAN timing. The implementation uses 3GPP S-NSSAI/DNN/PDU-session concepts, but the scheduler, A1-like API, identity mapping, and parts of the control profile are prototype-level and are not claimed as certified 3GPP/O-RAN conformance implementations.
