#!/usr/bin/env bash
# Local-only static performance acceptance; publishes no robot control commands.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
broker_host=127.0.0.1
broker_port=${RM27_TEST_PORT:-18884}
[[ $# -ge 2 && $# -le 3 ]] || { echo '用法: tools/rm27_p2b3_acceptance.sh start|send red|blue [--auto]' >&2; exit 2; }
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
auto=${3:-}
if [[ $auto != --auto && ! -t 0 ]]; then echo '请在终端运行，或使用 --auto' >&2; exit 1; fi
pause() { if [[ $auto != --auto ]]; then printf '%s 按回车继续：' "$1"; read -r _; fi; }
publish() {
  local topic=$1 message=$2 body=$3
  printf '%s\n' "$body" | protoc -I "$proto_dir" "--encode=robomaster.$message" "$proto_dir/robomaster.proto" |
    mosquitto_pub -h "$broker_host" -p "$broker_port" -q 1 -t "$topic" -s
}
robot_id=1
[[ $scenario == blue ]] && robot_id=101
base="robot_id: $robot_id
connection_state: 1
field_state: 0
alive_state: 1
performance_system_shooter: 1
performance_system_chassis: 1
max_health: 100
max_heat: 0
heat_cooldown_rate: 0
max_power: 50
max_buffer_energy: 60
max_chassis_energy: 120"
echo "测试目标：本机 $broker_host:$broker_port，$scenario 方，机器人 ID $robot_id。"
publish RobotStaticStatus RobotStaticStatus "$base"
echo '已发送初始状态：最大血量 100、最大热量 0、冷却 0、功率 50；缓冲能量 60、底盘能量 120；冷却优先、血量优先。'
pause '请观察静态性能。'
publish RobotModuleStatus RobotModuleStatus 'modular_42mm: 1'
publish Event Event 'event_id: 16'
echo '已发送安装权限 1 和 Event 16；静态性能仍为最大血量 100。'
pause '请分别观察权限、事件和性能。'
publish RobotStaticStatus RobotStaticStatus "robot_id: $robot_id
connection_state: 1
field_state: 0
alive_state: 1
performance_system_shooter: 2
performance_system_chassis: 2
max_health: 450
max_heat: 250
heat_cooldown_rate: 66
max_power: 50
max_buffer_energy: 80
max_chassis_energy: 140"
echo '已发送性能状态：最大血量 450、最大热量 250、冷却 66、功率 50；缓冲能量 80、底盘能量 140；爆发优先、功率优先。'
pause '请观察静态性能变化。'
publish RobotStaticStatus RobotStaticStatus "$base"
echo '已恢复测试状态：最大血量 100、最大热量 0、冷却 0、功率 50；缓冲能量 60、底盘能量 120；冷却优先、血量优先。'
echo '注入完成；仅发送 RobotStaticStatus、RobotModuleStatus 和 Event。'
