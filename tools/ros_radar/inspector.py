"""ROS 雷达终端检查器：解析完整快照、显示统计并记录 JSONL。"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, TextIO

from . import parser as radar_parser
from . import receiver

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_MESSAGE = 2
EXIT_INTERRUPTED = 130


def print_status(message: str, *, stream: TextIO) -> None:
    """输出一行立即可见的生命周期状态。"""

    print(f"[ROS Radar] {message}", file=stream, flush=True)


def message_to_dict(value: object) -> Any:
    """把 ROS 消息转成只含 JSON 基本类型的原始结构，不做协议解释。"""

    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (bytes, bytearray)):
        return list(value)
    if isinstance(value, (list, tuple)):
        return [message_to_dict(item) for item in value]
    fields_getter = getattr(value, "get_fields_and_field_types", None)
    if callable(fields_getter):
        names = fields_getter().keys()
    elif hasattr(value, "__dict__"):
        names = (name for name in vars(value) if not name.startswith("_"))
    else:
        return repr(value)
    return {name: message_to_dict(getattr(value, name)) for name in names}


@dataclass
class InspectorStats:
    received: int = 0
    valid: int = 0
    invalid: int = 0
    first_monotonic: float | None = None
    last_monotonic: float | None = None

    @property
    def frequency_hz(self) -> float:
        if (
            self.received < 2
            or self.first_monotonic is None
            or self.last_monotonic is None
            or self.last_monotonic <= self.first_monotonic
        ):
            return 0.0
        return (self.received - 1) / (self.last_monotonic - self.first_monotonic)


class RadarInspector:
    def __init__(
        self,
        *,
        show_raw: bool = False,
        show_parsed: bool = True,
        output_path: Path | None = None,
        stream: TextIO = sys.stdout,
    ) -> None:
        self.show_raw = show_raw
        self.show_parsed = show_parsed
        self.stream = stream
        self.stats = InspectorStats()
        self._previous_keys: set[tuple[int, int]] = set()
        self._output = None
        if output_path is not None:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            self._output = output_path.open("a", encoding="utf-8")

    def handle(self, envelope: receiver.MessageEnvelope) -> None:
        if self.stats.received == 0:
            print_status("已收到首帧消息，开始解析", stream=self.stream)

        self.stats.received += 1
        if self.stats.first_monotonic is None:
            self.stats.first_monotonic = envelope.monotonic_at
        self.stats.last_monotonic = envelope.monotonic_at
        raw = message_to_dict(envelope.message)
        record: dict[str, Any] = {
            "received_at": envelope.received_at.isoformat(timespec="milliseconds"),
            "raw": raw,
        }

        try:
            frame = radar_parser.parse_message(envelope.message)
        except radar_parser.RadarParseError as error:
            self.stats.invalid += 1
            record.update(status="INVALID", issues=list(error.issues))
            print(
                f"[{record['received_at']}] INVALID "
                f"rate={self.stats.frequency_hz:.2f}Hz",
                file=self.stream,
                flush=True,
            )
            for issue in error.issues:
                print(f"    ! {issue}", file=self.stream, flush=True)
        else:
            self.stats.valid += 1
            current_keys = {robot.key for robot in frame.robots}
            appeared = sorted(current_keys - self._previous_keys)
            disappeared = sorted(self._previous_keys - current_keys)
            self._previous_keys = current_keys
            record.update(
                status="VALID",
                parsed=frame.to_dict(),
                appeared=[list(key) for key in appeared],
                disappeared=[list(key) for key in disappeared],
            )
            print(
                f"[{record['received_at']}] VALID robots={len(frame.robots)} "
                f"rate={self.stats.frequency_hz:.2f}Hz",
                file=self.stream,
                flush=True,
            )
            if appeared:
                print(f"    appeared={appeared}", file=self.stream, flush=True)
            if disappeared:
                print(f"    disappeared={disappeared}", file=self.stream, flush=True)
            if self.show_parsed:
                print(
                    "    parsed="
                    + json.dumps(frame.to_dict(), ensure_ascii=False, separators=(",", ":")),
                    file=self.stream,
                    flush=True,
                )

        if self.show_raw:
            print(
                "    raw=" + json.dumps(raw, ensure_ascii=False, separators=(",", ":")),
                file=self.stream,
                flush=True,
            )
        if self._output is not None:
            self._output.write(json.dumps(record, ensure_ascii=False) + "\n")
            self._output.flush()

    def summary(self) -> str:
        return (
            f"收到={self.stats.received} 有效={self.stats.valid} "
            f"异常={self.stats.invalid} 频率={self.stats.frequency_hz:.2f}Hz"
        )

    def close(self) -> None:
        if self._output is not None:
            self._output.close()
            self._output = None


def build_argument_parser() -> argparse.ArgumentParser:
    argument_parser = argparse.ArgumentParser(
        description="订阅 ROS 2 雷达位置完整快照，校验后输出并可记录 JSONL。"
    )
    argument_parser.add_argument("--topic", default=receiver.DEFAULT_TOPIC)
    argument_parser.add_argument("--node-name", default=receiver.DEFAULT_NODE_NAME)
    argument_parser.add_argument(
        "--timeout", type=float, default=None, help="运行秒数；默认持续运行到 Ctrl-C"
    )
    argument_parser.add_argument("--max-messages", type=int, default=None)
    argument_parser.add_argument("--show-raw", action="store_true")
    argument_parser.add_argument(
        "--no-parsed", action="store_true", help="不在终端逐帧打印解析结构"
    )
    argument_parser.add_argument("--output", type=Path, help="追加保存原始与解析结果 JSONL")
    return argument_parser


def run_inspector(
    args: argparse.Namespace,
    *,
    radar_receiver: receiver.RadarReceiver | None = None,
    inspector: RadarInspector | None = None,
    clock: Callable[[], float] = time.monotonic,
    stdout: TextIO | None = None,
    stderr: TextIO | None = None,
) -> int:
    """运行检查器并统一输出启动、首帧、停止和异常状态。"""

    stdout = sys.stdout if stdout is None else stdout
    stderr = sys.stderr if stderr is None else stderr
    try:
        if inspector is None:
            inspector = RadarInspector(
                show_raw=args.show_raw,
                show_parsed=not args.no_parsed,
                output_path=args.output,
                stream=stdout,
            )
        if radar_receiver is None:
            radar_receiver = receiver.RadarReceiver(
                topic=args.topic, node_name=args.node_name
            )
    except (OSError, receiver.ReceiverError) as error:
        print_status(f"启动失败：{error}", stream=stderr)
        print_status("程序已退出", stream=stdout)
        return EXIT_ERROR

    started = clock()
    subscription_started = False

    def on_started() -> None:
        nonlocal subscription_started
        subscription_started = True
        print_status(f"ROS 2 节点已启动：{args.node_name}", stream=stdout)
        print_status(f"Topic：{args.topic}", stream=stdout)
        print_status(f"消息类型：{receiver.MESSAGE_TYPE_NAME}", stream=stdout)
        print_status("QoS：Reliable、Keep Last、Depth 10", stream=stdout)
        print_status("订阅已创建（不代表已发现发布者）", stream=stdout)
        print_status("等待雷达消息...", stream=stdout)

    exit_code = EXIT_OK
    try:
        result = radar_receiver.run(
            handler=inspector.handle,
            timeout=args.timeout,
            max_messages=args.max_messages,
            on_started=on_started,
        )
        if result.stopped_by_shutdown:
            print_status("用户请求停止", stream=stdout)
            exit_code = EXIT_INTERRUPTED
        elif result.stopped_by_limit:
            print_status("已达到最大消息数，准备停止", stream=stdout)
        elif args.timeout is not None:
            print_status("已达到运行时限，准备停止", stream=stdout)

        if result.received == 0 and exit_code == EXIT_OK:
            print_status("未收到雷达消息（尚未确认发布者）", stream=stdout)
            exit_code = EXIT_NO_MESSAGE
    except KeyboardInterrupt:
        print_status("用户请求停止", stream=stdout)
        exit_code = EXIT_INTERRUPTED
    except receiver.ReceiverError as error:
        print_status(f"启动失败：{error}", stream=stderr)
        exit_code = EXIT_ERROR
    except Exception as error:  # noqa: BLE001 - CLI 将运行时异常转成简洁状态
        print_status(f"运行异常：{error}", stream=stderr)
        exit_code = EXIT_ERROR
    finally:
        inspector.close()
        if subscription_started:
            print_status("订阅已停止", stream=stdout)
        elapsed = max(0.0, clock() - started)
        print_status(f"运行时长：{elapsed:.2f} s", stream=stdout)
        print_status(f"接收消息：{inspector.stats.received}", stream=stdout)
        print_status(f"解析成功：{inspector.stats.valid}", stream=stdout)
        print_status(f"解析失败：{inspector.stats.invalid}", stream=stdout)
        print_status("程序已退出", stream=stdout)

    return exit_code


def main(argv: list[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    return run_inspector(args)


if __name__ == "__main__":
    raise SystemExit(main())
