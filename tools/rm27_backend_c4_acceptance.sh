#!/usr/bin/env bash
# Local-only GlobalUnitStatus facts; no CommonCommand or robot control messages.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
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
publish() { printf '%s\n' "$1" | protoc -I "$proto_dir" --encode=robomaster.GlobalUnitStatus "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t GlobalUnitStatus -s; }
id=1; [[ $mode == blue ]] && id=101
echo "仅发送本机测试数据至 127.0.0.1:$port；请确认客户端身份为 $mode / $id。"
publish 'robot_health: 120 robot_bullets: 20 robot_bullets: -1 robot_bullets: 0 total_damage_ally: 150 total_damage_enemy: 200 total_CP_ally: 30 total_CP_enemy: 40'
echo '第一组：血量 120，弹量原始数组 [20, -1, 0] 长度 3；己方/对方伤害 150/200，累计控制度 30/40。'
pause '观察第一组。'
publish 'robot_health: 0 robot_bullets: 9 robot_bullets: 8 total_damage_ally: 0 total_damage_enemy: 300 total_CP_ally: 0 total_CP_enemy: 55'
echo '第二组：血量 0，弹量 [9, 8] 长度 2；己方/对方伤害 0/300，累计控制度 0/55。'
pause '观察第二组。'
publish 'robot_health: 70 total_damage_enemy: 400 total_CP_enemy: 60'
echo '第三组：血量 70，弹量空数组（与未传递不可区分）；己方伤害/控制度未上报（上次均为 0），对方伤害/控制度 400/60。'
echo '所有控制度均为协议原始值，不代表剩余控制度；不发送控制命令。'
