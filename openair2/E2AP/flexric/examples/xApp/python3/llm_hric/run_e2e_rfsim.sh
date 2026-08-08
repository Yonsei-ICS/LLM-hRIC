#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-/home/ics1/openairinterface5g}"
FLEXRIC_DIR="${FLEXRIC_DIR:-$ROOT_DIR/openair2/E2AP/flexric}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/cmake_targets/ran_build/build}"
CORE_DIR="${CORE_DIR:-$ROOT_DIR/ci-scripts/yaml_files/5g_rfsimulator}"
CONF_DIR="${CONF_DIR:-$ROOT_DIR/ci-scripts/conf_files}"
LLM_HRIC_DIR="${LLM_HRIC_DIR:-$FLEXRIC_DIR/examples/xApp/python3/llm_hric}"

RUN_DIR="${RUN_DIR:-/tmp/llm_hric/e2e_rfsim}"
LOG_DIR="$RUN_DIR/logs"
PID_DIR="$RUN_DIR/pids"
SM_RUNTIME_DIR="${SM_RUNTIME_DIR:-$RUN_DIR/sm}"
PDCP_SM_BUILD="${PDCP_SM_BUILD:-}"
XAPP_SDK_SO="${XAPP_SDK_SO:-$FLEXRIC_DIR/build/examples/xApp/python3/_xapp_sdk.so}"
E42_XAPP_SO="${E42_XAPP_SO:-$FLEXRIC_DIR/build/src/xApp/libe42_xapp_shared.so}"

GNB_CONF="${GNB_CONF:-$CONF_DIR/gnb.sa.band78.106prb.rfsim.conf}"
UE_COUNT="${UE_COUNT:-5}"
DEFAULT_UE_MODE=single
if (( UE_COUNT > 1 )); then
  DEFAULT_UE_MODE=multi
fi
UE_MODE="${UE_MODE:-$DEFAULT_UE_MODE}"
UE_CONF="${UE_CONF:-$CONF_DIR/nrue.uicc.conf}"
UE1_CONF="${UE1_CONF:-$CONF_DIR/nrue.uicc.conf}"
UE2_CONF="${UE2_CONF:-$CONF_DIR/nrue.uicc.slice2.conf}"
UE1_RFSIM_ADDR="${UE1_RFSIM_ADDR:-10.201.1.100}"
UE2_RFSIM_ADDR="${UE2_RFSIM_ADDR:-10.202.1.100}"
UE_CONF_DIR="${UE_CONF_DIR:-$RUN_DIR/ue_confs}"
MULTI_UE_SCRIPT="${MULTI_UE_SCRIPT:-$ROOT_DIR/tools/scripts/multi-ue.sh}"
DELETE_UE_NETNS_ON_STOP="${DELETE_UE_NETNS_ON_STOP:-0}"
RC_POLICY_FILE="${RC_POLICY_FILE:-$LLM_HRIC_DIR/sample_prb_policy.json}"

NR_SOFTMODEM="${NR_SOFTMODEM:-$BUILD_DIR/nr-softmodem}"
NR_UESOFTMODEM="${NR_UESOFTMODEM:-$BUILD_DIR/nr-uesoftmodem}"
PARAMS_LIBCONFIG="${PARAMS_LIBCONFIG:-$BUILD_DIR/libparams_libconfig.so}"
RFSIMULATOR_LIB="${RFSIMULATOR_LIB:-$BUILD_DIR/librfsimulator.so}"
OAI_CMAKE_CACHE="${OAI_CMAKE_CACHE:-$BUILD_DIR/CMakeCache.txt}"
NEARRT_RIC="${NEARRT_RIC:-$FLEXRIC_DIR/build/examples/ric/nearRT-RIC}"
SLICE_XAPP="${SLICE_XAPP:-$FLEXRIC_DIR/build/examples/xApp/c/rc_slice_ctrl/xapp_rc_slice_ctrl}"
KPM_XAPP="${KPM_XAPP:-$FLEXRIC_DIR/build/examples/xApp/c/monitor/xapp_kpm_moni}"

START_SLICE_XAPP="${START_SLICE_XAPP:-0}"
START_GRAFANA="${START_GRAFANA:-0}"
START_LLM_HRIC="${START_LLM_HRIC:-0}"
START_DDPG="${START_DDPG:-1}"
START_GUIDANCE="${START_GUIDANCE:-1}"
START_KPM_MONITOR="${START_KPM_MONITOR:-1}"
LLM_INTENT="${LLM_INTENT:-prioritize slice 0xffffff while keeping slice 0x123456 above 30 Mbps}"
DDPG_APPLY="${DDPG_APPLY:-0}"
DDPG_MODE="${DDPG_MODE:-deploy}"
DDPG_CHECKPOINT="${DDPG_CHECKPOINT:-/tmp/llm_hric/ddpg_prb_v4.pt}"
DDPG_CONTINUE_TRAINING="${DDPG_CONTINUE_TRAINING:-0}"
DDPG_ARM="${DDPG_ARM:-llm_guided_ddpg}"
DDPG_SEED="${DDPG_SEED:-1}"
RFSIM_DYNAMIC_CHANNEL="${RFSIM_DYNAMIC_CHANNEL:-0}"
RFSIM_CHANNEL_MODEL="${RFSIM_CHANNEL_MODEL:-TDL_A}"
RFSIM_CHANNEL_NOISE_DB="${RFSIM_CHANNEL_NOISE_DB:--35}"
RFSIM_CHANNEL_FORGETFACT="${RFSIM_CHANNEL_FORGETFACT:-0.9}"
RFSIM_TELNET_PORT="${RFSIM_TELNET_PORT:-9090}"
CONDA_BASE="/home/ics1/anaconda3"
LLM_HRIC_PYTHON="$CONDA_BASE/bin/python"
USE_SUDO="${USE_SUDO:-1}"
CORE_SERVICES="${CORE_SERVICES:-mysql oai-amf oai-smf oai-upf oai-ext-dn}"

GNB_START_WAIT_S="${GNB_START_WAIT_S:-15}"
RIC_START_WAIT_S="${RIC_START_WAIT_S:-3}"
UE_START_WAIT_S="${UE_START_WAIT_S:-20}"
CORE_WAIT_TIMEOUT_S="${CORE_WAIT_TIMEOUT_S:-120}"

mkdir -p "$LOG_DIR" "$PID_DIR"

prepare_sm_runtime_dir() {
  mkdir -p "$SM_RUNTIME_DIR"
  local plugin
  for plugin in /usr/local/lib/flexric/lib*_sm.so; do
    [[ -e "$plugin" ]] || continue
    ln -sfn "$plugin" "$SM_RUNTIME_DIR/$(basename "$plugin")"
  done
  local pdcp_plugin="$PDCP_SM_BUILD"
  if [[ -z "$pdcp_plugin" ]]; then
    local candidate
    for candidate in \
      "$BUILD_DIR/openair2/E2AP/flexric/src/sm/pdcp_sm/libpdcp_sm.so" \
      "$FLEXRIC_DIR/build/src/sm/pdcp_sm/libpdcp_sm.so" \
      /usr/local/lib/flexric/libpdcp_sm.so; do
      if [[ -e "$candidate" ]]; then
        pdcp_plugin="$candidate"
        break
      fi
    done
  fi
  if [[ -z "$pdcp_plugin" ]]; then
    echo "missing libpdcp_sm.so; rebuild FlexRIC or set PDCP_SM_BUILD" >&2
    exit 1
  fi
  require_file "$pdcp_plugin"
  local resolved_pdcp_plugin
  resolved_pdcp_plugin="$(realpath -e "$pdcp_plugin")"
  local stale_assertion
  for stale_assertion in \
    'rb->txpdu_pkts <= rb->txpdu_bytes' \
    'rb->rxpdu_pkts <= rb->rxpdu_bytes' \
    'rb->txsdu_pkts <= rb->txsdu_bytes' \
    'rb->rxsdu_pkts <= rb->rxsdu_bytes'; do
    if LC_ALL=C strings "$resolved_pdcp_plugin" | grep -F -- "$stale_assertion" >/dev/null; then
      echo "refusing stale PDCP SM plugin containing fatal counter assertion:" >&2
      echo "  plugin: $resolved_pdcp_plugin" >&2
      echo "  assertion: $stale_assertion" >&2
      echo "rebuild it with:" >&2
      echo "  cd $FLEXRIC_DIR && CCACHE_DISABLE=1 cmake --build build --target pdcp_sm -j2" >&2
      exit 1
    fi
  done
  local pdcp_sha256
  pdcp_sha256="$(sha256sum "$resolved_pdcp_plugin" | awk '{print $1}')"
  echo "PDCP SM plugin: $resolved_pdcp_plugin"
  echo "PDCP SM SHA256: $pdcp_sha256"
  ln -sfn "$resolved_pdcp_plugin" "$SM_RUNTIME_DIR/libpdcp_sm.so"
}

validate_xapp_sdk() {
  require_file "$XAPP_SDK_SO"
  require_file "$E42_XAPP_SO"
  local binary
  for binary in "$XAPP_SDK_SO" "$E42_XAPP_SO"; do
    if LC_ALL=C grep -aFq 'rb->txpdu_pkts <= rb->txpdu_bytes' "$binary"; then
      echo "FlexRIC Python SDK still contains the fatal PDCP counter assertion: $binary" >&2
      echo "rebuild it with: CCACHE_DISABLE=1 cmake --build $FLEXRIC_DIR/build --target xapp_sdk -j\$(nproc)" >&2
      exit 1
    fi
  done
}

if [[ "$USE_SUDO" == "1" ]]; then
  SUDO_CMD=(sudo -E)
else
  SUDO_CMD=()
fi

usage() {
  cat <<EOF
Usage: $(basename "$0") {start|gui|stop|cleanup|status|logs|check-pdcp-plugin}

Environment overrides:
  ROOT_DIR=$ROOT_DIR
  GNB_CONF=$GNB_CONF
  UE_MODE=$UE_MODE                       # single|multi
  UE_COUNT=$UE_COUNT
  UE_CONF=$UE_CONF
  UE1_CONF=$UE1_CONF
  UE2_CONF=$UE2_CONF
  START_SLICE_XAPP=$START_SLICE_XAPP
  START_GRAFANA=$START_GRAFANA
  START_LLM_HRIC=$START_LLM_HRIC
  START_DDPG=$START_DDPG
  START_GUIDANCE=$START_GUIDANCE
  START_KPM_MONITOR=$START_KPM_MONITOR
  DDPG_APPLY=$DDPG_APPLY
  DDPG_MODE=$DDPG_MODE                   # train|deploy
  DDPG_CHECKPOINT=$DDPG_CHECKPOINT
  DDPG_CONTINUE_TRAINING=$DDPG_CONTINUE_TRAINING
  DDPG_ARM=$DDPG_ARM                   # static_equal|llm_only|ddpg_only|llm_guided_ddpg
  DDPG_SEED=$DDPG_SEED
  RFSIM_DYNAMIC_CHANNEL=$RFSIM_DYNAMIC_CHANNEL # 1 enables UE-side TDL/chanmod + telnet
  RFSIM_CHANNEL_MODEL=$RFSIM_CHANNEL_MODEL
  RFSIM_CHANNEL_NOISE_DB=$RFSIM_CHANNEL_NOISE_DB
  RFSIM_CHANNEL_FORGETFACT=$RFSIM_CHANNEL_FORGETFACT
  LLM_HRIC_PYTHON=$LLM_HRIC_PYTHON       # fixed Anaconda base interpreter
  USE_SUDO=$USE_SUDO
  RUN_DIR=$RUN_DIR

Examples:
  $(basename "$0") start
  UE_MODE=multi UE_COUNT=5 START_LLM_HRIC=1 START_GRAFANA=1 $(basename "$0") start
  START_LLM_HRIC=1 START_SLICE_XAPP=1 START_GRAFANA=1 $(basename "$0") start
  $(basename "$0") gui
  $(basename "$0") status
  $(basename "$0") logs
  $(basename "$0") check-pdcp-plugin
  $(basename "$0") stop
  $(basename "$0") cleanup                    # also terminate unmanaged stale OAI/FlexRIC processes
EOF
}

require_file() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "missing required path: $path" >&2
    exit 1
  fi
}

require_e2_enabled() {
  require_file "$OAI_CMAKE_CACHE"
  if ! grep -q '^E2_AGENT:STRING=ON$' "$OAI_CMAKE_CACHE"; then
    echo "OAI build has E2_AGENT disabled: $OAI_CMAKE_CACHE" >&2
    echo "reconfigure with: cd $ROOT_DIR/cmake_targets && ./build_oai --ninja --gNB --nrUE --build-e2" >&2
    exit 1
  fi
}

ensure_sudo() {
  if [[ "$USE_SUDO" == "1" ]]; then
    echo "checking sudo credentials for gNB/nrUE"
    sudo -v
  fi
}

warn_tun_permissions() {
  if [[ "$USE_SUDO" != "1" ]]; then
    echo "warning: USE_SUDO=0; nrUE can sync/register but may fail to create oaitun without CAP_NET_ADMIN" >&2
  fi
  if [[ ! -e /dev/net/tun ]]; then
    echo "warning: /dev/net/tun is not present; UE user-plane TUN and iperf will not work until TUN is available" >&2
  fi
}

fix_llm_hric_db_permissions() {
  mkdir -p /tmp/llm_hric
  chmod a+rwx /tmp/llm_hric 2>/dev/null || sudo -n chmod a+rwx /tmp/llm_hric 2>/dev/null || true
  if compgen -G "/tmp/llm_hric/llm_hric.sqlite3*" >/dev/null; then
    if [[ "$USE_SUDO" == "1" ]]; then
      sudo -n chown "$USER:$(id -gn)" /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
      sudo -n chmod a+rw /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
    fi
    chmod a+rw /tmp/llm_hric/llm_hric.sqlite3* 2>/dev/null || true
  fi
}

run_bg() {
  local name="$1"
  local cwd="$2"
  shift 2
  local pid_file="$PID_DIR/$name.pid"
  local log_file="$LOG_DIR/$name.log"

  if [[ -f "$pid_file" ]] && pid_running "$(cat "$pid_file")"; then
    echo "$name already running with pid $(cat "$pid_file")"
    return
  fi

  echo "starting $name"
  if [[ "${1:-}" == "sudo" ]]; then
    shift
    while [[ "${1:-}" == -* ]]; do
      shift
    done
    sudo -E bash -c '
      cwd="$1"
      pid_file="$2"
      log_file="$3"
      shift 3
      cd "$cwd"
      nohup "$@" >"$log_file" 2>&1 < /dev/null &
      echo $! >"$pid_file"
    ' bash "$cwd" "$pid_file" "$log_file" "$@"
  else
    (
      cd "$cwd"
      nohup "$@" >"$log_file" 2>&1 < /dev/null &
      echo $! >"$pid_file"
    )
  fi
  echo "  pid: $(cat "$pid_file")"
  echo "  log: $log_file"
}

pid_running() {
  local pid="$1"
  if kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  if [[ "$USE_SUDO" == "1" ]] && sudo -n kill -0 "$pid" 2>/dev/null; then
    return 0
  fi
  return 1
}

process_pattern() {
  local name="$1"
  case "$name" in
    nearRT-RIC) echo "$NEARRT_RIC" ;;
    gnb) echo "$NR_SOFTMODEM" ;;
    nrUE) [[ "$UE_MODE" == "single" ]] && echo "$NR_UESOFTMODEM" || return 1 ;;
    nrUE[0-9]*) echo "$NR_UESOFTMODEM.*nrue\.uicc\.ue${name#nrUE}\.conf" ;;
    flexric_sm_monitor) echo "$FLEXRIC_DIR/examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni\.py" ;;
    kpm_monitor) echo "$KPM_XAPP" ;;
    rc_slice_actuator) echo "$SLICE_XAPP.*--serve" ;;
    llm_hric_guidance) echo "llm_hric\.llm_guidance_service" ;;
    a1_policy_server) echo "llm_hric\.a1_policy_server" ;;
    llm_hric_ddpg) echo "llm_hric\.ddpg_rc_agent" ;;
    *) return 1 ;;
  esac
}

discover_processes() {
  local name="$1"
  case "$name" in
    gnb)
      pgrep -x nr-softmodem 2>/dev/null || true
      return
      ;;
    nrUE)
      [[ "$UE_MODE" == "single" ]] && pgrep -x nr-uesoftmodem 2>/dev/null || true
      return
      ;;
    nrUE[0-9]*)
      local suffix="nrue.uicc.ue${name#nrUE}.conf"
      local pid args
      while read -r pid; do
        [[ -z "$pid" ]] && continue
        args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
        [[ "$args" == *"$suffix"* ]] && echo "$pid"
      done < <(pgrep -x nr-uesoftmodem 2>/dev/null || true)
      return
      ;;
  esac
  local pattern
  pattern="$(process_pattern "$name")" || return 0
  pgrep -f -- "$pattern" 2>/dev/null || true
}

refresh_pid_file() {
  local name="$1"
  local pid_file="$PID_DIR/$name.pid"
  local candidates=()
  mapfile -t candidates < <(discover_processes "$name")
  if (( ${#candidates[@]} == 1 )); then
    echo "${candidates[0]}" >"$pid_file"
    return 0
  fi
  if (( ${#candidates[@]} > 1 )); then
    echo "$name has multiple candidate processes: ${candidates[*]}" >&2
    return 2
  fi
  if [[ -f "$pid_file" ]] && pid_running "$(cat "$pid_file")"; then
    return 0
  fi
  return 1
}

ensure_no_unmanaged_process() {
  local name="$1"
  local comm="$2"
  local managed_pid=""
  local pid_file="$PID_DIR/$name.pid"
  if [[ -f "$pid_file" ]]; then
    refresh_pid_file "$name" >/dev/null 2>&1 || true
    managed_pid="$(cat "$pid_file")"
  fi
  local unmanaged=()
  local pid
  while read -r pid; do
    [[ -z "$pid" ]] && continue
    if [[ -z "$managed_pid" || "$pid" != "$managed_pid" ]]; then
      unmanaged+=("$pid")
    fi
  done < <(pgrep -x "$comm" 2>/dev/null || true)
  if (( ${#unmanaged[@]} > 0 )); then
    echo "refusing to start $name: unmanaged $comm process(es): ${unmanaged[*]}" >&2
    echo "run '$(basename "$0") cleanup' before starting a new stack" >&2
    exit 1
  fi
}

stop_pid() {
  local name="$1"
  local pid_file="$PID_DIR/$name.pid"
  refresh_pid_file "$name" >/dev/null 2>&1 || true
  # A missing PID file is the normal idempotent-stop case. Returning the
  # previous [[ ]] status here aborts cleanup under set -e.
  [[ -f "$pid_file" ]] || return 0
  local pid
  pid="$(cat "$pid_file")"
  if pid_running "$pid"; then
    echo "stopping $name pid $pid"
    kill "$pid" 2>/dev/null || sudo -n kill "$pid" 2>/dev/null || true
    sleep 2
    if pid_running "$pid"; then
      kill -9 "$pid" 2>/dev/null || sudo -n kill -9 "$pid" 2>/dev/null || true
    fi
  fi
  rm -f "$pid_file"
}

health_status() {
  local container="$1"
  docker inspect -f '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container" 2>/dev/null || true
}

wait_core() {
  local deadline=$((SECONDS + CORE_WAIT_TIMEOUT_S))
  local containers=(rfsim5g-mysql rfsim5g-oai-amf rfsim5g-oai-smf rfsim5g-oai-upf rfsim5g-oai-ext-dn)
  while (( SECONDS < deadline )); do
    local ready=1
    for c in "${containers[@]}"; do
      local s
      s="$(health_status "$c")"
      if [[ "$s" != "healthy" && "$s" != "running" ]]; then
        ready=0
        break
      fi
    done
    if [[ "$ready" == "1" ]]; then
      echo "core containers are ready"
      return
    fi
    sleep 3
  done
  echo "core did not become ready within ${CORE_WAIT_TIMEOUT_S}s" >&2
  docker compose -f "$CORE_DIR/docker-compose.yaml" ps || true
  exit 1
}

start_core() {
  echo "starting OAI 5GC docker services: $CORE_SERVICES"
  docker compose -f "$CORE_DIR/docker-compose.yaml" up -d $CORE_SERVICES
  wait_core
  if docker ps --format '{{.Names}}' | grep -qx rfsim5g-oai-ext-dn; then
    docker exec rfsim5g-oai-ext-dn ip route replace 12.1.1.0/24 via 192.168.72.134 dev eth0 || true
    docker exec rfsim5g-oai-ext-dn ip route replace 12.1.2.0/24 via 192.168.72.134 dev eth0 || true
  fi
}

start_ric() {
  require_file "$NEARRT_RIC"
  prepare_sm_runtime_dir
  ensure_no_unmanaged_process nearRT-RIC nearRT-RIC
  run_bg nearRT-RIC "$FLEXRIC_DIR" "$NEARRT_RIC" -p "$SM_RUNTIME_DIR/"
  sleep "$RIC_START_WAIT_S"
}

start_gnb() {
  require_file "$NR_SOFTMODEM"
  require_file "$PARAMS_LIBCONFIG"
  require_file "$RFSIMULATOR_LIB"
  require_e2_enabled
  require_file "$GNB_CONF"
  ensure_no_unmanaged_process gnb nr-softmodem
  prepare_sm_runtime_dir
  run_bg gnb "$RUN_DIR" "${SUDO_CMD[@]}" "$NR_SOFTMODEM" -O "$GNB_CONF" -E --rfsim \
    --e2_agent.sm_dir "$SM_RUNTIME_DIR/"
  sleep "$GNB_START_WAIT_S"
  if grep -aq "failed to bind socket" "$LOG_DIR/gnb.log"; then
    echo "gNB failed to bind N3/GTP-U; another process is probably using the configured address/port" >&2
    grep -a "failed to bind socket" "$LOG_DIR/gnb.log" | tail -1 >&2
    stop_pid gnb
    exit 1
  fi
}

start_ue_single() {
  require_file "$NR_UESOFTMODEM"
  require_file "$PARAMS_LIBCONFIG"
  require_file "$RFSIMULATOR_LIB"
  require_file "$UE_CONF"
  run_bg nrUE "$RUN_DIR" "${SUDO_CMD[@]}" "$NR_UESOFTMODEM" \
    -O "$UE_CONF" \
    -E \
    --rfsim \
    -r 106 \
    --numerology 1 \
    --band 78 \
    -C 3319680000 \
    --ssb 516 \
    --rfsimulator.[0].serveraddr 127.0.0.1
  sleep "$UE_START_WAIT_S"
}

ensure_ue_namespace() {
  local ue_id="$1"
  local ns="ue${ue_id}"
  require_file "$MULTI_UE_SCRIPT"
  if ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$ns"; then
    echo "network namespace $ns already exists"
    return
  fi
  echo "creating network namespace $ns with $MULTI_UE_SCRIPT"
  "${SUDO_CMD[@]}" "$MULTI_UE_SCRIPT" -c"$ue_id"
}

ue_rfsim_addr() {
  local ue_id="$1"
  local base_ip=$((200 + ue_id))
  echo "10.${base_ip}.1.100"
}

ue_imsi() {
  local ue_id="$1"
  printf "20899010000110%d" $((ue_id - 1))
}

ue_dnn() {
  local ue_id="$1"
  if (( ue_id <= 3 )); then
    echo "oai"
  else
    echo "openairinterface"
  fi
}

ue_sd() {
  local ue_id="$1"
  if (( ue_id <= 3 )); then
    echo "0xFFFFFF"
  else
    echo "0x123456"
  fi
}

generate_ue_conf() {
  local ue_id="$1"
  local path="$UE_CONF_DIR/nrue.uicc.ue${ue_id}.conf"
  mkdir -p "$UE_CONF_DIR"
  local channel_config=""
  if [[ "$RFSIM_DYNAMIC_CHANNEL" == "1" ]]; then
    channel_config=$(cat <<EOF_CHANNEL

channelmod = {
  max_chan = 2;
  modellist = "modellist_llm_hric";
  modellist_llm_hric = (
    {
      model_name = "rfsimu_channel_enB0";
      type = "$RFSIM_CHANNEL_MODEL";
      ploss_dB = 0;
      noise_power_dB = $RFSIM_CHANNEL_NOISE_DB;
      forgetfact = $RFSIM_CHANNEL_FORGETFACT;
      offset = 0;
      ds_tdl = 30e-9;
    }
  );
};
EOF_CHANNEL
)
  fi
  cat >"$path" <<EOF
# SPDX-License-Identifier: LicenseRef-CSSL-1.0

uicc0 = {
  imsi = "$(ue_imsi "$ue_id")";
  key = "fec86ba6eb707ed08905757b1bb44b8f";
  opc= "C42449363BBAD02B66D16BC975D77CC1";
  pdu_sessions = ({ id = 1; dnn = "$(ue_dnn "$ue_id")"; nssai_sst = 1; nssai_sd = $(ue_sd "$ue_id"); });
}

thread-pool = "-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1"

rfsimulator = (
  {
    serveraddr = "127.0.0.1";
  }
);
$channel_config
EOF
  echo "$path"
}

delete_ue_namespace() {
  local ue_id="$1"
  local ns="ue${ue_id}"
  if ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "$ns"; then
    echo "deleting network namespace $ns"
    "${SUDO_CMD[@]}" "$MULTI_UE_SCRIPT" -d"$ue_id" || true
  fi
}

start_ue_multi() {
  require_file "$NR_UESOFTMODEM"
  require_file "$PARAMS_LIBCONFIG"
  require_file "$RFSIMULATOR_LIB"
  local channel_args=()
  if [[ "$RFSIM_DYNAMIC_CHANNEL" == "1" ]]; then
    require_file "$BUILD_DIR/libtelnetsrv.so"
    require_file "$BUILD_DIR/libtelnetsrv_5Gue.so"
    channel_args=(
      --rfsimulator.[0].options chanmod
      --telnetsrv
      --telnetsrv.listenaddr 127.0.0.1
      --telnetsrv.listenport "$RFSIM_TELNET_PORT"
    )
  fi
  for ue_id in $(seq 1 "$UE_COUNT"); do
    ensure_ue_namespace "$ue_id"
    local ue_conf
    ue_conf="$(generate_ue_conf "$ue_id")"
    run_bg "nrUE${ue_id}" "$RUN_DIR" "${SUDO_CMD[@]}" ip netns exec "ue${ue_id}" "$NR_UESOFTMODEM" \
      -O "$ue_conf" \
      -E \
      --rfsim \
      -r 106 \
      --numerology 1 \
      --band 78 \
      -C 3319680000 \
      --ssb 516 \
      --rfsimulator.[0].serveraddr "$(ue_rfsim_addr "$ue_id")" \
      "${channel_args[@]}"
  done

  sleep "$UE_START_WAIT_S"
}

start_ue() {
  case "$UE_MODE" in
    single)
      start_ue_single
      ;;
    multi)
      start_ue_multi
      ;;
    *)
      echo "unsupported UE_MODE=$UE_MODE; expected single or multi" >&2
      exit 2
      ;;
  esac
}

start_llm_hric() {
  fix_llm_hric_db_permissions
  require_file "$SLICE_XAPP"
  require_file "$LLM_HRIC_PYTHON"
  if [[ "$START_KPM_MONITOR" == "1" ]]; then
    require_file "$KPM_XAPP"
  fi
  local py_path="$FLEXRIC_DIR/build/examples/xApp/python3:$FLEXRIC_DIR/examples/xApp/python3"
  local python_prefix
  python_prefix="$($LLM_HRIC_PYTHON -c 'import sys; print(sys.prefix)')"
  if [[ "$python_prefix" != "$CONDA_BASE" ]]; then
    echo "LLM-hRIC must use the Anaconda base environment; got sys.prefix=$python_prefix" >&2
    exit 1
  fi
  echo "LLM-hRIC Python: $LLM_HRIC_PYTHON (conda base)"
  local llm_cuda_lib_dir="${LLM_CUDA_LIB_DIR:-}"
  if [[ -z "$llm_cuda_lib_dir" ]]; then
    llm_cuda_lib_dir="$($LLM_HRIC_PYTHON - <<'PY'
import site
from pathlib import Path
for root in site.getsitepackages():
    candidate = Path(root) / "nvidia" / "cu13" / "lib"
    if (candidate / "libnvJitLink.so.13").exists():
        print(candidate)
        break
PY
)"
  fi
  local llm_ld_library_path="$CONDA_BASE/lib"
  if [[ -n "$llm_cuda_lib_dir" ]]; then
    llm_ld_library_path="$llm_cuda_lib_dir:$llm_ld_library_path"
  fi
  local base_path="$CONDA_BASE/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
  local base_env=(env -u VIRTUAL_ENV -u PYTHONHOME CONDA_DEFAULT_ENV=base CONDA_PREFIX="$CONDA_BASE" CONDA_SHLVL=1 PATH="$base_path" PYTHONNOUSERSITE=1)
  local llm_env=("${base_env[@]}" PYTHONUNBUFFERED=1 PYTHONPATH="$py_path" LD_LIBRARY_PATH="$llm_ld_library_path")
  local xapp_preload="$FLEXRIC_DIR/build/examples/xApp/c/monitor/RRC_MESSAGES/libasn1_nr_rrc_shared.so"
  local monitor_env=("${base_env[@]}" XAPP_DURATION=-1 PYTHONUNBUFFERED=1 PYTHONPATH="$py_path" LD_LIBRARY_PATH="$llm_ld_library_path" LD_PRELOAD="$xapp_preload")
  local monitor_cmd=("${monitor_env[@]}" "$LLM_HRIC_PYTHON" -u "$FLEXRIC_DIR/examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni.py" \
    --config "$LLM_HRIC_DIR/config.yaml" \
    --db-path /tmp/llm_hric/llm_hric.sqlite3 \
    --sm-dir "$SM_RUNTIME_DIR/")
  local ddpg_args=("${llm_env[@]}" "$LLM_HRIC_PYTHON" -u -m llm_hric.ddpg_rc_agent --mode "$DDPG_MODE" --arm "$DDPG_ARM" --seed "$DDPG_SEED" --checkpoint "$DDPG_CHECKPOINT")
  if [[ "$DDPG_APPLY" == "1" ]]; then
    ddpg_args+=(--apply)
  fi
  if [[ "$DDPG_CONTINUE_TRAINING" == "1" ]]; then
    ddpg_args+=(--continue-training)
  fi
  prepare_sm_runtime_dir
  validate_xapp_sdk
  run_bg rc_slice_actuator "$FLEXRIC_DIR" "$SLICE_XAPP" -p "$SM_RUNTIME_DIR/" --serve /tmp/llm_hric/rc_slice_ctrl.sock
  run_bg flexric_sm_monitor "$LLM_HRIC_DIR" "${monitor_cmd[@]}"
  if [[ "$START_KPM_MONITOR" == "1" ]]; then
    run_bg kpm_monitor "$FLEXRIC_DIR" env XAPP_DURATION=-1 LLM_HRIC_DB_PATH=/tmp/llm_hric/llm_hric.sqlite3 \
      "$KPM_XAPP" -p "$SM_RUNTIME_DIR/"
  fi
  if [[ "$START_GUIDANCE" == "1" ]]; then
    run_bg a1_policy_server "$LLM_HRIC_DIR" "${llm_env[@]}" "$LLM_HRIC_PYTHON" -u -m llm_hric.a1_policy_server
    sleep 1
    run_bg llm_hric_guidance "$LLM_HRIC_DIR" "${llm_env[@]}" "$LLM_HRIC_PYTHON" -u -m llm_hric.llm_guidance_service --intent "$LLM_INTENT"
  fi
  if [[ "$START_DDPG" == "1" ]]; then
    run_bg llm_hric_ddpg "$LLM_HRIC_DIR" "${ddpg_args[@]}"
  fi
}

run_slice_xapp_once() {
  require_file "$SLICE_XAPP"
  require_file "$RC_POLICY_FILE"
  echo "running slice xApp once"
  (
    cd "$FLEXRIC_DIR"
    "$SLICE_XAPP" --policy-file "$RC_POLICY_FILE" --once
  ) | tee "$LOG_DIR/xapp_rc_slice_ctrl.log"
}

start_grafana() {
  echo "starting LLM-hRIC Grafana monitor"
  fix_llm_hric_db_permissions
  docker compose -f "$LLM_HRIC_DIR/grafana/docker-compose.yml" up -d
  fix_llm_hric_db_permissions
}

start_all() {
  ensure_sudo
  warn_tun_permissions
  require_file "$CORE_DIR/docker-compose.yaml"
  start_core
  start_ric
  start_gnb
  start_ue
  if [[ "$START_SLICE_XAPP" == "1" ]]; then
    run_slice_xapp_once || echo "slice xApp failed; see $LOG_DIR/xapp_rc_slice_ctrl.log" >&2
  fi
  if [[ "$START_LLM_HRIC" == "1" ]]; then
    start_llm_hric
  fi
  if [[ "$START_GRAFANA" == "1" ]]; then
    start_grafana
  fi
  status_all
  echo
  echo "logs: $LOG_DIR"
  echo "Grafana: http://127.0.0.1:3000/d/llm-hric-runtime/llm-hric-runtime-monitor"
}

start_gui_only() {
  start_llm_hric
  start_grafana
  status_all
  echo
  echo "Grafana: http://127.0.0.1:3000/d/llm-hric-runtime/llm-hric-runtime-monitor"
}

stop_all() {
  stop_pid llm_hric_ddpg
  stop_pid llm_hric_guidance
  stop_pid a1_policy_server
  stop_pid kpm_monitor
  stop_pid flexric_sm_monitor
  stop_pid rc_slice_actuator
  stop_pid nrUE
  for ue_id in $(seq 1 "$UE_COUNT"); do
    stop_pid "nrUE${ue_id}"
  done
  stop_pid gnb
  stop_pid nearRT-RIC
  stop_pid llm_hric_monitor
  if [[ "$START_GRAFANA" == "1" ]]; then
    docker compose -f "$LLM_HRIC_DIR/grafana/docker-compose.yml" down || true
  fi
  docker compose -f "$CORE_DIR/docker-compose.yaml" down || true
  if [[ "$DELETE_UE_NETNS_ON_STOP" == "1" ]]; then
    for ue_id in $(seq 1 "$UE_COUNT"); do
      delete_ue_namespace "$ue_id"
    done
  fi
}

cleanup_all() {
  ensure_sudo
  stop_all
  echo "terminating unmanaged stale OAI/FlexRIC processes"
  local patterns=(
    "$NR_UESOFTMODEM"
    "$NR_SOFTMODEM"
    "$NEARRT_RIC"
    "$KPM_XAPP"
    "$FLEXRIC_DIR/examples/xApp/python3/xapp_mac_rlc_pdcp_gtp_moni.py"
    "$SLICE_XAPP"
  )
  local pattern
  for pattern in "${patterns[@]}"; do
    pkill -TERM -f -- "$pattern" 2>/dev/null || sudo -n pkill -TERM -f -- "$pattern" 2>/dev/null || true
  done
  sleep 2
  for pattern in "${patterns[@]}"; do
    pkill -KILL -f -- "$pattern" 2>/dev/null || sudo -n pkill -KILL -f -- "$pattern" 2>/dev/null || true
  done
  echo "removing stale FlexRIC xApp indication databases"
  find /tmp -maxdepth 1 -type f -name 'xapp_db_*' -delete 2>/dev/null \
    || sudo -n find /tmp -maxdepth 1 -type f -name 'xapp_db_*' -delete \
    || true
  rm -f "$PID_DIR"/*.pid
}

status_proc() {
  local name="$1"
  local pid_file="$PID_DIR/$name.pid"
  local refresh_rc=0
  refresh_pid_file "$name" || refresh_rc=$?
  if [[ "$refresh_rc" == "2" ]]; then
    echo "$name: duplicate processes"
  elif [[ -f "$pid_file" ]] && pid_running "$(cat "$pid_file")"; then
    echo "$name: running pid $(cat "$pid_file")"
  else
    echo "$name: stopped"
  fi
}

status_db_freshness() {
  local db=/tmp/llm_hric/llm_hric.sqlite3
  if [[ ! -f "$db" ]] || ! command -v sqlite3 >/dev/null 2>&1; then
    echo "database freshness: unavailable"
    return
  fi
  local now_ms
  now_ms="$(date +%s%3N)"
  echo "database freshness:"
  local spec table column latest age
  for spec in \
    "network_state:ts_ms" \
    "kpm_measurements_raw:ts_ms" \
    "llm_guidance:ts_ms" \
    "ddpg_actions:ts_ms"; do
    table="${spec%%:*}"
    column="${spec##*:}"
    latest="$(sqlite3 "$db" "SELECT COALESCE(MAX($column), 0) FROM $table;" 2>/dev/null || echo 0)"
    if [[ "$latest" =~ ^[0-9]+$ ]] && (( latest > 0 )); then
      age=$((now_ms - latest))
      echo "  $table: latest=$latest age_ms=$age"
    else
      echo "  $table: no data"
    fi
  done
}

status_all() {
  echo "process status:"
  status_proc nearRT-RIC
  status_proc gnb
  status_proc nrUE
  for ue_id in $(seq 1 "$UE_COUNT"); do
    status_proc "nrUE${ue_id}"
  done
  status_proc flexric_sm_monitor
  status_proc kpm_monitor
  status_proc rc_slice_actuator
  status_proc llm_hric_guidance
  status_proc a1_policy_server
  status_proc llm_hric_ddpg
  echo
  status_db_freshness
  echo
  echo "core status:"
  docker compose -f "$CORE_DIR/docker-compose.yaml" ps $CORE_SERVICES || true
}

show_logs() {
  echo "log directory: $LOG_DIR"
  ls -la "$LOG_DIR" || true
  echo
  echo "tail nearRT-RIC / gNB / nrUE:"
  for name in nearRT-RIC gnb nrUE $(for ue_id in $(seq 1 "$UE_COUNT"); do printf "nrUE%s " "$ue_id"; done) xapp_rc_slice_ctrl rc_slice_actuator flexric_sm_monitor kpm_monitor a1_policy_server llm_hric_monitor llm_hric_guidance llm_hric_ddpg; do
    if [[ -f "$LOG_DIR/$name.log" ]]; then
      echo "===== $name.log ====="
      tail -n 40 "$LOG_DIR/$name.log"
    fi
  done
}

case "${1:-}" in
  start)
    start_all
    ;;
  gui)
    start_gui_only
    ;;
  stop)
    stop_all
    ;;
  cleanup)
    cleanup_all
    ;;
  status)
    status_all
    ;;
  logs)
    show_logs
    ;;
  check-pdcp-plugin)
    prepare_sm_runtime_dir
    echo "PDCP SM plugin check passed"
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
