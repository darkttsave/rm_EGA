#!/usr/bin/env bash
# Local-only source comparison; connection lifecycle reuses B2's isolated environment.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
host=127.0.0.1
port=${RM27_TEST_PORT:-18884}
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
[[ $# -ge 2 && $# -le 3 ]] || { echo '用法: start red|blue / broker down|up / send red|blue [--auto]' >&2; exit 2; }
action=$1; mode=$2
if [[ $action == start || $action == broker ]]; then
  [[ $# == 2 ]] || exit 2
  exec "$root_dir/tools/rm27_backend_b2_acceptance.sh" "$action" "$mode"
fi
[[ $action == send && ( $mode == red || $mode == blue ) ]] || exit 2
[[ $# == 2 || ${3:-} == --auto ]] || exit 2
for tool in protoc mosquitto_pub; do command -v "$tool" >/dev/null || { echo "缺少 $tool" >&2; exit 1; }; done
auto=${3:-}; [[ $auto == --auto || -t 0 ]] || { echo '分步模式需要终端；自动模式加 --auto。' >&2; exit 1; }
pause() { if [[ $auto != --auto ]]; then printf '%s 按回车继续：' "$1"; read -r _; fi; }
publish() { local topic=$1 payload=$2; printf '%s\n' "$payload" | protoc -I "$proto_dir" --encode="robomaster.$topic" "$proto_dir/robomaster.proto" | mosquitto_pub -h "$host" -p "$port" -q 1 -t "$topic" -s; }
id=1; [[ $mode == blue ]] && id=101
echo "本地 Broker $host:$port；请确认客户端当前身份 $mode / $id。"
publish RobotStaticStatus "robot_id: $id connection_state: 1 field_state: 0 alive_state: 1 max_health: 300 max_heat: 100"
publish GameStatus 'current_round: 1 current_stage: 4 stage_countdown_sec: 240 red_score: 2 blue_score: 1 is_paused: false'
publish GlobalUnitStatus 'robot_health: 120 robot_health: 230 robot_health: 240 robot_health: 270 robot_health: 121 robot_health: 231 robot_health: 241 robot_health: 271'
publish RobotDynamicStatus 'current_health: 90 current_heat: 12.5'
echo '第一组：当前身份 Global 血量 120、Dynamic 血量 90、官方热量 12.5、官方剩余时间 240；裁判机器人连接已上报。'
pause '观察第一组。'
publish GlobalUnitStatus 'robot_health: 0 robot_health: 230 robot_health: 240 robot_health: 270'
publish RobotDynamicStatus 'current_health: 80 current_heat: 15.5'
echo '第二组：Global 血量显式 0，Dynamic 血量 80、热量 15.5。'
pause '观察第二组。'
publish GlobalUnitStatus 'total_damage_ally: 3'
publish RobotDynamicStatus 'current_health: 70 current_heat: 22.75'
echo '第三组：Global 血量未上报（上次为 0），Dynamic 血量 70、热量 22.75。'
echo '只发送测试状态，不发送控制命令。'
