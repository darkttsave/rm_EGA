#!/usr/bin/env bash
# Isolated local MQTT session and observation verification.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
host=127.0.0.1
port=${RM27_TEST_PORT:-18884}
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
run_dir="${TMPDIR:-/tmp}/rm27-b2-${UID}-${port}"
need() { command -v "$1" >/dev/null 2>&1 || { echo "缺少 $1" >&2; exit 1; }; }
[[ $# == 2 ]] || { echo '用法: start red|blue  /  send red|blue  /  broker down|up' >&2; exit 2; }
action=$1; mode=$2
broker_running() {
  [[ -f "$run_dir/broker.pid" ]] || return 1
  local pid; read -r pid < "$run_dir/broker.pid"
  [[ $pid =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null &&
    tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | grep -Fxq "$run_dir/mosquitto.conf"
}
start_broker() {
  need mosquitto
  if broker_running; then echo '测试 Broker 已运行。'; return; fi
  mosquitto -c "$run_dir/mosquitto.conf" >> "$run_dir/broker.log" 2>&1 &
  echo "$!" > "$run_dir/broker.pid"
  sleep 1
  if ! broker_running; then cat "$run_dir/broker.log" >&2; return 1; fi
  echo "本地测试 Broker 已启动：$host:$port"
}
stop_broker() {
  if broker_running; then
    local pid; read -r pid < "$run_dir/broker.pid"
    kill "$pid"
    for _ in {1..30}; do broker_running || break; sleep 0.1; done
    if broker_running; then echo 'Broker 尚未退出，请稍后重试。' >&2; return 1; fi
  fi
  rm -f "$run_dir/broker.pid"
  echo '本地测试 Broker 已停止。'
}
case "$action" in
start)
  [[ $mode == red || $mode == blue ]] || exit 2
  need python3; need mosquitto
  client="$root_dir/build/devtools/RoboMasterClient2025"
  [[ -x $client ]] || { echo '请先构建开发者客户端。' >&2; exit 1; }
  if ! mkdir -m 700 "$run_dir" 2>/dev/null; then echo "测试目录已存在：$run_dir；请先结束本次测试或更换端口。" >&2; exit 1; fi
  client_pid=''
  cleanup() {
    trap - EXIT INT TERM
    if [[ -n $client_pid ]] && kill -0 "$client_pid" 2>/dev/null; then kill "$client_pid" 2>/dev/null || true; wait "$client_pid" 2>/dev/null || true; fi
    stop_broker || true
    rm -rf -- "$run_dir"
    echo '本次客户端和临时配置已清理。'
  }
  trap cleanup EXIT; trap 'exit 130' INT; trap 'exit 143' TERM
  robot_id=1; [[ $mode == blue ]] && robot_id=101
  python3 - "$root_dir/config.example.json" "$run_dir/config.json" "$robot_id" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as source: config = json.load(source)
config.setdefault('network', {})['client_robot_id'] = int(sys.argv[3])
with open(sys.argv[2], 'w', encoding='utf-8') as target: json.dump(config, target, ensure_ascii=False, indent=2)
PY
  printf 'listener %s %s\nallow_anonymous true\n' "$port" "$host" > "$run_dir/mosquitto.conf"
  start_broker
  cd "$run_dir"
  env -u RM_CLIENT_ROBOT_ID -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
    RM_DEVTOOLS=1 RM_MQTT_BROKER="$host" RM_MQTT_PORT="$port" "$client" > "$run_dir/client.log" 2>&1 & client_pid=$!
  echo "已启动 $mode 客户端，初始身份 $robot_id。结束请关闭窗口或按 Ctrl+C。"
  set +e; wait "$client_pid"; status=$?; set -e; client_pid=''
  if ((status != 0)); then tail -n 30 "$run_dir/client.log" >&2; exit "$status"; fi
  ;;
broker)
  [[ -d $run_dir && -f $run_dir/mosquitto.conf ]] || { echo '请先启动本任务的客户端。' >&2; exit 1; }
  case "$mode" in down) stop_broker;; up) start_broker;; *) exit 2;; esac
  ;;
send)
  [[ $mode == red || $mode == blue ]] || exit 2
  need protoc; need mosquitto_pub
  broker_running || { echo '测试 Broker 未运行；请先执行 broker up。' >&2; exit 1; }
  robot_id=1; [[ $mode == blue ]] && robot_id=101
  publish() { local name=$1 data=$2; printf '%s\n' "$data" | protoc -I "$proto_dir" --encode="robomaster.$name" "$proto_dir/robomaster.proto" | mosquitto_pub -h "$host" -p "$port" -q 1 -t "$name" -s; }
  publish GameStatus 'current_round: 1 total_rounds: 3 current_stage: 4 stage_countdown_sec: 240 red_score: 2 blue_score: 1 is_paused: false game_result: 255 end_reason: 255'
  publish RobotStaticStatus "robot_id: $robot_id max_health: 300 max_heat: 100"
  publish RobotDynamicStatus 'current_health: 120 current_heat: 12.5 remaining_ammo: 9 can_remote_ammo: false'
  publish RobotModuleStatus 'power_manager: 1 modular_42mm: 0'
  publish Event 'event_id: 16'
  echo "已发送本机测试消息；身份 $mode / $robot_id。动态：血量 120、热量 12.5、剩余 9；Event 16 不关联机器人。"
  ;;
*) exit 2;;
esac
