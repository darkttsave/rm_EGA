#!/usr/bin/env bash
# Local-only logistics observation; never publishes robot control commands.
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
publish() { printf '%s\n' "$1" | protoc -I "$proto_dir" --encode=robomaster.GlobalLogisticsStatus "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t GlobalLogisticsStatus -s; }
id=1; [[ $mode == blue ]] && id=101
echo "仅发送本机状态数据至 127.0.0.1:$port；请确认客户端身份为 $mode / $id。"
publish 'remaining_economy: 120 total_economy_obtained: 500 tech_level: 2 encryption_level: 1'
echo '第一组：当前经济 120、累计总经济 500、科技等级 2、加密等级 1。'
pause '观察第一组。'
publish 'remaining_economy: 80 total_economy_obtained: 9007199254740993 tech_level: 3 encryption_level: 0'
echo '第二组：当前经济 80、累计总经济 9007199254740993、科技等级 3、加密等级 0。'
pause '观察第二组。'
publish 'remaining_economy: 0 tech_level: 0'
echo '第三组：当前经济 0、科技等级 0；累计总经济与加密等级未上报，上次分别为 9007199254740993、0。'
echo '测试数据只用于观测，不发送控制命令。'
