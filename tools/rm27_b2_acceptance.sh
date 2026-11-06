#!/usr/bin/env bash
# Local-only P2-B1-B0-B2 manual acceptance data; never publishes control commands.
set -euo pipefail

root_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
proto_dir="$root_dir/src/network/proto"
proto_file="$proto_dir/robomaster.proto"
broker_host=127.0.0.1
broker_port=${RM27_TEST_PORT:-18884}

usage() {
  cat <<'HELP'
用法：
  tools/rm27_b2_acceptance.sh start red|blue
  tools/rm27_b2_acceptance.sh send red|blue [--auto]

start 启动仅监听 127.0.0.1 的独立测试 Broker 和开发者客户端；关闭客户端或 Ctrl+C 自动清理。
send 仅向同一端口发送机器人状态、血量和 Event 测试数据，默认分步等待回车。
--auto 供自动验证使用，不等待回车。可设置 RM27_TEST_PORT，更改时两个终端必须一致。
HELP
}
fail() { printf '错误：%s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "缺少 $1，请先安装该程序。"; }
[[ $# -ge 2 && $# -le 3 ]] || { usage; exit 2; }
action=$1
scenario=$2
[[ $scenario == red || $scenario == blue ]] || fail '场景须为 red 或 blue。'
[[ $broker_port =~ ^[0-9]{1,5}$ ]] && (( 10#$broker_port >= 1 && 10#$broker_port <= 65535 )) || fail 'RM27_TEST_PORT 须为 1 至 65535。'
broker_port=$((10#$broker_port))
robot_id=1
[[ $scenario == blue ]] && robot_id=101

case "$action" in
  start)
    [[ $# == 2 ]] || fail 'start 不接受额外参数。'
    need mosquitto
    client="$root_dir/build/devtools/RoboMasterClient2025"
    [[ -x $client ]] || fail '找不到已构建的开发者客户端 build/devtools/RoboMasterClient2025。'
    temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/rm27-b2.XXXXXX")
    broker_pid=''
    client_pid=''
    cleanup() {
      trap - EXIT INT TERM
      if [[ -n $client_pid ]] && kill -0 "$client_pid" 2>/dev/null; then kill "$client_pid" 2>/dev/null || true; wait "$client_pid" 2>/dev/null || true; fi
      if [[ -n $broker_pid ]] && kill -0 "$broker_pid" 2>/dev/null; then kill "$broker_pid" 2>/dev/null || true; wait "$broker_pid" 2>/dev/null || true; fi
      rm -rf -- "$temp_dir"
      printf '\n本次测试客户端和 Broker 已停止，临时文件已清理。\n'
    }
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    printf 'listener %s %s\nallow_anonymous true\n' "$broker_port" "$broker_host" > "$temp_dir/mosquitto.conf"
    mosquitto -c "$temp_dir/mosquitto.conf" > "$temp_dir/broker.log" 2>&1 & broker_pid=$!
    sleep 1
    if ! kill -0 "$broker_pid" 2>/dev/null; then
      cat "$temp_dir/broker.log" >&2
      fail "本地端口 $broker_port 不可用；可设置 RM27_TEST_PORT 后重试。"
    fi
    printf '测试 Broker：%s:%s，%s 方，机器人 ID %s。\n' "$broker_host" "$broker_port" "$scenario" "$robot_id"
    printf '请等待客户端打开，在客户端按 Ctrl+Shift+F10。另开终端运行 send 命令。\n'
    printf '客户端日志：%s/client.log（退出后自动清理）。\n' "$temp_dir"
    cd "$root_dir"
    env -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
      RM_DEVTOOLS=1 RM_MQTT_BROKER="$broker_host" RM_MQTT_PORT="$broker_port" RM_CLIENT_ROBOT_ID="$robot_id" \
      "$client" > "$temp_dir/client.log" 2>&1 & client_pid=$!
    set +e
    wait "$client_pid"
    client_status=$?
    set -e
    client_pid=''
    if (( client_status != 0 )); then
      printf '客户端退出状态：%s；最近日志：\n' "$client_status" >&2
      tail -n 30 "$temp_dir/client.log" >&2
      exit "$client_status"
    fi
    ;;
  send)
    [[ $# == 2 || ${3:-} == --auto ]] || fail 'send 可选参数只有 --auto。'
    need protoc
    need mosquitto_pub
    [[ -f $proto_file ]] || fail '找不到仓库 Protobuf 定义。'
    auto=${3:-}
    if [[ $auto != --auto && ! -t 0 ]]; then
      fail '分步模式需要终端输入；自动执行请使用 --auto。'
    fi
    pause() {
      if [[ $auto != --auto ]]; then
        printf '%s 按回车继续：' "$1"
        read -r _
      fi
    }
    publish() {
      local topic=$1 type=$2 body=$3
      printf '%s\n' "$body" | protoc -I "$proto_dir" "--encode=robomaster.$type" "$proto_file" |
        mosquitto_pub -h "$broker_host" -p "$broker_port" -q 1 -t "$topic" -s
    }
    printf '发送到本机测试 Broker %s:%s；确认客户端使用 %s 方（当前 ID %s）。\n' "$broker_host" "$broker_port" "$scenario" "$robot_id"
    for id in 1 3 4 7 101 103 104 107; do
      publish RobotStaticStatus RobotStaticStatus "robot_id: $id
connection_state: 1
field_state: 0
alive_state: 1
max_health: 500"
    done
    publish GlobalUnitStatus GlobalUnitStatus 'robot_health: 101
robot_health: 102
robot_health: 103
robot_health: 104
robot_health: 105
robot_health: 106
robot_health: 107
robot_health: 108'
    if [[ $scenario == red ]]; then
      printf '预期红方 ID 1/3/4/7：101/102/103/104；蓝方 ID 101/103/104/107：105/106/107/108。\n'
    else
      printf '预期蓝方 ID 101/103/104/107：101/102/103/104；红方 ID 1/3/4/7：105/106/107/108。\n'
    fi
    pause '请在开发者面板查看血量，按 Esc 关闭面板；如未进入战术界面，按 Ctrl+T 切换。'
    publish Event Event 'event_id: 1
param: "1,101"'
    printf '已发送 Event 1：蓝方重装机器人击毁了红方重装机器人。\n'
    pause '请观察客户端短暂出现的事件提示，再按 Ctrl+Shift+F10 重新打开开发者面板。'
    publish Event Event 'event_id: 16'
    printf '已发送 Event 16：请在面板底部查看“已收到: 是”和最近接收时间。\n'
    printf '测试数据发送完成。此脚本只发送 RobotStaticStatus、GlobalUnitStatus 和 Event。\n'
    ;;
  *) usage; fail '未知操作。' ;;
esac
