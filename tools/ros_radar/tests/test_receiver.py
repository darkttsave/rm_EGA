from __future__ import annotations

import unittest
from datetime import datetime, timezone
from unittest import mock

from tools.ros_radar import receiver


class ReceiverTests(unittest.TestCase):
    def test_rejects_relative_topic(self):
        with self.assertRaises(receiver.ReceiverError):
            receiver.RadarReceiver(topic="robot_map_positions")

    def test_missing_ros_environment_has_clear_error(self):
        with mock.patch.dict("sys.modules", {"rclpy": None}):
            with self.assertRaisesRegex(receiver.ReceiverError, "rclpy"):
                receiver.load_ros_dependencies()

    def test_uses_required_qos_and_delivers_message(self):
        class ReliabilityPolicy:
            RELIABLE = "reliable"

        class HistoryPolicy:
            KEEP_LAST = "keep_last"

        class QoSProfile:
            created = None

            def __init__(self, **values):
                self.values = values
                QoSProfile.created = self

        class FakeNode:
            callback = None
            subscription_args = None
            destroyed = False

            def create_subscription(self, *args):
                self.subscription_args = args
                self.callback = args[2]
                return object()

            def destroy_node(self):
                self.destroyed = True

        node = FakeNode()

        class FakeRclpy:
            initialized = False
            shutdown_called = False

            @classmethod
            def ok(cls):
                return cls.initialized and not cls.shutdown_called

            @classmethod
            def init(cls, args=None):
                cls.initialized = True

            @staticmethod
            def create_node(name):
                self.assertEqual(name, receiver.DEFAULT_NODE_NAME)
                return node

            @staticmethod
            def spin_once(active_node, timeout_sec):
                self.assertIs(active_node, node)
                active_node.callback("message")

            @classmethod
            def shutdown(cls):
                cls.shutdown_called = True

        times = iter([0.0, 0.0, 0.25, 0.5, 1.0])
        envelopes = []
        fixed_time = datetime(2026, 11, 2, tzinfo=timezone.utc)
        dependencies = (
            FakeRclpy,
            QoSProfile,
            ReliabilityPolicy,
            HistoryPolicy,
            "RobotMapPositions",
        )

        with mock.patch.object(receiver, "load_ros_dependencies", return_value=dependencies):
            result = receiver.RadarReceiver(
                clock=lambda: next(times), wall_clock=lambda: fixed_time
            ).run(handler=envelopes.append, timeout=2.0, max_messages=1)

        self.assertEqual(node.subscription_args[0], "RobotMapPositions")
        self.assertEqual(node.subscription_args[1], receiver.DEFAULT_TOPIC)
        self.assertEqual(
            QoSProfile.created.values,
            {"reliability": "reliable", "history": "keep_last", "depth": 10},
        )
        self.assertEqual(envelopes[0].message, "message")
        self.assertEqual(envelopes[0].received_at, fixed_time)
        self.assertEqual(result.received, 1)
        self.assertTrue(result.stopped_by_limit)
        self.assertTrue(node.destroyed)
        self.assertTrue(FakeRclpy.shutdown_called)


if __name__ == "__main__":
    unittest.main()
