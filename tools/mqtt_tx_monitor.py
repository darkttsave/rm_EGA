#!/usr/bin/env python3
"""Read local DevHooks counters; never connect to MQTT or send commands."""

import argparse
import json
import math
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass


@dataclass(frozen=True)
class Snapshot:
    connected: bool
    broker: str | None
    controller_published: int
    controller_dropped: int
    outpost_published: int
    outpost_dropped: int


def parse_snapshot(document):
    try:
        state = document["network_state"]["custom_control"]
        controller, outpost = state["controller"], state["outpost"]
        connected, broker = state["mqtt_connected"], state["broker"]
        counts = (controller["published"], controller["dropped"],
                  outpost["published"], outpost["dropped"])
    except (KeyError, TypeError) as exc:
        raise ValueError("/state 未包含完整的 custom_control 统计") from exc
    if type(connected) is not bool or (broker is not None and not isinstance(broker, str)):
        raise ValueError("MQTT 状态或 Broker 字段格式错误")
    if any(type(count) is not int or count < 0 for count in counts):
        raise ValueError("统计值必须是非负整数")
    return Snapshot(connected, broker, *counts)


def format_snapshot(current, previous=None, elapsed=None):
    lines = [
        f"MQTT: {'已连接' if current.connected else '未连接'}",
        f"Broker（最近连接尝试）: {current.broker or '未知'}",
        "Client ID: 不可用",
        f"控制器 30B: 本地接受={current.controller_published} 丢弃={current.controller_dropped}",
        f"前哨站 10B: 本地接受={current.outpost_published} 丢弃={current.outpost_dropped}",
    ]
    if previous is None or elapsed is None or elapsed <= 0:
        lines.append("采样差分/频率: 暂无（等待下一次有效采样）")
    elif (current.controller_published < previous.controller_published or
          current.outpost_published < previous.outpost_published or
          current.controller_dropped < previous.controller_dropped or
          current.outpost_dropped < previous.outpost_dropped):
        lines.append("采样差分/频率: 计数器已重置，等待下一次采样")
    else:
        controller_delta = current.controller_published - previous.controller_published
        outpost_delta = current.outpost_published - previous.outpost_published
        lines.append(f"过去 {elapsed:.2f} 秒本地接受: 控制器 +{controller_delta} "
                     f"({controller_delta / elapsed:.2f} Hz)，前哨站 +{outpost_delta} "
                     f"({outpost_delta / elapsed:.2f} Hz)，合计 "
                     f"{(controller_delta + outpost_delta) / elapsed:.2f} Hz")
    lines.append("本地接受不代表官方 Broker 或机器人已收到；丢弃原因无法由总数区分。")
    return "\n".join(lines)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def fetch_snapshot(port, timeout):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    with opener.open(f"http://127.0.0.1:{port}/state", timeout=timeout) as response:
        return parse_snapshot(json.load(response))


def positive_float(value):
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("必须为正数") from exc
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("必须为有限正数")
    return number


def port_number(value):
    try:
        port = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("端口必须是 1–65535 的整数") from exc
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("端口必须是 1–65535 的整数")
    return port


def main(argv=None):
    parser = argparse.ArgumentParser(description="只读本机 DevHooks 的 MQTT CustomControl 发送统计")
    parser.add_argument("--port", type=port_number, default=17711, help="本机 DevHooks 端口（默认 17711）")
    parser.add_argument("--interval", type=positive_float, default=1.0, help="刷新间隔秒数（默认 1）")
    parser.add_argument("--timeout", type=positive_float, default=1.0, help="单次 HTTP 请求超时秒数（默认 1）")
    parser.add_argument("--once", action="store_true", help="只读取一次并退出")
    args = parser.parse_args(argv)
    previous = None
    previous_time = None
    try:
        while True:
            try:
                snapshot = fetch_snapshot(args.port, args.timeout)
                now = time.monotonic()
                print(format_snapshot(snapshot, previous,
                                      None if previous_time is None else now - previous_time), flush=True)
                previous, previous_time = snapshot, now
            except (OSError, ValueError, urllib.error.URLError) as exc:
                print(f"DevHooks 无数据或请求失败: {exc}；请确认已启用并监听 127.0.0.1:{args.port}。", flush=True)
                previous = previous_time = None
                if args.once:
                    return 1
            if args.once:
                return 0
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
