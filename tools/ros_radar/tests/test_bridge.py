from __future__ import annotations

import argparse
import io
import json
import unittest
from datetime import datetime, timezone

from tools.ros_radar import bridge, receiver
from tools.ros_radar.tests.helpers import make_message, make_robot


class FakePublisher:
    def __init__(self) -> None:
        self.connected = False
        self.closed = False
        self.messages: list[tuple[str, bytes, int]] = []

    def connect(self) -> None:
        self.connected = True

    def publish(self, topic: str, payload: bytes, *, qos: int) -> None:
        if not self.connected:
            raise RuntimeError("not connected")
        self.messages.append((topic, payload, qos))

    def close(self) -> None:
        self.closed = True


class ScriptedReceiver:
    def __init__(self, messages: list[object], *, shutdown: bool = False) -> None:
        self.messages = messages
        self.shutdown = shutdown

    def run(self, *, handler, timeout, max_messages, on_started):
        del timeout
        on_started()
        delivered = 0
        for message in self.messages:
            if max_messages is not None and delivered >= max_messages:
                break
            delivered += 1
            handler(
                receiver.MessageEnvelope(
                    message=message,
                    received_at=datetime(2026, 11, 2, tzinfo=timezone.utc),
                    monotonic_at=float(delivered),
                )
            )
        return receiver.ReceiveResult(
            received=delivered,
            elapsed=1.0,
            stopped_by_limit=max_messages is not None and delivered >= max_messages,
            stopped_by_shutdown=self.shutdown,
        )


def arguments() -> argparse.Namespace:
    return argparse.Namespace(
        ros_topic="/robot_map_positions",
        node_name="test_bridge",
        broker="127.0.0.1",
        port=1883,
        mqtt_topic="RosRadarPositions",
        client_id="test",
        qos=1,
        timeout=1.0,
        max_messages=None,
    )


class BridgeTests(unittest.TestCase):
    def test_publishes_versioned_complete_snapshot(self) -> None:
        publisher = FakePublisher()
        worker = bridge.RadarMqttBridge(publisher=publisher)
        publisher.connect()
        message = make_message(
            robots=[make_robot(color=2, robot_id=4, world_x=1.5, world_y=-2.0)]
        )

        worker.handle(
            receiver.MessageEnvelope(
                message=message,
                received_at=datetime(2026, 11, 2, tzinfo=timezone.utc),
                monotonic_at=1.0,
            )
        )

        self.assertEqual(1, len(publisher.messages))
        topic, payload, qos = publisher.messages[0]
        self.assertEqual("RosRadarPositions", topic)
        self.assertEqual(1, qos)
        decoded = json.loads(payload)
        self.assertEqual(bridge.SCHEMA, decoded["schema"])
        self.assertEqual(1, decoded["version"])
        self.assertEqual("rm_frame", decoded["frame_id"])
        self.assertEqual(2, decoded["robots"][0]["color"])
        self.assertEqual(4, decoded["robots"][0]["id"])
        self.assertEqual(1.5, decoded["robots"][0]["world_x"])
        self.assertEqual(-2.0, decoded["robots"][0]["world_y"])

    def test_empty_snapshot_is_published(self) -> None:
        publisher = FakePublisher()
        worker = bridge.RadarMqttBridge(publisher=publisher)
        publisher.connect()
        worker.handle(
            receiver.MessageEnvelope(
                message=make_message(robots=[]),
                received_at=datetime.now(timezone.utc),
                monotonic_at=1.0,
            )
        )
        self.assertEqual([], json.loads(publisher.messages[0][1])["robots"])
        self.assertEqual(1, worker.stats.published)

    def test_invalid_frame_is_not_published(self) -> None:
        publisher = FakePublisher()
        worker = bridge.RadarMqttBridge(
            publisher=publisher, error_stream=io.StringIO()
        )
        publisher.connect()
        invalid = make_message(robots=[make_robot(color=1)])
        worker.handle(
            receiver.MessageEnvelope(
                message=invalid,
                received_at=datetime.now(timezone.utc),
                monotonic_at=1.0,
            )
        )
        self.assertEqual([], publisher.messages)
        self.assertEqual(1, worker.stats.failed)

    def test_run_bridge_logs_and_cleans_up(self) -> None:
        publisher = FakePublisher()
        stdout = io.StringIO()
        result = bridge.run_bridge(
            arguments(),
            publisher=publisher,
            radar_receiver=ScriptedReceiver([make_message(robots=[])]),
            clock=lambda: 5.0,
            stdout=stdout,
            stderr=io.StringIO(),
        )
        self.assertEqual(bridge.EXIT_OK, result)
        self.assertTrue(publisher.closed)
        output = stdout.getvalue()
        self.assertIn("MQTT 已连接", output)
        self.assertIn("不代表已发现发布者", output)
        self.assertIn("发布：1", output)


if __name__ == "__main__":
    unittest.main()
