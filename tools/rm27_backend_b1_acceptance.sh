#!/usr/bin/env bash
# Local-only game status verification, with an isolated writable config.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
host=127.0.0.1
port=${RM27_TEST_PORT:-18884}
[[ $# -ge 2 && $# -le 3 ]] || { echo '用法: start|send red|blue [--auto]' >&2; exit 2; }
action=$1
scenario=$2
[[ $scenario == red || $scenario == blue ]] || { echo '场景须为 red 或 blue' >&2; exit 2; }
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
need() { command -v "$1" >/dev/null 2>&1 || { echo "缺少 $1" >&2; exit 1; }; }
robot_id=1
[[ $scenario == blue ]] && robot_id=101
case "$action" in
start)
  [[ $# == 2 ]] || exit 2
  need mosquitto; need python3
  client="$root_dir/build/devtools/RoboMasterClient2025"
  [[ -x $client ]] || { echo '请先构建开发者客户端' >&2; exit 1; }
  temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/rm27-b1.XXXXXX")
  broker_pid=''; client_pid=''
  cleanup() {
    trap - EXIT INT TERM
    if [[ -n $client_pid ]] && kill -0 "$client_pid" 2>/dev/null; then kill "$client_pid" 2>/dev/null || true; wait "$client_pid" 2>/dev/null || true; fi
    if [[ -n $broker_pid ]] && kill -0 "$broker_pid" 2>/dev/null; then kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; fi
    rm -rf -- "$temp_dir"
    echo '本次客户端、Broker 和临时配置已清理。'
  }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  python3 - "$root_dir/config.example.json" "$temp_dir/config.json" "$robot_id" <<'PY'
import json, sys
with open(sys.argv[1], encoding='utf-8') as source:
    config = json.load(source)
config.setdefault('network', {})['client_robot_id'] = int(sys.argv[3])
with open(sys.argv[2], 'w', encoding='utf-8') as target:
    json.dump(config, target, ensure_ascii=False, indent=2)
PY
  printf 'listener %s %s\nallow_anonymous true\n' "$port" "$host" > "$temp_dir/mosquitto.conf"
  mosquitto -c "$temp_dir/mosquitto.conf" > "$temp_dir/broker.log" 2>&1 & broker_pid=$!
  sleep 1
  if ! kill -0 "$broker_pid" 2>/dev/null; then cat "$temp_dir/broker.log" >&2; exit 1; fi
  echo "独立本机 Broker $host:$port；初始身份 $robot_id。临时配置可用于切换身份。"
  cd "$temp_dir"
  env -u RM_CLIENT_ROBOT_ID -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
    RM_DEVTOOLS=1 RM_MQTT_BROKER="$host" RM_MQTT_PORT="$port" "$client" > "$temp_dir/client.log" 2>&1 & client_pid=$!
  set +e
  wait "$client_pid"; status=$?
  set -e
  client_pid=''
  if (( status != 0 )); then tail -n 30 "$temp_dir/client.log" >&2; exit "$status"; fi
  ;;
send)
  [[ $# == 2 || ${3:-} == --auto ]] || exit 2
  need protoc; need mosquitto_pub
  auto=${3:-}
  [[ $auto == --auto || -t 0 ]] || { echo '分步模式需要终端；自动模式加 --auto' >&2; exit 1; }
  pause() { if [[ $auto != --auto ]]; then printf '%s 按回车继续：' "$1"; read -r _; fi; }
  publish() {
    printf '%s\n' "$1" | protoc -I "$proto_dir" --encode=robomaster.GameStatus "$proto_dir/robomaster.proto" |
      mosquitto_pub -h "$host" -p "$port" -q 1 -t GameStatus -s
  }
  echo "测试 Broker $host:$port；客户端身份 $scenario ID $robot_id。"
  publish 'current_round: 1 total_rounds: 3 red_score: 0 blue_score: 0 current_stage: 1 stage_countdown_sec: 30 stage_elapsed_sec: 0 is_paused: false game_result: 255 end_reason: 255'
  echo '第一组：准备阶段、剩余 30 秒、比分 0:0、未暂停；结果和原因不适用。'
  pause '观察第一组。'
  publish 'current_round: 1 total_rounds: 3 red_score: 2 blue_score: 1 current_stage: 4 stage_countdown_sec: 240 stage_elapsed_sec: 60 is_paused: false game_result: 255 end_reason: 255'
  echo '第二组：比赛中、剩余 240 秒、已过 60 秒、比分 2:1、未暂停。'
  pause '观察第二组。'
  publish 'current_stage: 4 stage_countdown_sec: 239 is_paused: true game_result: 255 end_reason: 255'
  echo '第三组：比赛中、剩余 239 秒、暂停；比分等缺失并显示上次有效。'
  pause '观察第三组。'
  publish 'current_stage: 4 stage_countdown_sec: 238 is_paused: false red_score: 2 blue_score: 2 game_result: 255 end_reason: 255'
  echo '第四组：比赛中、剩余 238 秒、恢复、比分 2:2。'
  pause '观察第四组。'
  publish 'current_round: 1 total_rounds: 3 red_score: 3 blue_score: 2 current_stage: 5 stage_countdown_sec: 0 stage_elapsed_sec: 300 is_paused: false game_result: 1 end_reason: 2'
  echo '第五组：结算、比分 3:2、红方胜利、结束原因控制度。'
  pause '观察第五组。'
  publish 'current_stage: 99 current_round: 0 game_result: 1 end_reason: 2'
  echo '第六组：阶段 99 和局号 0 非法或未识别，结果及原因不适用；其他字段未上报。'
  echo '测试结束；未发送任何控制命令。'
  ;;
*) echo '未知操作' >&2; exit 2;;
esac
