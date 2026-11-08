#!/usr/bin/env bash
# Only isolated localhost MQTT observations; never publishes control commands.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
port=${RM27_TEST_PORT:-18884}
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
[[ $# == 2 ]] || { echo '用法: start red|blue / send heavy|infantry|missing|zero / broker down|up' >&2; exit 2; }
action=$1; scenario=$2
if [[ $action == start || $action == broker ]]; then
  exec "$root_dir/tools/rm27_backend_b2_acceptance.sh" "$action" "$scenario"
fi
[[ $action == send ]] || exit 2
for cmd in protoc mosquitto_pub; do command -v "$cmd" >/dev/null || { echo "缺少 $cmd" >&2; exit 1; }; done
run_dir="${TMPDIR:-/tmp}/rm27-b2-${UID}-${port}"
[[ -f $run_dir/broker.pid && -f $run_dir/mosquitto.conf ]] || { echo '请先用本脚本启动隔离测试客户端。' >&2; exit 1; }
read -r broker_pid < "$run_dir/broker.pid"
[[ $broker_pid =~ ^[0-9]+$ ]] && kill -0 "$broker_pid" 2>/dev/null &&
  tr '\0' '\n' < "/proc/$broker_pid/cmdline" | grep -Fxq "$run_dir/mosquitto.conf" || { echo '隔离测试 Broker 未运行。' >&2; exit 1; }
publish() { printf '%s\n' "$2" | protoc -I "$proto_dir" --encode="robomaster.$1" "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t "$1" -s; }
case "$scenario" in
  heavy|infantry)
    robot_id=${RM27_TEST_ROBOT_ID:-1}
    if [[ $scenario == infantry ]]; then robot_id=${RM27_TEST_ROBOT_ID:-3}; fi
    [[ $robot_id =~ ^(1|3|101|103)$ ]] || { echo '仅支持测试身份 1、3、101、103' >&2; exit 2; }
    if [[ $scenario == heavy && $robot_id != 1 && $robot_id != 101 ]] || [[ $scenario == infantry && $robot_id != 3 && $robot_id != 103 ]]; then echo '场景与身份不匹配' >&2; exit 2; fi
    publish RobotStaticStatus "robot_id: $robot_id max_health: 200 max_heat: 100 connection_state: 1"
    if [[ $scenario == heavy ]]; then
      publish RobotDynamicStatus 'current_health: 120 current_heat: 110.5 last_projectile_fire_rate: 12.5 remaining_ammo: 9'
      echo "身份 $robot_id：血量 120/200；热量 110.5/100，提示数值超上限；上一发速度 12.5；通用剩余 9。"
    else
      publish RobotDynamicStatus 'current_health: 100 current_heat: 50 last_projectile_fire_rate: 18.5 remaining_ammo: 8'
      echo "身份 $robot_id：血量 100/200；热量 50/100；上一发速度 18.5；保留步兵旧弹量显示。"
    fi
    ;;
  missing) publish RobotDynamicStatus 'current_health: 100 current_heat: 50'; echo '速度及重装剩余量均未上报；不得显示成 0。';;
  zero) publish RobotDynamicStatus 'current_health: 100 current_heat: 0 last_projectile_fire_rate: 0 remaining_ammo: 0'; echo '速度 0.0，重装通用剩余量 0；这是显式上报。';;
  *) echo '未知场景' >&2; exit 2;;
esac
echo "仅发送至 127.0.0.1:$port；请确认客户端当前身份与场景一致。"
