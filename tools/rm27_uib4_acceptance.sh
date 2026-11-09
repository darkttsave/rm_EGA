#!/usr/bin/env bash
# Local isolated official observations only; no robot control commands.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
port=${RM27_TEST_PORT:-18884}
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
[[ $# -ge 2 && $# -le 3 ]] || { echo '用法: start red|blue / broker down|up / send red|blue [granted|denied|invalid|missing]' >&2; exit 2; }
action=$1; mode=$2
if [[ $action == start || $action == broker ]]; then [[ $# == 2 ]] || exit 2; exec "$root_dir/tools/rm27_backend_b2_acceptance.sh" "$action" "$mode"; fi
[[ $action == send && ( $mode == red || $mode == blue ) ]] || exit 2
scenario=${3:-granted}
for cmd in protoc mosquitto_pub; do command -v "$cmd" >/dev/null || { echo "缺少 $cmd" >&2; exit 1; }; done
run_dir="${TMPDIR:-/tmp}/rm27-b2-${UID}-${port}"
[[ -f $run_dir/broker.pid && -f $run_dir/mosquitto.conf ]] || { echo '请先启动隔离测试客户端。' >&2; exit 1; }
read -r broker_pid < "$run_dir/broker.pid"
[[ $broker_pid =~ ^[0-9]+$ ]] && kill -0 "$broker_pid" 2>/dev/null && tr '\0' '\n' < "/proc/$broker_pid/cmdline" | grep -Fxq "$run_dir/mosquitto.conf" || { echo '隔离测试 Broker 未运行。' >&2; exit 1; }
publish() { printf '%s\n' "$2" | protoc -I "$proto_dir" --encode="robomaster.$1" "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t "$1" -s; }
id=1; [[ $mode == blue ]] && id=101
case "$scenario" in
 granted)
  publish RobotStaticStatus "robot_id: $id connection_state: 1 level: 2 max_health: 300 max_heat: 100 heat_cooldown_rate: 12.5 max_power: 50 performance_system_shooter: 1 performance_system_chassis: 2"
  publish RobotDynamicStatus 'current_health: 120 current_heat: 25.5 last_projectile_fire_rate: 12.5 remaining_ammo: 9'
  publish RobotModuleStatus 'modular_42mm: 1 big_shooter: 1'
  publish Event 'event_id: 16'
  echo '重装：权限已获得；测速在线；观察到 Event 16；等级 2、最大血量 300、热量 25.5/100、冷却 12.5、功率 50、冷却优先/功率优先；速度 12.5，剩余允许发弹量 9。';;
 denied)
  publish RobotModuleStatus 'modular_42mm: 0 big_shooter: 0'
  echo '权限未获得，测速离线；先前 Event 16 为独立事件，不应改变性能或被解释为当前已安装。';;
 invalid)
  publish RobotModuleStatus 'modular_42mm: 3 big_shooter: 2'
  echo '权限无效值 3；测速安装异常导致离线。';;
 missing)
  publish RobotModuleStatus 'power_manager: 1'
  publish RobotStaticStatus "robot_id: $id max_heat: 0 heat_cooldown_rate: 0"
  echo '权限和测速未上报；最大热量和冷却显式 0，其余静态字段未上报。';;
 *) echo '未知场景' >&2; exit 2;;
esac
echo "仅发送状态到 127.0.0.1:$port；客户端身份应为 $id。Event 16 不关联具体机器人；不发送控制命令。"
