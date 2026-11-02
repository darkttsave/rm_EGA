from __future__ import annotations

import io
import unittest
from datetime import datetime, timezone

from tools.ros_radar import receiver
from tools.ros_radar.inspector import (
    EXIT_ERROR,
    EXIT_INTERRUPTED,
    EXIT_NO_MESSAGE,
    build_argument_parser,
    run_inspector,
)
from tools.ros_radar.receiver import MessageEnvelope, ReceiveResult
from tools.ros_radar.tests.helpers import make_message, make_robot


class FlushTrackingStream(io.StringIO):
    def __init__(self):
        super().__init__()
        self.flush_count = 0

    def flush(self):
        self.flush_count += 1
        super().flush()


def make_args(*extra: str):
    return build_argument_parser().parse_args(list(extra))


class LifecycleTests(unittest.TestCase):
    def test_startup_without_messages_is_explicit_and_not_connected(self):
        class NoMessageReceiver:
            def run(self, *, handler, timeout, max_messages, on_started):
                on_started()
                return ReceiveResult(received=0, elapsed=1.0, stopped_by_limit=False)

        stdout = FlushTrackingStream()
        stderr = FlushTrackingStream()
        times = iter([10.0, 11.25])

        exit_code = run_inspector(
            make_args("--timeout", "1"),
            radar_receiver=NoMessageReceiver(),
            clock=lambda: next(times),
            stdout=stdout,
            stderr=stderr,
        )

        output = stdout.getvalue()
        self.assertEqual(exit_code, EXIT_NO_MESSAGE)
        self.assertIn("ROS 2 节点已启动：ros_radar_inspector", output)
        self.assertIn("Topic：/robot_map_positions", output)
        self.assertIn("消息类型：vision_interface/msg/RobotMapPositions", output)
        self.assertIn("QoS：Reliable、Keep Last、Depth 10", output)
        self.assertIn("订阅已创建（不代表已发现发布者）", output)
        self.assertIn("等待雷达消息...", output)
        self.assertNotIn("已收到首帧消息", output)
        self.assertNotIn("连接成功", output)
        self.assertIn("未收到雷达消息（尚未确认发布者）", output)
        self.assertIn("订阅已停止", output)
        self.assertIn("运行时长：1.25 s", output)
        self.assertGreater(stdout.flush_count, 0)
        self.assertEqual(stderr.getvalue(), "")

    def test_first_frame_and_ros_shutdown_print_final_statistics(self):
        class ShutdownReceiver:
            def run(self, *, handler, timeout, max_messages, on_started):
                on_started()
                handler(
                    MessageEnvelope(
                        message=make_message(robots=[make_robot()]),
                        received_at=datetime.now(timezone.utc),
                        monotonic_at=20.0,
                    )
                )
                handler(
                    MessageEnvelope(
                        message=make_message(robots=[make_robot(color=1)]),
                        received_at=datetime.now(timezone.utc),
                        monotonic_at=20.5,
                    )
                )
                return ReceiveResult(
                    received=2,
                    elapsed=2.5,
                    stopped_by_limit=False,
                    stopped_by_shutdown=True,
                )

        stdout = FlushTrackingStream()
        stderr = FlushTrackingStream()
        times = iter([20.0, 22.5])

        exit_code = run_inspector(
            make_args("--no-parsed"),
            radar_receiver=ShutdownReceiver(),
            clock=lambda: next(times),
            stdout=stdout,
            stderr=stderr,
        )

        output = stdout.getvalue()
        self.assertEqual(exit_code, EXIT_INTERRUPTED)
        self.assertEqual(output.count("已收到首帧消息，开始解析"), 1)
        expected_in_order = [
            "用户请求停止",
            "订阅已停止",
            "运行时长：2.50 s",
            "接收消息：2",
            "解析成功：1",
            "解析失败：1",
            "程序已退出",
        ]
        positions = [output.index(text) for text in expected_in_order]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(stderr.getvalue(), "")

    def test_startup_failure_has_clear_reason_without_false_subscription(self):
        class FailingReceiver:
            def run(self, *, handler, timeout, max_messages, on_started):
                raise receiver.ReceiverError("缺少测试消息包")

        stdout = FlushTrackingStream()
        stderr = FlushTrackingStream()
        times = iter([1.0, 1.1])

        exit_code = run_inspector(
            make_args(),
            radar_receiver=FailingReceiver(),
            clock=lambda: next(times),
            stdout=stdout,
            stderr=stderr,
        )

        self.assertEqual(exit_code, EXIT_ERROR)
        self.assertIn("启动失败：缺少测试消息包", stderr.getvalue())
        self.assertNotIn("订阅已停止", stdout.getvalue())
        self.assertIn("程序已退出", stdout.getvalue())

    def test_runtime_failure_cleans_up_and_reports_reason(self):
        class RuntimeFailingReceiver:
            def run(self, *, handler, timeout, max_messages, on_started):
                on_started()
                raise RuntimeError("模拟 DDS 异常")

        stdout = FlushTrackingStream()
        stderr = FlushTrackingStream()
        times = iter([1.0, 1.2])

        exit_code = run_inspector(
            make_args(),
            radar_receiver=RuntimeFailingReceiver(),
            clock=lambda: next(times),
            stdout=stdout,
            stderr=stderr,
        )

        self.assertEqual(exit_code, EXIT_ERROR)
        self.assertIn("运行异常：模拟 DDS 异常", stderr.getvalue())
        self.assertIn("订阅已停止", stdout.getvalue())
        self.assertIn("程序已退出", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
