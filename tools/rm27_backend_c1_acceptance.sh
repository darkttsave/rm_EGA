#!/usr/bin/env bash
# Penalty and injury observations through an isolated local broker; no control messages.
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
publish() { local topic=$1 payload=$2; printf '%s\n' "$payload" | protoc -I "$proto_dir" --encode="robomaster.$topic" "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t "$topic" -s; }
id=1; killer=101; [[ $mode == blue ]] && { id=101; killer=1; }
echo "仅发送本机测试数据至 127.0.0.1:$port；请确认客户端身份为 $mode / $id。"
publish PenaltyInfo 'penalty_type: 1 penalty_effect_sec: 10 total_penalty_num: 2'
publish RobotInjuryStat "total_damage: 100 collision_damage: 10 small_projectile_damage: 20 large_projectile_damage: 30 dart_splash_damage: 4 module_offline_damage: 5 offline_damage: 6 penalty_damage: 7 server_kill_damage: 18 killer_id: $killer"
echo "第一组：黄牌(1)、10 秒、2 次；总计 100，各类 10/20/30/4/5/6/7/18，击杀者 $killer。"
pause '观察第一组。'
publish PenaltyInfo 'penalty_type: 4 penalty_effect_sec: 5 total_penalty_num: 3'
publish RobotInjuryStat "total_damage: 150 collision_damage: 11 small_projectile_damage: 40 large_projectile_damage: 30 dart_splash_damage: 4 module_offline_damage: 5 offline_damage: 6 penalty_damage: 16 server_kill_damage: 38 killer_id: $killer"
echo '第二组：超功率(4)、5 秒、3 次；总计 150，撞击 11、17mm 40、判罚 16、服务器强制战亡 38。'
pause '观察第二组。'
publish PenaltyInfo 'penalty_type: 0 total_penalty_num: 0'
publish RobotInjuryStat 'total_damage: 0 collision_damage: 0 killer_id: 0'
echo '第三组：判罚类型显式 0（未知类型）、时长未上报、数量 0；总计/撞击/击杀者为 0，其余伤害未上报。'
echo '测试数据只用于观察，不发送控制命令。'
