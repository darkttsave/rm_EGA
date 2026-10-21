"""MQTT 通用观察入口。

第一版只解析 :data:`decoders.DECODERS` 中白名单内的 Topic；其余 Topic **不猜协议**，
只输出并保存原始信息（timestamp / topic / payload_length / 受限长度的 hex 预览）。

状态含义：

- ``PASS``：受支持 Topic，Protobuf 解析成功且语义检查通过；
- ``NOT_MATCH``：受支持 Topic，Protobuf 解析成功但字段/数量/取值/sender_id 不符合预期；
- ``PARSE_FAIL``：受支持 Topic，但无法解析为对应 Protobuf；
- ``UNSUPPORTED``：收到消息，但当前没有注册该 Topic 的解析器（已保存原始信息）；
- ``NO_MESSAGE``：超时前没有收到任何消息。

只订阅，不发布；不实现回放。
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Sequence

from . import decoders, receiver

STATUS_PASS = decoders.STATUS_PASS
STATUS_NOT_MATCH = decoders.STATUS_NOT_MATCH
STATUS_PARSE_FAIL = decoders.STATUS_PARSE_FAIL
STATUS_UNSUPPORTED = "UNSUPPORTED"
STATUS_NO_MESSAGE = "NO_MESSAGE"

# 终端展示用的「解析成功」标记：只用于把「能不能解析」与「校验是否通过」分开显示。
# 它不是 record.status 的取值，JSONL 里的 status 仍是 PASS / NOT_MATCH / PARSE_FAIL / ...
LABEL_PARSED = "PARSED"
# 只有这两种状态代表「Protobuf 已成功解析」。
PARSED_STATUSES = frozenset({STATUS_PASS, STATUS_NOT_MATCH})

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_MESSAGE = 2
EXIT_INTERRUPTED = 130

DEFAULT_TIMEOUT = 30.0
DEFAULT_TOPIC = "#"
PREVIEW_HEX_BYTES = 32

Point = tuple[int, int]


@dataclass
class InspectRecord:
    """一条消息的观察结果（纯数据，可直接序列化）。"""

    received_at: datetime
    topic: str
    payload_length: int
    status: str
    payload: bytes
    fields: dict[str, Any] = field(default_factory=dict)
    points: list[Point] = field(default_factory=list)
    issues: list[str] = field(default_factory=list)
    error: str | None = None

    def to_json(self, *, dump_points: bool = False) -> dict[str, Any]:
        record: dict[str, Any] = {
            "timestamp": self.received_at.isoformat(timespec="milliseconds"),
            "topic": self.topic,
            "payload_length": self.payload_length,
            "payload_b64": base64.b64encode(self.payload).decode("ascii"),
            "status": self.status,
            "fields": self.fields,
            "issues": self.issues,
        }
        if self.error is not None:
            record["error"] = self.error
        if dump_points:
            record["points"] = [[x, y] for x, y in self.points]
        return record

    def to_lines(self, *, dump_points: bool = False) -> list[str]:
        stamp = self.received_at.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

        # 首行只回答「能不能解析」：受支持且 Protobuf 解析成功显示 PARSED，
        # 解析失败显示 PARSE_FAIL，未注册 Topic 显示 UNSUPPORTED。
        parsed = self.status in PARSED_STATUSES
        head_label = LABEL_PARSED if parsed else self.status
        lines = [
            f"[{stamp}] topic={self.topic} payload={self.payload_length}B -> {head_label}"
        ]

        if self.status == STATUS_UNSUPPORTED:
            lines.append(f"    payload_hex={preview_hex(self.payload)}")
            lines.append("    未注册该 Topic 的解析器，仅保存原始信息")
            return lines

        # 解析成功时，把「协议校验结论」单独一行，与解析结果区分开。
        if parsed:
            lines.append(f"    validation={self.status}")

        for name, value in self.fields.items():
            lines.append(f"    {name}={value}")
        for issue in self.issues:
            lines.append(f"    ! {issue}")
        if self.error is not None:
            lines.append(f"    ! {self.error}")
        if dump_points:
            lines.append(f"    points({len(self.points)})={self.points}")
        return lines


def preview_hex(payload: bytes, *, limit: int = PREVIEW_HEX_BYTES) -> str:
    """有长度限制的十六进制预览，避免把大包刷屏。"""

    head = payload[:limit].hex(" ")
    if len(payload) > limit:
        return f"{head} … (+{len(payload) - limit}B)"
    return head


def inspect_message(
    envelope: receiver.MessageEnvelope, *, expected_sender_id: int | None = None
) -> InspectRecord:
    """把一条消息判定为 PASS / NOT_MATCH / PARSE_FAIL / UNSUPPORTED。"""

    result = decoders.decode(
        envelope.topic, envelope.payload, expected_sender_id=expected_sender_id
    )

    if result is None:
        return InspectRecord(
            received_at=envelope.received_at,
            topic=envelope.topic,
            payload_length=len(envelope.payload),
            status=STATUS_UNSUPPORTED,
            payload=bytes(envelope.payload),
        )

    return InspectRecord(
        received_at=envelope.received_at,
        topic=envelope.topic,
        payload_length=len(envelope.payload),
        status=result.status,
        payload=bytes(envelope.payload),
        fields=result.fields,
        points=result.points,
        issues=result.issues,
        error=result.error,
    )


class Inspector:
    """把观察结果写到终端，并按需追加 JSON Lines 现场记录。"""

    def __init__(
        self,
        *,
        expected_sender_id: int | None = None,
        dump_points: bool = False,
        output_path: Path | None = None,
        stream=None,
    ) -> None:
        self.expected_sender_id = expected_sender_id
        self.dump_points = dump_points
        self._stream = stream if stream is not None else sys.stdout
        self.counts: dict[str, int] = {}
        self.received = 0
        self._dump_file = None
        if output_path is not None:
            output_path.parent.mkdir(parents=True, exist_ok=True)
            self._dump_file = output_path.open("a", encoding="utf-8")

    def handle(self, envelope: receiver.MessageEnvelope) -> InspectRecord:
        record = inspect_message(
            envelope, expected_sender_id=self.expected_sender_id
        )
        self.received += 1
        self.counts[record.status] = self.counts.get(record.status, 0) + 1

        for line in record.to_lines(dump_points=self.dump_points):
            print(line, file=self._stream)

        if self._dump_file is not None:
            self._dump_file.write(
                json.dumps(record.to_json(dump_points=self.dump_points), ensure_ascii=False)
            )
            self._dump_file.write("\n")
            self._dump_file.flush()

        return record

    def summary_lines(self) -> list[str]:
        parts = [f"{status}={count}" for status, count in sorted(self.counts.items())]
        detail = "，".join(parts) if parts else "无"
        return ["=== 汇总 ===", f"收到消息：{self.received}（{detail}）"]

    def close(self) -> None:
        if self._dump_file is not None:
            self._dump_file.close()
            self._dump_file = None


def run_inspector(
    args: argparse.Namespace,
    *,
    client_factory: Callable[[str], object] = receiver.default_client_factory,
    clock: Callable[[], float] = time.monotonic,
    loop_timeout: float = receiver.DEFAULT_LOOP_TIMEOUT,
    inspector: Inspector | None = None,
) -> int:
    """连接、订阅并观察；只订阅，绝不 publish。"""

    if inspector is None:
        inspector = Inspector(
            expected_sender_id=args.expected_sender_id,
            dump_points=args.dump_points,
            output_path=args.output,
        )

    topics = list(args.topics or [DEFAULT_TOPIC])
    deadline = clock() + args.timeout

    def on_connected(subscribed: Sequence[str]) -> None:
        print(
            f"已连接 {args.broker}:{args.port}，已订阅 {', '.join(subscribed)}，"
            f"client-id={args.client_id}（只订阅，不发布）",
            file=sys.stdout,
        )

    def should_stop() -> bool:
        return args.max_messages is not None and inspector.received >= args.max_messages

    receiver_ = receiver.MessageReceiver(
        broker=args.broker,
        port=args.port,
        client_id=args.client_id,
        topics=topics,
        client_factory=client_factory,
        clock=clock,
        loop_timeout=loop_timeout,
    )
    try:
        result = receiver_.run(
            timeout=args.timeout,
            handler=inspector.handle,
            should_stop=should_stop,
            on_connected=on_connected,
            deadline=deadline,
        )
    finally:
        inspector.close()

    if result.connection_error is not None:
        print(
            f"错误：连接 {args.broker}:{args.port} 失败：{result.connection_error}",
            file=sys.stderr,
        )
        return EXIT_ERROR

    if not result.connected:
        print(
            f"错误：在 {args.timeout:g}s 内未完成 MQTT 握手（rc={result.connect_rc}）；"
            "请确认 broker 与 client-id 可用。",
            file=sys.stderr,
        )
        return EXIT_ERROR

    for line in inspector.summary_lines():
        print(line, file=sys.stdout)

    if inspector.received == 0:
        print(
            f"{STATUS_NO_MESSAGE}：{args.timeout:g}s 内没有收到任何消息"
            f"（topics={', '.join(topics)}）",
            file=sys.stderr,
        )
        return EXIT_NO_MESSAGE

    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "MQTT 通用观察入口：只解析白名单 Topic，其余只保存原始信息；只订阅不发布。"
        )
    )
    parser.add_argument("--broker", help="MQTT broker 主机名或 IP，必须显式指定")
    parser.add_argument("--port", type=int, help="MQTT 端口，必须显式指定")
    parser.add_argument("--client-id", help="本次观察使用的 MQTT Client ID，必须显式指定")
    parser.add_argument(
        "--topic",
        dest="topics",
        action="append",
        help=f"要订阅的 Topic，可重复；默认 {DEFAULT_TOPIC}（全部）",
    )
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"总超时秒数，默认 {DEFAULT_TIMEOUT:g}",
    )
    parser.add_argument(
        "--max-messages", type=int, default=None,
        help="收到多少条消息后提前退出",
    )
    parser.add_argument(
        "--expected-sender-id", type=int, default=None,
        help="期望的 RobotPathPlanInfo sender_id；不匹配判为 NOT_MATCH",
    )
    parser.add_argument(
        "--dump-points", action="store_true",
        help="输出还原后的绝对路径点",
    )
    parser.add_argument(
        "--output", type=Path, default=None,
        help="把现场消息以 JSON Lines 追加保存到该文件",
    )
    parser.add_argument(
        "--allow-live-connect",
        action="store_true",
        help="确认允许连接现场 broker；相同 Client ID 可能挤掉正在运行的客户端",
    )
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not args.broker:
        parser.error("未指定 broker，请使用 --broker")
    if "://" in args.broker:
        parser.error("broker 只填写主机名或 IP，不要包含协议前缀")
    if args.port is None:
        parser.error("未指定端口，请使用 --port")
    if not 1 <= args.port <= 65535:
        parser.error("端口必须在 1 到 65535 之间")
    if not args.client_id:
        parser.error("未指定 Client ID，请使用 --client-id")
    if "\0" in args.client_id:
        parser.error("Client ID 不能包含空字符")
    if args.timeout <= 0:
        parser.error("--timeout 必须为正数")
    if args.max_messages is not None and args.max_messages < 1:
        parser.error("--max-messages 必须为正整数")
    if args.topics and any(not topic for topic in args.topics):
        parser.error("Topic 不能为空")
    if not args.allow_live_connect:
        parser.error(
            "连接 broker 可能挤掉相同 Client ID 的在线客户端，"
            "确认现场安全后再加 --allow-live-connect"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)

    deadline = time.monotonic() + args.timeout
    if not receiver.wait_for_port(args.broker, args.port, deadline=deadline):
        print(
            f"错误：{args.broker}:{args.port} 在 {args.timeout:g}s 内不可达",
            file=sys.stderr,
        )
        return EXIT_ERROR

    try:
        return run_inspector(args)
    except receiver.ReceiverError as error:
        print(f"错误：{error}", file=sys.stderr)
        return EXIT_ERROR
    except KeyboardInterrupt:
        print("已被用户中断。", file=sys.stderr)
        return EXIT_INTERRUPTED


if __name__ == "__main__":
    raise SystemExit(main())
