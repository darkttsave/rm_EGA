"""ROS 雷达终端检查器：解析完整快照、显示统计并记录 JSONL。"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, TextIO

from . import parser as radar_parser
from . import receiver

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_MESSAGE = 2
EXIT_INTERRUPTED = 130


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
        raw = message_to_dict(envelope.message)
        self.stats.received += 1
        if self.stats.first_monotonic is None:
            self.stats.first_monotonic = envelope.monotonic_at
        self.stats.last_monotonic = envelope.monotonic_at
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
            )
            for issue in error.issues:
                print(f"    ! {issue}", file=self.stream)
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
            )
            if appeared:
                print(f"    appeared={appeared}", file=self.stream)
            if disappeared:
                print(f"    disappeared={disappeared}", file=self.stream)
            if self.show_parsed:
                print(
                    "    parsed="
                    + json.dumps(frame.to_dict(), ensure_ascii=False, separators=(",", ":")),
                    file=self.stream,
                )

        if self.show_raw:
            print(
                "    raw=" + json.dumps(raw, ensure_ascii=False, separators=(",", ":")),
                file=self.stream,
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


def main(argv: list[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    inspector = RadarInspector(
        show_raw=args.show_raw,
        show_parsed=not args.no_parsed,
        output_path=args.output,
    )
    radar_receiver = receiver.RadarReceiver(topic=args.topic, node_name=args.node_name)
    try:
        result = radar_receiver.run(
            handler=inspector.handle,
            timeout=args.timeout,
            max_messages=args.max_messages,
        )
    except KeyboardInterrupt:
        print("已由用户中断", file=sys.stderr)
        return EXIT_INTERRUPTED
    except receiver.ReceiverError as error:
        print(f"错误：{error}", file=sys.stderr)
        return EXIT_ERROR
    finally:
        inspector.close()

    print("=== 汇总 ===")
    print(inspector.summary())
    if result.received == 0:
        print("NO_MESSAGE：运行期间未收到雷达消息", file=sys.stderr)
        return EXIT_NO_MESSAGE
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
