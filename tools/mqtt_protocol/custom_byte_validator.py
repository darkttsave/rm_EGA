#!/usr/bin/env python3
"""校验 NUC/C 板 0x0310 自定义字节块在 MQTT 上的实际取值。

本工具只订阅并解析 MQTT 消息，从不向 broker 发布任何内容。

data 的预期长度与字段布局全部来自 --spec 指定的 JSON 描述，Python 代码不写死
长度与字段名。MQTT payload 交给仓库既有的 sim/robomaster_pb2.py 解析，不手工
解析 Protobuf wire format，也不复制第二份 CustomByteBlock 结构。

MQTT 传输层（连接/订阅/超时/停止/计数）复用同包内的 ``receiver``，本模块只保留
协议校验逻辑；pb2 加载复用 ``decoders.load_message_class``。

运行方式（包内模块，必须用 ``-m``）：

    python -m tools.mqtt_protocol.custom_byte_validator --help

依赖（只在真正需要时才导入）：
- paho-mqtt：连接 broker 时必需；
- protobuf 运行时：解析 CustomByteBlock 时必需，版本须满足
  sim/robomaster_pb2.py 自带校验的要求（由仓库门禁推导，当前为
  protobuf>=6.33.2,<7）。
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Sequence

from . import decoders
from .receiver import (
    DEFAULT_LOOP_TIMEOUT,
    MessageReceiver,
    default_client_factory,
    wait_for_port,
)

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SPEC_PATH = (
    PROJECT_ROOT / "tools/mqtt_protocol/protocol_specs/sentry_bt_status_v1.json"
)

STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_NOT_MATCH = "NOT_MATCH"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_NO_MATCH = 2
EXIT_INTERRUPTED = 130

DEFAULT_MAX_RATE_HZ = 50.0
DEFAULT_RATE_WINDOW = 1.0
DEFAULT_TIMEOUT = 30.0

# 字段类型 -> (struct 格式字符, 字节数)。字节序在本模块之外单独拼接。
FIELD_TYPES: dict[str, tuple[str, int]] = {
    "uint8": ("B", 1),
    "int8": ("b", 1),
    "uint16": ("H", 2),
    "int16": ("h", 2),
    "uint32": ("I", 4),
    "int32": ("i", 4),
}

BYTE_ORDER_PREFIX = {"little": "<", "big": ">"}

REQUIRED_TOP_LEVEL_KEYS = (
    "protocol_name",
    "protocol_version",
    "mqtt_topic",
    "protobuf_message_type",
    "protobuf_data_field",
    "source_command_id",
    "payload_length",
    "byte_order",
    "fields",
)

REQUIRED_FIELD_KEYS = ("name", "offset", "type")


class ValidatorError(RuntimeError):
    """表示工具自身的环境或配置问题。"""


class SpecError(ValidatorError):
    """表示 JSON 协议描述不合法。"""


@dataclass(frozen=True)
class FieldSpec:
    """协议描述中的单个字段。"""

    name: str
    offset: int
    type_name: str
    byte_order: str

    @property
    def size(self) -> int:
        return FIELD_TYPES[self.type_name][1]

    @property
    def end(self) -> int:
        return self.offset + self.size

    def parse(self, data: bytes) -> int:
        format_char = FIELD_TYPES[self.type_name][0]
        prefix = BYTE_ORDER_PREFIX[self.byte_order]
        return struct.unpack_from(f"{prefix}{format_char}", data, self.offset)[0]


@dataclass(frozen=True)
class ProtocolSpec:
    """一份经过校验的 JSON 协议描述。"""

    protocol_name: str
    protocol_version: str
    mqtt_topic: str
    protobuf_message_type: str
    protobuf_data_field: str
    source_command_id: str
    payload_length: int
    byte_order: str
    max_rate_hz: float
    fields: tuple[FieldSpec, ...]
    source_path: Path | None = None

    def parse_data(self, data: bytes) -> dict[str, int]:
        """按字段布局解析 data，全部字段必须落在 data 范围内。"""

        if len(data) != self.payload_length:
            raise SpecError(
                f"data 长度 {len(data)} 与协议 payload_length {self.payload_length} 不符"
            )
        return {item.name: item.parse(data) for item in self.fields}

    @classmethod
    def from_mapping(
        cls, mapping: dict, *, source_path: Path | None = None
    ) -> "ProtocolSpec":
        return parse_spec_mapping(mapping, source_path=source_path)

    @classmethod
    def load(cls, path: Path) -> "ProtocolSpec":
        try:
            raw_text = path.read_text(encoding="utf-8")
        except OSError as error:
            raise SpecError(f"无法读取协议描述 {path}：{error}") from error
        try:
            mapping = json.loads(raw_text)
        except json.JSONDecodeError as error:
            raise SpecError(f"协议描述 {path} 不是合法 JSON：{error}") from error
        if not isinstance(mapping, dict):
            raise SpecError(f"协议描述 {path} 的顶层必须是 JSON 对象")
        return cls.from_mapping(mapping, source_path=path)


def _require_nonempty_str(mapping: dict, key: str, issues: list[str]) -> str:
    value = mapping.get(key)
    if value is None:
        issues.append(f"缺少必填键 {key!r}")
        return ""
    if not isinstance(value, str) or not value.strip():
        issues.append(f"{key!r} 必须是非空字符串，当前为 {value!r}")
        return ""
    return value


def _validate_fields(
    raw_fields: object, *, payload_length: int, default_byte_order: str, issues: list[str]
) -> tuple[FieldSpec, ...]:
    if not isinstance(raw_fields, list) or not raw_fields:
        issues.append("'fields' 必须是非空数组")
        return ()

    parsed: list[FieldSpec] = []
    seen_names: set[str] = set()

    for index, raw_field in enumerate(raw_fields):
        label = f"fields[{index}]"
        if not isinstance(raw_field, dict):
            issues.append(f"{label} 必须是 JSON 对象")
            continue

        missing = [key for key in REQUIRED_FIELD_KEYS if key not in raw_field]
        if missing:
            issues.append(f"{label} 缺少必填键 {', '.join(missing)}")
            continue

        name = raw_field["name"]
        if not isinstance(name, str) or not name.strip():
            issues.append(f"{label}.name 必须是非空字符串")
            continue
        if name in seen_names:
            issues.append(f"字段名 {name!r} 重复")
            continue
        seen_names.add(name)

        offset = raw_field["offset"]
        if isinstance(offset, bool) or not isinstance(offset, int):
            issues.append(f"{label}.offset 必须是整数，当前为 {offset!r}")
            continue
        if offset < 0:
            issues.append(f"{label}.offset 不能为负，当前为 {offset}")
            continue

        type_name = raw_field["type"]
        if not isinstance(type_name, str) or type_name not in FIELD_TYPES:
            issues.append(
                f"{label}.type 必须是 {sorted(FIELD_TYPES)} 之一，当前为 {type_name!r}"
            )
            continue

        byte_order = raw_field.get("byte_order", default_byte_order)
        if not isinstance(byte_order, str) or byte_order not in BYTE_ORDER_PREFIX:
            issues.append(
                f"{label}.byte_order 必须是 "
                f"{sorted(BYTE_ORDER_PREFIX)} 之一，当前为 {byte_order!r}"
            )
            continue

        item = FieldSpec(name=name, offset=offset, type_name=type_name, byte_order=byte_order)
        if item.end > payload_length:
            issues.append(
                f"字段 {name!r} 越界：offset {offset} + {item.size} 字节 = {item.end}，"
                f"超出 payload_length {payload_length}"
            )
            continue
        parsed.append(item)

    parsed.sort(key=lambda item: item.offset)
    for previous, current in zip(parsed, parsed[1:]):
        if current.offset < previous.end:
            issues.append(
                f"字段 {current.name!r}(offset {current.offset}) 与 "
                f"{previous.name!r}(offset {previous.offset}, 结束于 {previous.end}) 重叠"
            )
    return tuple(parsed)


def parse_spec_mapping(
    mapping: dict, *, source_path: Path | None = None
) -> ProtocolSpec:
    """校验 JSON 映射并构造 ProtocolSpec；所有问题一次性汇总报出。"""

    issues: list[str] = []
    values = {
        key: _require_nonempty_str(mapping, key, issues)
        for key in REQUIRED_TOP_LEVEL_KEYS
        if key not in ("payload_length", "byte_order", "fields")
    }

    payload_length = mapping.get("payload_length")
    if isinstance(payload_length, bool) or not isinstance(payload_length, int):
        issues.append(f"'payload_length' 必须是整数，当前为 {payload_length!r}")
        payload_length = 0
    elif payload_length < 1:
        issues.append(f"'payload_length' 必须为正整数，当前为 {payload_length}")

    byte_order = mapping.get("byte_order")
    if not isinstance(byte_order, str) or byte_order not in BYTE_ORDER_PREFIX:
        issues.append(
            f"'byte_order' 必须是 {sorted(BYTE_ORDER_PREFIX)} 之一，当前为 {byte_order!r}"
        )
        byte_order = "little"

    message_type = values.get("protobuf_message_type", "")
    if message_type and "." not in message_type:
        issues.append(
            f"'protobuf_message_type' 应形如 'package.Message'，当前为 {message_type!r}"
        )

    fields = _validate_fields(
        mapping.get("fields"),
        payload_length=payload_length,
        default_byte_order=byte_order,
        issues=issues,
    )

    max_rate_hz = mapping.get("max_rate_hz", DEFAULT_MAX_RATE_HZ)
    if isinstance(max_rate_hz, bool) or not isinstance(max_rate_hz, (int, float)):
        issues.append(f"'max_rate_hz' 必须是数字，当前为 {max_rate_hz!r}")
        max_rate_hz = DEFAULT_MAX_RATE_HZ
    elif max_rate_hz <= 0:
        issues.append(f"'max_rate_hz' 必须为正数，当前为 {max_rate_hz}")
        max_rate_hz = DEFAULT_MAX_RATE_HZ

    if issues:
        location = f"{source_path}：" if source_path else ""
        detail = "\n".join(f"  - {issue}" for issue in issues)
        raise SpecError(f"协议描述不合法（{location}共 {len(issues)} 项）\n{detail}")

    return ProtocolSpec(
        protocol_name=values["protocol_name"],
        protocol_version=values["protocol_version"],
        mqtt_topic=values["mqtt_topic"],
        protobuf_message_type=message_type,
        protobuf_data_field=values["protobuf_data_field"],
        source_command_id=values["source_command_id"],
        payload_length=payload_length,
        byte_order=byte_order,
        max_rate_hz=float(max_rate_hz),
        fields=fields,
        source_path=source_path,
    )


def load_custom_byte_block(spec: ProtocolSpec):
    """返回 spec 指向的消息类；pb2 加载统一走 decoders 层，不重复实现。"""

    package, _, short_name = spec.protobuf_message_type.rpartition(".")
    if not short_name:
        raise ValidatorError(
            f"协议描述中的 protobuf_message_type 不合法：{spec.protobuf_message_type!r}"
        )

    try:
        message_class = decoders.load_message_class(short_name)
    except decoders.DecoderError as error:
        raise ValidatorError(str(error)) from error

    actual_name = message_class.DESCRIPTOR.full_name
    if actual_name != spec.protobuf_message_type:
        raise ValidatorError(
            f"协议描述要求消息类型 {spec.protobuf_message_type}，"
            f"生成模块实际提供 {actual_name}"
        )
    if spec.protobuf_data_field not in message_class.DESCRIPTOR.fields_by_name:
        raise ValidatorError(
            f"消息类型 {actual_name} 中不存在字段 {spec.protobuf_data_field!r}"
        )
    return message_class


def build_data_decoder(spec: ProtocolSpec) -> Callable[[bytes], bytes]:
    """返回把 MQTT payload 解码为业务字节的函数（使用仓库 pb2，不手工解析）。"""

    message_class = load_custom_byte_block(spec)
    field_name = spec.protobuf_data_field

    def decode(payload: bytes) -> bytes:
        message = message_class()
        message.ParseFromString(payload)
        return bytes(getattr(message, field_name))

    return decode


@dataclass
class PacketOutcome:
    """单条 MQTT 消息的判定结果。"""

    status: str
    received_at: datetime
    topic: str
    payload_length: int
    data_length: int | None = None
    data_hex: str | None = None
    values: dict[str, int] | None = None
    reason: str | None = None


def evaluate_payload(
    *,
    topic: str,
    payload: bytes,
    spec: ProtocolSpec,
    decode_data: Callable[[bytes], bytes],
    received_at: datetime | None = None,
) -> PacketOutcome:
    """把一条 MQTT 消息判定为 PASS / FAIL / NOT_MATCH。"""

    moment = received_at or datetime.now()
    outcome = PacketOutcome(
        status=STATUS_FAIL,
        received_at=moment,
        topic=topic,
        payload_length=len(payload),
    )

    try:
        data = decode_data(payload)
    except Exception as error:  # 非法 Protobuf 不得让程序崩溃
        outcome.reason = f"Protobuf 解析失败：{error}"
        return outcome

    outcome.data_length = len(data)
    outcome.data_hex = data.hex(" ") if data else "(空)"

    if len(data) != spec.payload_length:
        outcome.status = STATUS_NOT_MATCH
        outcome.reason = (
            f"data 长度 {len(data)} 与协议 payload_length {spec.payload_length} 不符"
        )
        return outcome

    try:
        outcome.values = spec.parse_data(data)
    except Exception as error:
        outcome.reason = f"字段解析失败：{error}"
        return outcome

    outcome.status = STATUS_PASS
    return outcome


def format_outcome(outcome: PacketOutcome, spec: ProtocolSpec) -> str:
    """按协议描述格式化单条结果，字段名全部来自 spec。"""

    stamp = outcome.received_at.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    parts = [f"[{stamp}]", f"topic={outcome.topic}", f"mqtt_payload={outcome.payload_length}B"]
    if outcome.data_length is not None:
        parts.append(f"data={outcome.data_length}B")
        parts.append(f"data_hex={outcome.data_hex}")

    head = " ".join(parts)
    if outcome.status == STATUS_PASS and outcome.values is not None:
        rendered = "  ".join(
            f"{item.name}={outcome.values[item.name]}" for item in spec.fields
        )
        return f"{head}\n    {rendered}  -> {STATUS_PASS}"
    if outcome.status == STATUS_NOT_MATCH:
        return f"{head}  -> {STATUS_NOT_MATCH} 原因：{outcome.reason}"
    return f"{head}  -> {STATUS_FAIL} 原因：{outcome.reason}"


@dataclass
class LogThrottle:
    """限制重复日志的逐条输出量，避免刷屏。"""

    headline: int = 3
    every: int = 100

    def allows(self, count: int) -> bool:
        if count <= self.headline:
            return True
        return self.every > 0 and count % self.every == 0


class ValidationSession:
    """累积统计、限流与退出判定；不依赖 MQTT 库，便于离线测试。"""

    def __init__(
        self,
        spec: ProtocolSpec,
        *,
        max_matches: int | None = None,
        rate_window: float = DEFAULT_RATE_WINDOW,
        max_rate_hz: float | None = None,
        output: Callable[[str], None] = print,
    ) -> None:
        self.spec = spec
        self.max_matches = max_matches
        self.rate_window = rate_window
        self.max_rate_hz = float(max_rate_hz if max_rate_hz is not None else spec.max_rate_hz)
        self.output = output

        self.received = 0
        self.passed = 0
        self.not_matched = 0
        self.failed = 0
        self.suppressed_not_match = 0
        self.suppressed_fail = 0
        self.rate_exceeded = False

        self._not_match_throttle = LogThrottle(headline=3, every=100)
        self._fail_throttle = LogThrottle(headline=5, every=100)
        self._matched_window: deque[float] = deque()
        self._first_matched_at: float | None = None
        self._last_matched_at: float | None = None
        self._last_rate_warning_at: float | None = None
        self._started_at = time.monotonic()

    @property
    def matched(self) -> int:
        return self.passed

    def is_complete(self) -> bool:
        return self.max_matches is not None and self.passed >= self.max_matches

    def exit_code(self) -> int:
        return EXIT_OK if self.passed > 0 else EXIT_NO_MATCH

    def _observe_rate(self, now: float) -> float | None:
        self._matched_window.append(now)
        while self._matched_window and now - self._matched_window[0] > self.rate_window:
            self._matched_window.popleft()
        if self._first_matched_at is None:
            self._first_matched_at = now
        self._last_matched_at = now
        if len(self._matched_window) < 2:
            return None
        span = self._matched_window[-1] - self._matched_window[0]
        if span <= 0:
            return None
        return (len(self._matched_window) - 1) / span

    def _rate_warning(self, hz: float, now: float) -> None:
        if (
            self._last_rate_warning_at is not None
            and now - self._last_rate_warning_at < 1.0
        ):
            return
        self._last_rate_warning_at = now
        self.output(
            f"    警告：观测频率 {hz:.1f}Hz 超过协议上限 {self.max_rate_hz:.1f}Hz"
        )

    def handle_message(
        self,
        *,
        topic: str,
        payload: bytes,
        decode_data: Callable[[bytes], bytes],
        now: float | None = None,
    ) -> PacketOutcome:
        outcome = evaluate_payload(
            topic=topic, payload=payload, spec=self.spec, decode_data=decode_data
        )
        self.received += 1
        moment = time.monotonic() if now is None else now

        if outcome.status == STATUS_PASS:
            self.passed += 1
            rate = self._observe_rate(moment)
            self.output(format_outcome(outcome, self.spec))
            if rate is not None:
                self.output(f"    窗口频率={rate:.1f}Hz（窗口 {self.rate_window:.1f}s）")
                if rate > self.max_rate_hz:
                    self.rate_exceeded = True
                    self._rate_warning(rate, moment)
        elif outcome.status == STATUS_NOT_MATCH:
            self.not_matched += 1
            if self._not_match_throttle.allows(self.not_matched):
                suffix = ""
                if self.not_matched > self._not_match_throttle.headline:
                    suffix = f"（累计 {self.not_matched} 条）"
                self.output(format_outcome(outcome, self.spec) + suffix)
            else:
                self.suppressed_not_match += 1
        else:
            self.failed += 1
            if self._fail_throttle.allows(self.failed):
                suffix = ""
                if self.failed > self._fail_throttle.headline:
                    suffix = f"（累计 {self.failed} 条）"
                self.output(format_outcome(outcome, self.spec) + suffix)
            else:
                self.suppressed_fail += 1
        return outcome

    def overall_rate(self) -> float | None:
        if self._first_matched_at is None or self._last_matched_at is None:
            return None
        span = self._last_matched_at - self._first_matched_at
        if span <= 0 or self.passed < 2:
            return None
        return (self.passed - 1) / span

    def summary_lines(self) -> list[str]:
        elapsed = time.monotonic() - self._started_at
        lines = [
            "=== 汇总 ===",
            f"协议：{self.spec.protocol_name} {self.spec.protocol_version}"
            f"（topic={self.spec.mqtt_topic}，命令字 {self.spec.source_command_id}）",
            f"运行时长：{elapsed:.1f}s",
            "收到消息："
            f"{self.received}（PASS={self.passed}，NOT_MATCH={self.not_matched}，"
            f"FAIL={self.failed}）",
        ]
        if self.suppressed_not_match or self.suppressed_fail:
            lines.append(
                "已限流未逐条输出："
                f"NOT_MATCH={self.suppressed_not_match}，FAIL={self.suppressed_fail}"
            )
        rate = self.overall_rate()
        if rate is not None:
            lines.append(
                f"匹配数据包平均频率：{rate:.1f}Hz（协议上限 {self.max_rate_hz:.1f}Hz）"
            )
        elif self.passed == 1:
            lines.append("匹配数据包：1 条，样本不足无法计算频率")

        if self.rate_exceeded:
            lines.append(
                f"结论：观测频率曾超过协议上限 {self.max_rate_hz:.1f}Hz，需现场确认。"
            )
        elif self.passed:
            lines.append("结论：收到符合协议描述的数据包。")
        else:
            lines.append(
                "结论：超时前未收到合法数据包（data 长度始终与协议 "
                f"payload_length {self.spec.payload_length} 不符）。"
            )
        return lines


def run_validator(
    args: argparse.Namespace,
    spec: ProtocolSpec,
    session: ValidationSession,
    decoder: Callable[[bytes], bytes],
    *,
    client_factory: Callable[[str], object] = default_client_factory,
    clock: Callable[[], float] = time.monotonic,
    loop_timeout: float = DEFAULT_LOOP_TIMEOUT,
    deadline: float | None = None,
) -> int:
    """通过接收层订阅并驱动校验；只订阅，绝不 publish。

    MQTT 传输细节（连接/订阅/超时/停止/计数）全部由 ``receiver.MessageReceiver``
    负责，本函数只把消息交给校验会话。
    """

    if deadline is None:
        deadline = clock() + args.timeout

    def on_connected(subscribed) -> None:
        session.output(
            f"已连接 {args.broker}:{args.port}，已订阅 {', '.join(subscribed)}，"
            f"client-id={args.client_id}（只订阅，不发布）"
        )

    receiver_ = MessageReceiver(
        broker=args.broker,
        port=args.port,
        client_id=args.client_id,
        topics=[spec.mqtt_topic],
        client_factory=client_factory,
        clock=clock,
        loop_timeout=loop_timeout,
    )
    result = receiver_.run(
        timeout=args.timeout,
        handler=lambda envelope: session.handle_message(
            topic=envelope.topic, payload=envelope.payload, decode_data=decoder
        ),
        should_stop=session.is_complete,
        on_connected=on_connected,
        deadline=deadline,
    )

    if result.connection_error is not None:
        session.output(f"连接 {args.broker}:{args.port} 失败：{result.connection_error}")
        return EXIT_ERROR

    if not result.connected:
        session.output(
            f"在 {args.timeout:.0f}s 内未完成 MQTT 握手（rc={result.connect_rc}）；"
            "请确认 broker 与 client-id 可用。"
        )
        return EXIT_ERROR

    for line in session.summary_lines():
        session.output(line)

    if session.is_complete():
        session.output(f"已达到 --max-matches {session.max_matches}，提前结束。")
    return session.exit_code()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "校验 MQTT CustomByteBlock 上的自定义协议字节块；只订阅并解析，不发布任何消息。"
        )
    )
    parser.add_argument(
        "--spec",
        type=Path,
        default=DEFAULT_SPEC_PATH,
        help=f"JSON 协议描述路径，默认 {DEFAULT_SPEC_PATH}",
    )
    parser.add_argument("--broker", help="MQTT broker 主机名或 IP，必须显式指定")
    parser.add_argument("--port", type=int, help="MQTT 端口，必须显式指定")
    parser.add_argument("--client-id", help="本次校验使用的 MQTT Client ID，必须显式指定")
    parser.add_argument(
        "--allow-live-connect",
        action="store_true",
        help="确认允许连接现场 broker；相同 Client ID 可能挤掉正在运行的客户端",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help=f"总超时秒数，默认 {DEFAULT_TIMEOUT:g}",
    )
    parser.add_argument(
        "--max-matches",
        type=int,
        default=None,
        help="收到多少个合法数据包后提前退出，便于一次性验证",
    )
    parser.add_argument(
        "--rate-window",
        type=float,
        default=DEFAULT_RATE_WINDOW,
        help=f"频率统计窗口秒数，默认 {DEFAULT_RATE_WINDOW:g}",
    )
    parser.add_argument(
        "--max-rate-hz",
        type=float,
        default=None,
        help="协议允许的最大频率，默认取 JSON 中的 max_rate_hz",
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
    if args.rate_window <= 0:
        parser.error("--rate-window 必须为正数")
    if args.max_matches is not None and args.max_matches < 1:
        parser.error("--max-matches 必须为正整数")
    if args.max_rate_hz is not None and args.max_rate_hz <= 0:
        parser.error("--max-rate-hz 必须为正数")
    if not args.allow_live_connect:
        parser.error(
            "连接 broker 可能挤掉相同 Client ID 的在线客户端，"
            "确认现场安全后再加 --allow-live-connect"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)

    try:
        spec = ProtocolSpec.load(args.spec)
        decoder = build_data_decoder(spec)
    except ValidatorError as error:
        print(f"错误：{error}", file=sys.stderr)
        return EXIT_ERROR

    print(
        f"协议描述：{spec.protocol_name} {spec.protocol_version} | "
        f"topic={spec.mqtt_topic} | 命令字={spec.source_command_id} | "
        f"payload_length={spec.payload_length} | 字段="
        + ", ".join(item.name for item in spec.fields)
    )

    deadline = time.monotonic() + args.timeout

    if not wait_for_port(args.broker, args.port, deadline=deadline):
        print(
            f"错误：{args.broker}:{args.port} 在 {args.timeout:g}s 内不可达",
            file=sys.stderr,
        )
        return EXIT_ERROR

    session = ValidationSession(
        spec,
        max_matches=args.max_matches,
        rate_window=args.rate_window,
        max_rate_hz=args.max_rate_hz,
    )
    try:
        return run_validator(args, spec, session, decoder, deadline=deadline)
    except ValidatorError as error:
        print(f"错误：{error}", file=sys.stderr)
        return EXIT_ERROR
    except KeyboardInterrupt:
        for line in session.summary_lines():
            session.output(line)
        session.output("已被用户中断。")
        return EXIT_INTERRUPTED


if __name__ == "__main__":
    raise SystemExit(main())
