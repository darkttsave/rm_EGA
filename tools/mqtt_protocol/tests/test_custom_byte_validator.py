"""custom_byte_validator 的离线单元测试。

不依赖正在运行的 MQTT broker：连接流程用假客户端与假时钟驱动。

运行：
    .venv-mqtt/bin/python -m unittest discover -s tools/mqtt_protocol/tests -p 'test_*.py' -v
    .venv-mqtt/bin/python -m unittest tools.mqtt_protocol.tests.test_custom_byte_validator

未安装 protobuf 运行时（sim/robomaster_pb2.py 所需）时，涉及真实 Protobuf
编解码的用例会以 skip 呈现，其余用例仍会执行。
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

PACKAGE_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = PACKAGE_DIR.parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.mqtt_protocol import custom_byte_validator as validator  # noqa: E402

SPEC_PATH = PACKAGE_DIR / "protocol_specs/sentry_bt_status_v1.json"

try:  # pragma: no cover - 仅用于在缺少运行时的情况下降级
    import google.protobuf  # noqa: F401

    HAS_PROTOBUF = True
except Exception:  # pragma: no cover
    HAS_PROTOBUF = False

PROTOBUF_REASON = "需要 protobuf 运行时（sim/robomaster_pb2.py 依赖），请使用 .venv-mqtt/bin/python"


def load_repository_spec() -> validator.ProtocolSpec:
    return validator.ProtocolSpec.load(SPEC_PATH)


def make_payload(data: bytes) -> bytes:
    """用仓库既有 pb2 构造 CustomByteBlock（不手工拼 wire format）。"""

    sim_dir = str(PROJECT_ROOT / "sim")
    if sim_dir not in sys.path:
        sys.path.insert(0, sim_dir)
    import robomaster_pb2  # noqa: PLC0415

    return robomaster_pb2.CustomByteBlock(data=data).SerializeToString()


def raw_decoder(spec: validator.ProtocolSpec):
    """把 MQTT payload 直接当作业务字节的测试替身，用于不依赖 protobuf 的流程用例。"""

    return lambda payload: bytes(payload)


_DECODER_CACHE: dict[str, object] = {}


def repository_decoder(spec: validator.ProtocolSpec):
    """真实 pb2 解码器。缓存在模块级，避免作为类属性时被绑定成方法。"""

    key = str(spec.source_path)
    if key not in _DECODER_CACHE:
        _DECODER_CACHE[key] = validator.build_data_decoder(spec)
    return _DECODER_CACHE[key]


def base_mapping(**overrides) -> dict:
    mapping = {
        "protocol_name": "unit_test_protocol",
        "protocol_version": "v1",
        "mqtt_topic": "CustomByteBlock",
        "protobuf_message_type": "robomaster.CustomByteBlock",
        "protobuf_data_field": "data",
        "source_command_id": "0x0310",
        "payload_length": 4,
        "byte_order": "little",
        "fields": [
            {"name": "alpha", "offset": 0, "type": "uint8", "byte_order": "little"}
        ],
    }
    mapping.update(overrides)
    return mapping


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now

    def advance(self, delta: float) -> None:
        self.now += delta


class FakeMessage:
    def __init__(self, topic: str, payload: bytes) -> None:
        self.topic = topic
        self.payload = payload


class FakeClient:
    """模拟 paho 客户端：不触网，按脚本投递消息，并拒绝任何 publish。"""

    def __init__(
        self,
        *,
        clock: FakeClock,
        script: list[tuple[str, bytes]] | None = None,
        loop_step: float = 0.1,
    ) -> None:
        self.clock = clock
        self.script = list(script or [])
        self.loop_step = loop_step
        self.on_connect = None
        self.on_message = None
        self.subscribed: list[str] = []
        self.published: list[tuple] = []
        self.connected_to: tuple[str, int] | None = None
        self.disconnected = False
        self._announced = False

    def subscribe(self, topic, *args, **kwargs):
        self.subscribed.append(topic)

    def publish(self, *args, **kwargs):
        self.published.append((args, kwargs))
        raise AssertionError("本工具只订阅，不得向 broker 发布消息")

    def connect(self, host, port, keepalive=60):
        self.connected_to = (host, port)
        return 0

    def disconnect(self):
        self.disconnected = True

    def loop(self, timeout=1.0):
        if not self._announced:
            self._announced = True
            if self.on_connect is not None:
                self.on_connect(self, None, {}, 0)
            self.clock.advance(self.loop_step)
            return 0
        self.clock.advance(self.loop_step)
        if self.script and self.on_message is not None:
            topic, payload = self.script.pop(0)
            self.on_message(self, None, FakeMessage(topic, payload))
        return 0


def make_args(**overrides) -> argparse.Namespace:
    values = {
        "broker": "127.0.0.1",
        "port": 3333,
        "client_id": "unit-test-client",
        "allow_live_connect": True,
        "timeout": 1.0,
        "max_matches": None,
        "rate_window": 1.0,
        "max_rate_hz": None,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class SpecValidationTest(unittest.TestCase):
    """用例 6、7：JSON 描述缺失、越界与重叠。"""

    def test_repository_spec_is_valid(self) -> None:
        spec = load_repository_spec()

        self.assertEqual("sentry_bt_status", spec.protocol_name)
        self.assertEqual("v1", spec.protocol_version)
        self.assertEqual("CustomByteBlock", spec.mqtt_topic)
        self.assertEqual("robomaster.CustomByteBlock", spec.protobuf_message_type)
        self.assertEqual("0x0310", spec.source_command_id)
        self.assertEqual(4, spec.payload_length)

    def test_repository_spec_field_layout(self) -> None:
        spec = load_repository_spec()
        layout = [(item.name, item.offset, item.type_name, item.byte_order) for item in spec.fields]

        self.assertEqual(
            [
                ("current_stance", 0, "uint8", "little"),
                ("bt_remain_time", 1, "uint16", "little"),
                ("bt_game_status", 3, "uint8", "little"),
            ],
            layout,
        )

    def test_repository_spec_does_not_define_stance_semantics(self) -> None:
        raw = json.loads(SPEC_PATH.read_text(encoding="utf-8"))

        for item in raw["fields"]:
            for forbidden in ("values", "enum", "semantics", "meaning"):
                self.assertNotIn(forbidden, item)
        self.assertNotIn("stance_values", raw)

    def test_missing_top_level_key_is_rejected(self) -> None:
        for key in ("payload_length", "mqtt_topic", "protobuf_message_type"):
            with self.subTest(key=key):
                mapping = base_mapping()
                del mapping[key]
                with self.assertRaises(validator.SpecError) as context:
                    validator.parse_spec_mapping(mapping)
                self.assertIn(key, str(context.exception))

    def test_missing_field_key_is_rejected(self) -> None:
        for key in ("name", "offset", "type"):
            with self.subTest(key=key):
                mapping = base_mapping()
                del mapping["fields"][0][key]
                with self.assertRaises(validator.SpecError) as context:
                    validator.parse_spec_mapping(mapping)
                self.assertIn("fields[0]", str(context.exception))
                self.assertIn(key, str(context.exception))

    def test_missing_fields_array_is_rejected(self) -> None:
        mapping = base_mapping()
        del mapping["fields"]
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("fields", str(context.exception))

    def test_field_out_of_bounds_is_rejected(self) -> None:
        mapping = base_mapping(
            fields=[{"name": "wide", "offset": 3, "type": "uint16", "byte_order": "little"}]
        )
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("越界", str(context.exception))

    def test_overlapping_fields_are_rejected(self) -> None:
        mapping = base_mapping(
            fields=[
                {"name": "first", "offset": 0, "type": "uint16", "byte_order": "little"},
                {"name": "second", "offset": 1, "type": "uint8", "byte_order": "little"},
            ]
        )
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("重叠", str(context.exception))

    def test_duplicate_field_name_is_rejected(self) -> None:
        mapping = base_mapping(
            fields=[
                {"name": "same", "offset": 0, "type": "uint8", "byte_order": "little"},
                {"name": "same", "offset": 1, "type": "uint8", "byte_order": "little"},
            ]
        )
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("重复", str(context.exception))

    def test_unknown_field_type_is_rejected(self) -> None:
        mapping = base_mapping(
            fields=[{"name": "x", "offset": 0, "type": "float64", "byte_order": "little"}]
        )
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("type", str(context.exception))

    def test_invalid_byte_order_is_rejected(self) -> None:
        mapping = base_mapping(byte_order="middle")
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        self.assertIn("byte_order", str(context.exception))

    def test_issues_are_aggregated(self) -> None:
        mapping = base_mapping(byte_order="middle", payload_length=0)
        del mapping["protocol_name"]
        with self.assertRaises(validator.SpecError) as context:
            validator.parse_spec_mapping(mapping)
        message = str(context.exception)
        self.assertIn("protocol_name", message)
        self.assertIn("payload_length", message)
        self.assertIn("byte_order", message)

    def test_missing_spec_file_is_reported(self) -> None:
        with self.assertRaises(validator.SpecError):
            validator.ProtocolSpec.load(PACKAGE_DIR / "protocol_specs/does_not_exist.json")

    def test_invalid_json_is_reported(self) -> None:
        path = PROJECT_ROOT / "tmp" / "validator-bad-spec.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("{ not json", encoding="utf-8")
        try:
            with self.assertRaises(validator.SpecError):
                validator.ProtocolSpec.load(path)
        finally:
            path.unlink(missing_ok=True)


class SpecDrivenBehaviourTest(unittest.TestCase):
    """协议驱动：长度与字段名全部来自 JSON，不写死。"""

    def test_length_follows_spec_not_hardcoded_four(self) -> None:
        mapping = base_mapping(
            payload_length=2,
            fields=[{"name": "only", "offset": 0, "type": "uint16", "byte_order": "big"}],
        )
        spec = validator.parse_spec_mapping(mapping)

        outcome = validator.evaluate_payload(
            topic="CustomByteBlock",
            payload=b"\x01\x02",
            spec=spec,
            decode_data=raw_decoder(spec),
        )
        self.assertEqual(validator.STATUS_PASS, outcome.status)
        self.assertEqual({"only": 0x0102}, outcome.values)

    def test_four_byte_payload_is_not_match_for_two_byte_spec(self) -> None:
        spec = validator.parse_spec_mapping(
            base_mapping(
                payload_length=2,
                fields=[{"name": "only", "offset": 0, "type": "uint16", "byte_order": "little"}],
            )
        )

        outcome = validator.evaluate_payload(
            topic="CustomByteBlock",
            payload=b"\x01\x02\x03\x04",
            spec=spec,
            decode_data=raw_decoder(spec),
        )
        self.assertEqual(validator.STATUS_NOT_MATCH, outcome.status)

    def test_field_names_in_output_come_from_spec(self) -> None:
        spec = validator.parse_spec_mapping(
            base_mapping(
                payload_length=1,
                fields=[{"name": "renamed_field", "offset": 0, "type": "uint8"}],
            )
        )
        outcome = validator.evaluate_payload(
            topic="CustomByteBlock",
            payload=b"\x2a",
            spec=spec,
            decode_data=raw_decoder(spec),
        )

        self.assertEqual({"renamed_field": 42}, outcome.values)
        self.assertIn("renamed_field=42", validator.format_outcome(outcome, spec))

    def test_module_does_not_hardcode_protocol_fields(self) -> None:
        source = Path(validator.__file__).read_text(encoding="utf-8")

        for field_name in ("current_stance", "bt_remain_time", "bt_game_status"):
            self.assertNotIn(field_name, source)
        self.assertNotRegex(source, r"len\(\s*\w+\s*\)\s*[=!]=\s*4\b")

    def test_module_never_publishes(self) -> None:
        source = Path(validator.__file__).read_text(encoding="utf-8")
        self.assertNotIn(".publish(", source)


class PayloadEvaluationTest(unittest.TestCase):
    """用例 1-5、8、10：真实 Protobuf 编解码路径。"""

    @classmethod
    def setUpClass(cls) -> None:
        if not HAS_PROTOBUF:
            raise unittest.SkipTest(PROTOBUF_REASON)
        cls.spec = load_repository_spec()

    def evaluate(self, data: bytes):
        return validator.evaluate_payload(
            topic="CustomByteBlock",
            payload=make_payload(data),
            spec=self.spec,
            decode_data=repository_decoder(self.spec),
        )

    def test_valid_four_byte_payload_passes(self) -> None:
        outcome = self.evaluate(bytes([1, 0x01, 0x02, 0x03]))

        self.assertEqual(validator.STATUS_PASS, outcome.status)
        self.assertIsNone(outcome.reason)
        self.assertEqual(4, outcome.data_length)
        self.assertEqual("01 01 02 03", outcome.data_hex)
        self.assertIn("-> PASS", validator.format_outcome(outcome, self.spec))

    def test_uint16_is_little_endian(self) -> None:
        outcome = self.evaluate(bytes([1, 0x01, 0x02, 0x03]))

        self.assertEqual(0x0201, outcome.values["bt_remain_time"])
        self.assertNotEqual(0x0102, outcome.values["bt_remain_time"])

    def test_all_three_fields_are_parsed(self) -> None:
        outcome = self.evaluate(bytes([0x07, 0x34, 0x12, 0x09]))

        self.assertEqual(
            {"current_stance": 7, "bt_remain_time": 0x1234, "bt_game_status": 9},
            outcome.values,
        )

    def test_data_shorter_than_expected_is_not_match(self) -> None:
        outcome = self.evaluate(bytes([1, 0x02, 0x00]))

        self.assertEqual(validator.STATUS_NOT_MATCH, outcome.status)
        self.assertEqual(3, outcome.data_length)
        self.assertIn("payload_length 4", outcome.reason)

    def test_data_longer_than_expected_is_not_match(self) -> None:
        outcome = self.evaluate(bytes(range(7)))

        self.assertEqual(validator.STATUS_NOT_MATCH, outcome.status)
        self.assertEqual(7, outcome.data_length)

    def test_empty_data_is_not_match(self) -> None:
        outcome = self.evaluate(b"")

        self.assertEqual(validator.STATUS_NOT_MATCH, outcome.status)
        self.assertEqual(0, outcome.data_length)

    def test_non_target_custom_byte_block_is_not_match(self) -> None:
        """共享 Topic 上承载视频等其它数据时不得误报为合法哨兵状态。"""

        for length in (1, 2, 3, 5, 8, 64, 1024):
            with self.subTest(length=length):
                outcome = self.evaluate(bytes(length))
                self.assertEqual(validator.STATUS_NOT_MATCH, outcome.status)
                self.assertIsNone(outcome.values)

    def test_invalid_protobuf_is_fail(self) -> None:
        for raw in (b"\xff\xff\xff", b"\x0a\x04\x01", b"\x0a\xff\xff\xff\xff\x0f"):
            with self.subTest(raw=raw.hex()):
                outcome = validator.evaluate_payload(
                    topic="CustomByteBlock",
                    payload=raw,
                    spec=self.spec,
                    decode_data=repository_decoder(self.spec),
                )
                self.assertEqual(validator.STATUS_FAIL, outcome.status)
                self.assertIn("Protobuf 解析失败", outcome.reason)

    def test_repository_pb2_message_type_matches_spec(self) -> None:
        message_class = validator.load_custom_byte_block(self.spec)

        self.assertEqual("robomaster.CustomByteBlock", message_class.DESCRIPTOR.full_name)
        self.assertIn("data", message_class.DESCRIPTOR.fields_by_name)

    def test_mismatched_message_type_is_reported(self) -> None:
        spec = validator.parse_spec_mapping(
            base_mapping(protobuf_message_type="robomaster.NotARealMessage")
        )
        with self.assertRaises(validator.ValidatorError):
            validator.load_custom_byte_block(spec)

    def test_unknown_data_field_is_reported(self) -> None:
        spec = validator.parse_spec_mapping(base_mapping(protobuf_data_field="nope"))
        with self.assertRaises(validator.ValidatorError):
            validator.load_custom_byte_block(spec)


class SessionStatisticsTest(unittest.TestCase):
    """限流、频率统计与退出码。"""

    def setUp(self) -> None:
        self.spec = load_repository_spec()
        self.lines: list[str] = []

    def make_session(self, **overrides) -> validator.ValidationSession:
        options = {"output": self.lines.append}
        options.update(overrides)
        return validator.ValidationSession(self.spec, **options)

    def test_not_match_logging_is_throttled(self) -> None:
        session = self.make_session()
        for _ in range(250):
            session.handle_message(
                topic="CustomByteBlock",
                payload=b"\x01\x02\x03",
                decode_data=raw_decoder(self.spec),
            )

        self.assertEqual(250, session.not_matched)
        self.assertEqual(0, session.passed)
        detailed = [line for line in self.lines if line.startswith("[")]
        self.assertEqual(5, len(detailed))
        self.assertEqual(245, session.suppressed_not_match)

    def test_rate_above_limit_warns_and_marks_exceeded(self) -> None:
        session = self.make_session()
        payload = bytes([1, 0x01, 0x02, 0x03])
        for index in range(60):
            session.handle_message(
                topic="CustomByteBlock",
                payload=payload,
                decode_data=raw_decoder(self.spec),
                now=index / 60.0,
            )

        self.assertTrue(session.rate_exceeded)
        warnings = [line for line in self.lines if "警告" in line]
        self.assertGreaterEqual(len(warnings), 1)
        self.assertLessEqual(len(warnings), 3)

    def test_rate_below_limit_does_not_warn(self) -> None:
        session = self.make_session()
        payload = bytes([1, 0x01, 0x02, 0x03])
        for index in range(40):
            session.handle_message(
                topic="CustomByteBlock",
                payload=payload,
                decode_data=raw_decoder(self.spec),
                now=index / 40.0,
            )

        self.assertFalse(session.rate_exceeded)
        self.assertEqual([], [line for line in self.lines if "警告" in line])

    def test_rate_limit_follows_configured_threshold(self) -> None:
        session = self.make_session(max_rate_hz=70.0)
        payload = bytes([1, 0x01, 0x02, 0x03])
        for index in range(60):
            session.handle_message(
                topic="CustomByteBlock",
                payload=payload,
                decode_data=raw_decoder(self.spec),
                now=index / 60.0,
            )

        self.assertFalse(session.rate_exceeded)

    def test_decoder_exception_does_not_crash(self) -> None:
        session = self.make_session()

        def exploding_decoder(payload: bytes) -> bytes:
            raise RuntimeError("boom")

        outcome = session.handle_message(
            topic="CustomByteBlock", payload=b"x", decode_data=exploding_decoder
        )
        self.assertEqual(validator.STATUS_FAIL, outcome.status)
        self.assertEqual(1, session.failed)

    def test_exit_code_is_nonzero_without_match(self) -> None:
        session = self.make_session()
        session.handle_message(
            topic="CustomByteBlock",
            payload=b"\x01\x02\x03",
            decode_data=raw_decoder(self.spec),
        )

        self.assertEqual(validator.EXIT_NO_MATCH, session.exit_code())
        self.assertNotEqual(0, session.exit_code())

    def test_exit_code_is_zero_with_match(self) -> None:
        session = self.make_session()
        session.handle_message(
            topic="CustomByteBlock",
            payload=bytes([1, 2, 3, 4]),
            decode_data=raw_decoder(self.spec),
        )

        self.assertEqual(validator.EXIT_OK, session.exit_code())

    def test_summary_reports_counts(self) -> None:
        session = self.make_session()
        session.handle_message(
            topic="CustomByteBlock",
            payload=bytes([1, 2, 3, 4]),
            decode_data=raw_decoder(self.spec),
        )
        summary = "\n".join(session.summary_lines())

        self.assertIn("收到消息：1", summary)
        self.assertIn("PASS=1", summary)

    def test_max_matches_completion_flag(self) -> None:
        session = self.make_session(max_matches=2)
        self.assertFalse(session.is_complete())
        for _ in range(2):
            session.handle_message(
                topic="CustomByteBlock",
                payload=bytes([1, 2, 3, 4]),
                decode_data=raw_decoder(self.spec),
            )
        self.assertTrue(session.is_complete())


class RunValidatorFlowTest(unittest.TestCase):
    """用例 9、10：超时与非零退出码、达到 --max-matches 提前结束。"""

    def setUp(self) -> None:
        self.spec = load_repository_spec()
        self.lines: list[str] = []

    def test_timeout_without_match_returns_nonzero(self) -> None:
        clock = FakeClock()
        script = [("CustomByteBlock", b"\x01\x02\x03")] * 200
        client = FakeClient(clock=clock, script=script)
        session = validator.ValidationSession(self.spec, output=self.lines.append)

        code = validator.run_validator(
            make_args(timeout=1.0),
            self.spec,
            session,
            raw_decoder(self.spec),
            client_factory=lambda client_id: client,
            clock=clock,
            loop_timeout=0.1,
        )

        self.assertEqual(validator.EXIT_NO_MATCH, code)
        self.assertNotEqual(0, code)
        self.assertEqual(0, session.passed)
        self.assertTrue(client.disconnected)
        self.assertEqual(["CustomByteBlock"], client.subscribed)

    def test_valid_message_passes_and_exits_zero(self) -> None:
        clock = FakeClock()
        client = FakeClient(
            clock=clock, script=[("CustomByteBlock", bytes([1, 0x01, 0x02, 0x03]))]
        )
        session = validator.ValidationSession(
            self.spec, max_matches=1, output=self.lines.append
        )

        code = validator.run_validator(
            make_args(timeout=10.0, max_matches=1),
            self.spec,
            session,
            raw_decoder(self.spec),
            client_factory=lambda client_id: client,
            clock=clock,
            loop_timeout=0.1,
        )

        self.assertEqual(validator.EXIT_OK, code)
        self.assertEqual(1, session.passed)
        self.assertTrue(session.is_complete())
        self.assertTrue(client.disconnected)
        self.assertLess(clock.now, 10.0, "达到 --max-matches 后应提前结束")
        self.assertEqual([], client.published)

    def test_invalid_protobuf_over_mqtt_does_not_crash(self) -> None:
        clock = FakeClock()
        client = FakeClient(
            clock=clock, script=[("CustomByteBlock", b"\xff\xff\xff")] * 5
        )
        session = validator.ValidationSession(self.spec, output=self.lines.append)

        def strict_decoder(payload: bytes) -> bytes:
            raise ValueError("invalid protobuf")

        code = validator.run_validator(
            make_args(timeout=0.5),
            self.spec,
            session,
            strict_decoder,
            client_factory=lambda client_id: client,
            clock=clock,
        )

        self.assertEqual(validator.EXIT_NO_MATCH, code)
        self.assertGreaterEqual(session.failed, 1)

    def test_connection_failure_returns_error(self) -> None:
        clock = FakeClock()

        class RefusingClient(FakeClient):
            def connect(self, host, port, keepalive=60):
                raise OSError("connection refused")

        client = RefusingClient(clock=clock)
        session = validator.ValidationSession(self.spec, output=self.lines.append)

        code = validator.run_validator(
            make_args(timeout=1.0),
            self.spec,
            session,
            raw_decoder(self.spec),
            client_factory=lambda client_id: client,
            clock=clock,
        )

        self.assertEqual(validator.EXIT_ERROR, code)


class CommandLineTest(unittest.TestCase):
    """安全约束：必须显式提供连接参数与 --allow-live-connect。"""

    def parse(self, argv: list[str]) -> argparse.Namespace:
        parser = validator.build_parser()
        args = parser.parse_args(argv)
        validator.validate_args(parser, args)
        return args

    def assert_parse_error(self, argv: list[str]) -> str:
        """断言参数被拒绝，并返回 argparse 写入 stderr 的提示（保持测试输出干净）。"""

        buffer = io.StringIO()
        with redirect_stderr(buffer), redirect_stdout(buffer):
            with self.assertRaises(SystemExit) as context:
                self.parse(argv)
        self.assertEqual(2, context.exception.code)
        return buffer.getvalue()

    def test_valid_arguments_are_accepted(self) -> None:
        args = self.parse(
            [
                "--broker", "127.0.0.1",
                "--port", "3333",
                "--client-id", "some-client",
                "--allow-live-connect",
            ]
        )

        self.assertEqual("127.0.0.1", args.broker)
        self.assertEqual(3333, args.port)
        self.assertEqual("some-client", args.client_id)
        self.assertEqual(validator.DEFAULT_SPEC_PATH, args.spec)

    def test_live_connect_requires_explicit_opt_in(self) -> None:
        message = self.assert_parse_error(
            ["--broker", "127.0.0.1", "--port", "3333", "--client-id", "x"]
        )

        self.assertIn("--allow-live-connect", message)
        self.assertIn("挤掉", message)

    def test_connection_parameters_are_mandatory(self) -> None:
        cases = [
            ["--port", "3333", "--client-id", "x", "--allow-live-connect"],
            ["--broker", "127.0.0.1", "--client-id", "x", "--allow-live-connect"],
            ["--broker", "127.0.0.1", "--port", "3333", "--allow-live-connect"],
        ]
        for argv in cases:
            with self.subTest(argv=argv):
                self.assert_parse_error(argv)

    def test_client_id_has_no_default(self) -> None:
        parser = validator.build_parser()
        args = parser.parse_args([])

        self.assertIsNone(args.client_id)
        self.assertIsNone(args.broker)
        self.assertIsNone(args.port)

    def test_help_exits_zero(self) -> None:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            with self.assertRaises(SystemExit) as context:
                validator.build_parser().parse_args(["--help"])
        self.assertEqual(0, context.exception.code)
        self.assertIn("--allow-live-connect", buffer.getvalue())

    def test_broker_scheme_is_rejected(self) -> None:
        message = self.assert_parse_error(
            [
                "--broker", "tcp://127.0.0.1",
                "--port", "3333",
                "--client-id", "x",
                "--allow-live-connect",
            ]
        )

        self.assertIn("协议前缀", message)

    def test_invalid_limits_are_rejected(self) -> None:
        for extra in (["--timeout", "0"], ["--max-matches", "0"], ["--max-rate-hz", "-1"]):
            with self.subTest(extra=extra):
                self.assert_parse_error(
                    [
                        "--broker", "127.0.0.1",
                        "--port", "3333",
                        "--client-id", "x",
                        "--allow-live-connect",
                        *extra,
                    ]
                )


if __name__ == "__main__":
    unittest.main()
