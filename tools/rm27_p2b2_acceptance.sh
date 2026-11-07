#!/usr/bin/env bash
# Local-only RobotModuleStatus observation; no control commands are published.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
broker_host=127.0.0.1
broker_port=${RM27_TEST_PORT:-18884}
[[ $# -ge 2 && $# -le 3 ]] || { echo '用法: tools/rm27_p2b2_acceptance.sh start|send red|blue [--auto]' >&2; exit 2; }
action=$1
scenario=$2
[[ $scenario == red || $scenario == blue ]] || { echo '场景须为 red 或 blue' >&2; exit 2; }
if [[ $action == start ]]; then
  [[ $# == 2 ]] || exit 2
  exec "$root_dir/tools/rm27_b2_acceptance.sh" start "$scenario"
fi
[[ $action == send && ( $# == 2 || ${3:-} == --auto ) ]] || exit 2
[[ $broker_port =~ ^[0-9]{1,5}$ ]] && ((10#$broker_port >= 1 && 10#$broker_port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
command -v protoc >/dev/null && command -v mosquitto_pub >/dev/null || { echo '需要 protoc 和 mosquitto_pub' >&2; exit 1; }
if [[ ${3:-} != --auto && ! -t 0 ]]; then echo '请在终端运行，或使用 --auto' >&2; exit 1; fi
auto=${3:-}
pause() { if [[ $auto != --auto ]]; then printf '%s 按回车继续：' "$1"; read -r _; fi; }
publish() {
  local topic=$1 message=$2 body=$3
  printf '%s\n' "$body" | protoc -I "$proto_dir" "--encode=robomaster.$message" "$proto_dir/robomaster.proto" |
    mosquitto_pub -h "$broker_host" -p "$broker_port" -q 1 -t "$topic" -s
}
common='power_manager: 0
rfid: 1
light_strip: 2
small_shooter: 1
big_shooter: 2
uwb: 1
armor: 1
video_transmission: 1
capacitor: 1
main_controller: 1'
echo "连接本机 $broker_host:$broker_port，确认客户端为 $scenario 场景。"
publish RobotModuleStatus RobotModuleStatus "$common"
echo '模块状态已收到；电源 0，RFID 1，灯条 2；42mm 安装权限未收到。'
pause '查看开发者面板。'
publish RobotModuleStatus RobotModuleStatus "$common
modular_42mm: 0"
echo '42mm 安装权限：已确认，0（未拥有）。'
pause '查看开发者面板。'
publish Event Event 'event_id: 16'
echo 'Event 16 已发送；权限仍为 0。'
pause '观察 Event 16 和权限各自的状态。'
publish RobotModuleStatus RobotModuleStatus "$common
modular_42mm: 1"
echo '42mm 安装权限：已确认，1（拥有）；Event 16 仍为已收到。'
echo '注入完成；仅发送 RobotModuleStatus 和 Event，不发送控制命令。'
