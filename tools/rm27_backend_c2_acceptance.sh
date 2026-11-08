#!/usr/bin/env bash
# Local-only official respawn observation; no CommonCommand is published.
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
publish() { printf '%s\n' "$1" | protoc -I "$proto_dir" --encode=robomaster.RobotRespawnStatus "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t RobotRespawnStatus -s; }
id=1; [[ $mode == blue ]] && id=101
echo "仅发送本地状态数据至 127.0.0.1:$port；请确认客户端身份为 $mode / $id。"
publish 'is_pending_respawn: true total_respawn_progress: 100 current_respawn_progress: 25 can_free_respawn: false gold_cost_for_respawn: 50 can_pay_for_respawn: true'
echo '第一组：待复活 true、总读条 100、当前 25、免费 false、费用 50、金币资格 true。'
pause '观察第一组。'
publish 'is_pending_respawn: true total_respawn_progress: 120 current_respawn_progress: 90 can_free_respawn: true gold_cost_for_respawn: 20 can_pay_for_respawn: false'
echo '第二组：待复活 true、总读条 120、当前 90、免费 true、费用 20、金币资格 false。'
pause '观察第二组。'
publish 'is_pending_respawn: false current_respawn_progress: 0 can_free_respawn: false'
echo '第三组：待复活 false、当前读条 0、免费 false；总读条、费用和金币资格未上报，分别保留上次 120、20、false。'
echo '复活操作已冻结；本脚本不发送控制命令。'
