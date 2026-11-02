from __future__ import annotations

import io
import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from tools.ros_radar.inspector import RadarInspector
from tools.ros_radar.receiver import MessageEnvelope
from tools.ros_radar.tests.helpers import make_message, make_robot


def envelope(message, monotonic_at: float) -> MessageEnvelope:
    return MessageEnvelope(
        message=message,
        received_at=datetime(2026, 11, 2, 8, 0, tzinfo=timezone.utc),
        monotonic_at=monotonic_at,
    )


class InspectorTests(unittest.TestCase):
    def test_full_snapshot_reports_target_disappearance_and_frequency(self):
        stream = io.StringIO()
        inspector = RadarInspector(stream=stream)
        inspector.handle(
            envelope(
                make_message(
                    robots=[make_robot(color=0, robot_id=1), make_robot(color=2, robot_id=2)]
                ),
                10.0,
            )
        )
        inspector.handle(
            envelope(make_message(robots=[make_robot(color=2, robot_id=2)]), 10.5)
        )

        output = stream.getvalue()
        self.assertEqual(output.count("已收到首帧消息，开始解析"), 1)
        self.assertIn("disappeared=[(0, 1)]", output)
        self.assertIn("rate=2.00Hz", output)
        self.assertEqual(inspector.stats.valid, 2)

    def test_empty_snapshot_clears_all_previous_targets(self):
        stream = io.StringIO()
        inspector = RadarInspector(stream=stream)
        inspector.handle(envelope(make_message(robots=[make_robot()]), 1.0))
        inspector.handle(envelope(make_message(robots=[]), 2.0))
        self.assertIn("disappeared=[(0, 1)]", stream.getvalue())

    def test_invalid_message_is_counted_and_does_not_replace_snapshot(self):
        stream = io.StringIO()
        inspector = RadarInspector(stream=stream)
        inspector.handle(envelope(make_message(robots=[make_robot()]), 1.0))
        inspector.handle(envelope(make_message(robots=[make_robot(color=1)]), 2.0))
        inspector.handle(envelope(make_message(robots=[]), 3.0))
        self.assertEqual(inspector.stats.invalid, 1)
        self.assertIn("disappeared=[(0, 1)]", stream.getvalue())

    def test_jsonl_records_raw_and_parsed_data(self):
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "capture.jsonl"
            inspector = RadarInspector(output_path=output_path, stream=io.StringIO())
            inspector.handle(envelope(make_message(robots=[make_robot()]), 1.0))
            inspector.close()

            record = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(record["status"], "VALID")
            self.assertIn("raw", record)
            self.assertIn("parsed", record)
            self.assertEqual(record["parsed"]["robots"][0]["id"], 1)


if __name__ == "__main__":
    unittest.main()
