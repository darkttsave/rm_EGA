#!/usr/bin/env bash
# Local-only dynamic status verification, with an isolated writable config.
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
  temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/rm27-a1.XXXXXX")
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
    printf '%s\n' "$1" | protoc -I "$proto_dir" --encode=robomaster.RobotDynamicStatus "$proto_dir/robomaster.proto" |
      mosquitto_pub -h "$host" -p "$port" -q 1 -t RobotDynamicStatus -s
  }
  echo "测试 Broker $host:$port；请确认当前客户端为 $scenario 方 ID $robot_id。"
  publish 'current_health: 120
current_heat: 12.5
last_projectile_fire_rate: 16.25
reserved: 99
current_buffer_energy: 42
current_experience: 100
experience_for_upgrade: 50
total_projectiles_fired: 77
remaining_ammo: 9
is_out_of_combat: false
out_of_combat_countdown: 4
can_remote_heal: false
can_remote_ammo: true'
  echo '第一组：血量 120、热量 12.5、上一发射速 16.25、缓冲 42、已发 77、剩余 9、脱战 false、补血 false、补弹 true。'
  pause '请观察动态状态。'
  publish 'current_heat: 22.75
last_projectile_fire_rate: 0
remaining_ammo: 0
can_remote_heal: false
can_remote_ammo: false'
  echo '第二组：热量 22.75、上一发射速 0、剩余 0、补血 false、补弹 false；血量等未上报，显示上次有效值。'
  echo '保留字段仅显示不适用；未发送任何控制命令。'
  ;;
*) echo '未知操作' >&2; exit 2;;
esac
