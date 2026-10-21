"""解码层与 inspector 的离线单元测试。

覆盖：合法 49 组增量、负数保留、X/Y 数量不一致、数量不为 49、非法 intention、
增量越界、sender_id 不匹配、起点越界、累计路径越界、非法 Protobuf、
未支持 Topic 输出原始信息、JSON Lines 落盘、NO_MESSAGE 非零退出。

不依赖真实 Broker：事件循环用假客户端与假时钟驱动。
"""

from __future__ import annotations

import base64
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.mqtt_protocol import decoders, inspector, receiver  # noqa: E402

try:  # pragma: no cover - 缺少运行时则跳过真实编解码用例
    import google.protobuf  # noqa: F401

    HAS_PROTOBUF = True
except Exception:  # pragma: no cover
    HAS_PROTOBUF = False

PROTOBUF_REASON = (
    "需要 protobuf 运行时（sim/robomaster_pb2.py 依赖），请使用 .venv-mqtt/bin/python"
)


def envelope(topic: str, payload: bytes) -> receiver.MessageEnvelope:
    return receiver.MessageEnvelope(
        topic=topic, payload=payload, received_at=datetime.now()
    )


def build_path_payload(
    *,
    intention: int = 1,
    start_x: int = 10,
    start_y: int = 20,
    offset_x: list[int] | None = None,
    offset_y: list[int] | None = None,
    sender_id: int = 7,
) -> bytes:
    """用仓库既有 pb2 构造 RobotPathPlanInfo，不手工拼 wire format。"""

    message_class = decoders.load_message_class("RobotPathPlanInfo")
    message = message_class()
    message.intention = intention
    message.start_pos_x = start_x
    message.start_pos_y = start_y
    message.offset_x.extend([1] * 49 if offset_x is None else offset_x)
    message.offset_y.extend([1] * 49 if offset_y is None else offset_y)
    message.sender_id = sender_id
    return message.SerializeToString()


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
        self.qos = 0
        self.retain = False


class FakeClient:
    def __init__(self, *, clock: FakeClock, script: list[FakeMessage] | None = None):
        self.clock = clock
        self.script = list(script or [])
        self.on_connect = None
        self.on_message = None
        self.subscribed: list[str] = []
        self.disconnected = False
        self._announced = False

    def subscribe(self, topic, *args, **kwargs):
        self.subscribed.append(topic)

    def publish(self, *args, **kwargs):
        raise AssertionError("本工具只订阅，不得发布消息")

    def connect(self, host, port, keepalive=60):
        return 0

    def disconnect(self):
        self.disconnected = True

    def loop(self, timeout=1.0):
        if not self._announced:
            self._announced = True
            if self.on_connect is not None:
                self.on_connect(self, None, {}, 0)
            self.clock.advance(0.1)
            return 0
        self.clock.advance(0.1)
        if self.script and self.on_message is not None:
            self.on_message(self, None, self.script.pop(0))
        return 0


class RegistryTest(unittest.TestCase):
    def test_only_robot_path_plan_is_registered(self):
        # robomaster_pb2.py 里还有很多消息，但白名单只允许这一个
        self.assertEqual({"RobotPathPlanInfo"}, set(decoders.DECODERS))

    def test_unsupported_topic_is_not_registered(self):
        self.assertFalse(decoders.is_supported("GameStatus"))
        self.assertIsNone(decoders.decode("GameStatus", b"\x00"))


class RawPreviewTest(unittest.TestCase):
    def test_preview_is_length_limited(self):
        preview = inspector.preview_hex(bytes(range(100)))

        # 只预览前 32 字节，并标注剩余长度
        self.assertIn("… (+68B)", preview)
        hex_part = preview.split(" … ")[0]
        self.assertEqual(inspector.PREVIEW_HEX_BYTES, len(hex_part.split(" ")))
        self.assertTrue(preview.startswith("00 01 02"))

    def test_unsupported_topic_reports_raw_info_without_protobuf(self):
        record = inspector.inspect_message(envelope("GameStatus", b"\x53\x49\x4d"))

        self.assertEqual(inspector.STATUS_UNSUPPORTED, record.status)
        self.assertEqual(3, record.payload_length)
        self.assertEqual({}, record.fields)
        lines = "\n".join(record.to_lines())
        self.assertIn("payload_hex=53 49 4d", lines)
        self.assertIn("UNSUPPORTED", lines)


@unittest.skipUnless(HAS_PROTOBUF, PROTOBUF_REASON)
class RobotPathPlanDecoderTest(unittest.TestCase):
    def test_valid_49_offsets_pass(self):
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload())
        )

        self.assertEqual(inspector.STATUS_PASS, record.status, record.issues)
        self.assertEqual([], record.issues)
        self.assertEqual(1, record.fields["intention"])
        self.assertEqual(10, record.fields["start_pos_x"])
        self.assertEqual(20, record.fields["start_pos_y"])
        self.assertEqual(49, record.fields["offset_x_count"])
        self.assertEqual(49, record.fields["offset_y_count"])
        self.assertEqual(7, record.fields["sender_id"])
        # 起点 + 49 个增量 = 50 个点
        self.assertEqual(50, record.fields["point_count"])
        self.assertEqual(50, len(record.points))

    def test_negative_offsets_are_preserved(self):
        record = inspector.inspect_message(
            envelope(
                "RobotPathPlanInfo",
                build_path_payload(
                    start_x=200,
                    start_y=100,
                    offset_x=[-2] * 49,
                    offset_y=[-1] * 49,
                ),
            )
        )

        self.assertEqual(inspector.STATUS_PASS, record.status, record.issues)
        # 第一个增量后应当减小，而不是被截断成 0
        self.assertEqual((198, 99), record.points[1])
        # 49 次 -2 / -1 后到达 (102, 51)
        self.assertEqual((102, 51), record.points[-1])

    def test_mismatched_xy_counts_not_match(self):
        record = inspector.inspect_message(
            envelope(
                "RobotPathPlanInfo",
                build_path_payload(offset_x=[1] * 49, offset_y=[1] * 48),
            )
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("数量不一致" in issue for issue in record.issues))

    def test_offset_count_not_49_not_match(self):
        record = inspector.inspect_message(
            envelope(
                "RobotPathPlanInfo",
                build_path_payload(offset_x=[1] * 48, offset_y=[1] * 48),
            )
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("不是 49" in issue for issue in record.issues))

    def test_invalid_intention_not_match(self):
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload(intention=4))
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("intention" in issue for issue in record.issues))

    def test_delta_out_of_int8_range_not_match(self):
        offsets = [1] * 49
        offsets[0] = 200
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload(offset_x=offsets))
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("int8" in issue for issue in record.issues))

    def test_sender_id_mismatch_not_match(self):
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload(sender_id=7)),
            expected_sender_id=9,
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("sender_id" in issue for issue in record.issues))

    def test_start_position_out_of_field_not_match(self):
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload(start_x=300))
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("start_pos_x" in issue for issue in record.issues))

    def test_cumulative_path_out_of_field_not_match(self):
        record = inspector.inspect_message(
            envelope(
                "RobotPathPlanInfo",
                build_path_payload(start_x=275, offset_x=[10] * 49),
            )
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        self.assertTrue(any("越出场地边界" in issue for issue in record.issues))

    def test_semantic_failure_still_reports_parsed_fields(self):
        record = inspector.inspect_message(
            envelope("RobotPathPlanInfo", build_path_payload(intention=9))
        )

        self.assertEqual(inspector.STATUS_NOT_MATCH, record.status)
        # 语义不通过也必须保留已解析字段
        self.assertEqual(9, record.fields["intention"])
        self.assertEqual(49, record.fields["offset_x_count"])
        self.assertEqual(50, len(record.points))

    def test_invalid_protobuf_is_parse_fail(self):
        for raw in (b"\xff\xff\xff", b"\x0a\x04\x01"):
            with self.subTest(raw=raw.hex()):
                record = inspector.inspect_message(
                    envelope("RobotPathPlanInfo", raw)
                )
                self.assertEqual(inspector.STATUS_PARSE_FAIL, record.status)
                self.assertIn("Protobuf 解析失败", record.error)
                self.assertEqual({}, record.fields)


class InspectorOutputTest(unittest.TestCase):
    def test_console_lines_include_status_and_fields(self):
        stream = io.StringIO()
        target = inspector.Inspector(stream=stream)

        target.handle(envelope("GameStatus", b"\x01\x02"))
        target.close()

        text = stream.getvalue()
        self.assertIn("topic=GameStatus", text)
        self.assertIn("payload=2B", text)
        self.assertIn("UNSUPPORTED", text)

    @unittest.skipUnless(HAS_PROTOBUF, PROTOBUF_REASON)
    def test_dump_points_adds_points_line(self):
        stream = io.StringIO()
        target = inspector.Inspector(dump_points=True, stream=stream)

        target.handle(envelope("RobotPathPlanInfo", build_path_payload()))
        target.close()

        text = stream.getvalue()
        self.assertIn("points(50)=", text)

    def test_json_lines_keep_raw_payload_and_status(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "capture.jsonl"
            target = inspector.Inspector(
                output_path=output_path, stream=io.StringIO()
            )
            payload = b"\x53\x49\x4d"

            target.handle(envelope("GameStatus", payload))
            target.close()

            lines = output_path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(1, len(lines))
            record = json.loads(lines[0])
            self.assertEqual("GameStatus", record["topic"])
            self.assertEqual(3, record["payload_length"])
            self.assertEqual(
                base64.b64encode(payload).decode("ascii"), record["payload_b64"]
            )
            self.assertEqual(inspector.STATUS_UNSUPPORTED, record["status"])
            self.assertIn("timestamp", record)


@unittest.skipUnless(HAS_PROTOBUF, PROTOBUF_REASON)
class ParsedVersusValidationOutputTest(unittest.TestCase):
    """终端首行只讲「能否解析」，校验结论单独一行；JSONL 的 status 不变。"""

    def render(self, payload: bytes, **kwargs) -> str:
        stream = io.StringIO()
        target = inspector.Inspector(stream=stream, **kwargs)
        target.handle(envelope("RobotPathPlanInfo", payload))
        target.close()
        return stream.getvalue()

    def test_pass_shows_parsed_then_validation(self):
        lines = self.render(build_path_payload()).splitlines()

        self.assertTrue(lines[0].endswith("-> PARSED"), lines[0])
        self.assertIn("    validation=PASS", lines)
        # 首行不能再把 PASS 当成解析结论
        self.assertNotIn("-> PASS", lines[0])

    def test_not_match_shows_parsed_and_keeps_reasons(self):
        lines = self.render(build_path_payload(intention=4)).splitlines()

        self.assertTrue(lines[0].endswith("-> PARSED"), lines[0])
        self.assertIn("    validation=NOT_MATCH", lines)
        self.assertNotIn("-> NOT_MATCH", lines[0])
        # 原有校验原因说明必须保留
        self.assertTrue(any(line.startswith("    ! ") for line in lines))

    def test_parse_fail_is_not_labelled_parsed(self):
        text = self.render(b"\xff\xff\xff")

        self.assertIn("-> PARSE_FAIL", text)
        self.assertNotIn("PARSED", text)
        self.assertNotIn("validation=", text)

    def test_unsupported_is_not_labelled_parsed(self):
        stream = io.StringIO()
        target = inspector.Inspector(stream=stream)
        target.handle(envelope("GameStatus", b"\x01"))
        target.close()
        text = stream.getvalue()

        self.assertIn("-> UNSUPPORTED", text)
        self.assertNotIn("PARSED", text)

    def test_jsonl_status_stays_unrenamed(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_path = Path(temp_dir) / "capture.jsonl"
            target = inspector.Inspector(
                output_path=output_path, stream=io.StringIO()
            )
            target.handle(envelope("RobotPathPlanInfo", build_path_payload(intention=4)))
            target.close()

            record = json.loads(output_path.read_text(encoding="utf-8").splitlines()[0])
            # JSONL 结构不变：status 仍是校验结论，没有 PARSED
            self.assertEqual(inspector.STATUS_NOT_MATCH, record["status"])
            self.assertNotEqual(inspector.LABEL_PARSED, record["status"])
            self.assertIn("issues", record)


class InspectorFlowTest(unittest.TestCase):
    def make_args(self, *extra: str):
        parser = inspector.build_parser()
        return parser.parse_args(
            [
                "--broker", "127.0.0.1",
                "--port", "3333",
                "--client-id", "inspector-test",
                *extra,
            ]
        )

    def test_no_message_returns_nonzero(self):
        clock = FakeClock()
        client = FakeClient(clock=clock)
        args = self.make_args("--topic", "RobotPathPlanInfo", "--timeout", "1")

        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = inspector.run_inspector(
                args,
                client_factory=lambda client_id: client,
                clock=clock,
            )

        self.assertEqual(inspector.EXIT_NO_MESSAGE, code)
        self.assertNotEqual(0, code)
        self.assertEqual(["RobotPathPlanInfo"], client.subscribed)

    def test_max_messages_stops_early(self):
        clock = FakeClock()
        client = FakeClient(
            clock=clock,
            script=[
                FakeMessage("GameStatus", b"\x01"),
                FakeMessage("GameStatus", b"\x02"),
            ],
        )
        args = self.make_args(
            "--topic", "#", "--timeout", "10", "--max-messages", "1"
        )

        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            code = inspector.run_inspector(
                args, client_factory=lambda client_id: client, clock=clock
            )

        self.assertEqual(inspector.EXIT_OK, code)
        self.assertLess(clock.now, 10.0, "达到 --max-messages 后应提前结束")

    @unittest.skipUnless(HAS_PROTOBUF, PROTOBUF_REASON)
    def test_end_to_end_path_message_passes(self):
        clock = FakeClock()
        client = FakeClient(
            clock=clock,
            script=[FakeMessage("RobotPathPlanInfo", build_path_payload())],
        )
        args = self.make_args(
            "--topic", "RobotPathPlanInfo",
            "--timeout", "10",
            "--max-messages", "1",
            "--expected-sender-id", "7",
        )
        stream = io.StringIO()

        with redirect_stdout(stream), redirect_stderr(io.StringIO()):
            code = inspector.run_inspector(
                args, client_factory=lambda client_id: client, clock=clock
            )

        self.assertEqual(inspector.EXIT_OK, code)
        text = stream.getvalue()
        self.assertIn("RobotPathPlanInfo", text)
        self.assertIn("-> PARSED", text)
        self.assertIn("validation=", text)


if __name__ == "__main__":
    unittest.main()
