#!/usr/bin/env bash
# Isolated localhost observations only. No robot control topics.
set -euo pipefail
root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
port=${RM27_TEST_PORT:-18884}
[[ $port =~ ^[0-9]{1,5}$ ]] && ((10#$port >= 1 && 10#$port <= 65535)) || { echo '端口必须为 1～65535' >&2; exit 2; }
port=$((10#$port))
[[ $# == 2 ]] || { echo '用法: start red|blue / broker down|up / send normal|zero|missing' >&2; exit 2; }
action=$1; scenario=$2
if [[ $action == start || $action == broker ]]; then exec "$root_dir/tools/rm27_backend_b2_acceptance.sh" "$action" "$scenario"; fi
[[ $action == send ]] || exit 2
for cmd in protoc mosquitto_pub; do command -v "$cmd" >/dev/null || { echo "缺少 $cmd" >&2; exit 1; }; done
run_dir="${TMPDIR:-/tmp}/rm27-b2-${UID}-${port}"
[[ -f $run_dir/broker.pid && -f $run_dir/mosquitto.conf ]] || { echo '请先启动隔离测试客户端。' >&2; exit 1; }
read -r broker_pid < "$run_dir/broker.pid"
[[ $broker_pid =~ ^[0-9]+$ ]] && kill -0 "$broker_pid" 2>/dev/null && tr '\0' '\n' < "/proc/$broker_pid/cmdline" | grep -Fxq "$run_dir/mosquitto.conf" || { echo '隔离测试 Broker 未运行。' >&2; exit 1; }
publish() { printf '%s\n' "$2" | protoc -I "$proto_dir" --encode="robomaster.$1" "$proto_dir/robomaster.proto" | mosquitto_pub -h 127.0.0.1 -p "$port" -q 1 -t "$1" -s; }
case "$scenario" in
 normal) publish GameStatus 'current_round: 2 current_stage: 4 stage_countdown_sec: 240 red_score: 2 blue_score: 1'; publish GlobalUnitStatus 'total_CP_ally: 30 total_CP_enemy: 40'; echo '阶段 4，第 2 局，时间 4:00，红 2 / 蓝 1；己方/对方累计控制度 30/40。';;
 zero) publish GameStatus 'current_stage: 0 stage_countdown_sec: 0 red_score: 0 blue_score: 0'; publish GlobalUnitStatus 'total_CP_ally: 0 total_CP_enemy: 0'; echo '阶段、时间、比分和双方累计控制度均为显式 0。';;
 missing) publish GameStatus 'current_round: 2'; publish GlobalUnitStatus 'total_damage_ally: 10'; echo '阶段、时间、比分及累计控制度未上报。';;
 *) echo '未知测试场景' >&2; exit 2;;
esac
echo "仅发送至 127.0.0.1:$port，未发送控制命令。"
