from __future__ import annotations

import math
import unittest

from tools.ros_radar.parser import RadarParseError, parse_message
from tools.ros_radar.tests.helpers import make_message, make_robot


class ParserTests(unittest.TestCase):
    def test_preserves_timestamp_color_id_and_coordinate_precision(self):
        robot = make_robot(color=2, robot_id=17)

        frame = parse_message(make_message(robots=[robot]))

        self.assertEqual(frame.timestamp.sec, 123)
        self.assertEqual(frame.timestamp.nanosec, 456_789_012)
        self.assertEqual(frame.timestamp.nanoseconds, 123_456_789_012)
        self.assertEqual(frame.frame_id, "rm_frame")
        self.assertEqual(frame.robots[0].color, 2)
        self.assertEqual(frame.robots[0].id, 17)
        self.assertEqual(frame.robots[0].x, 100.25)
        self.assertEqual(frame.robots[0].world_x, 1.234567890123)
        self.assertEqual(frame.meters_per_pixel, 0.012345678901)

    def test_empty_robots_is_a_valid_complete_snapshot(self):
        frame = parse_message(make_message(robots=[]))
        self.assertEqual(frame.robots, ())

    def test_multiple_robots_keep_input_order(self):
        robots = [
            make_robot(color=0, robot_id=1),
            make_robot(color=2, robot_id=7, x=300.0, y=400.0),
        ]
        frame = parse_message(make_message(robots=robots))
        self.assertEqual([robot.key for robot in frame.robots], [(0, 1), (2, 7)])

    def test_out_of_bounds_coordinates_are_preserved(self):
        robot = make_robot(x=-12.5, y=1200.75, in_bounds=False)
        frame = parse_message(make_message(robots=[robot]))
        self.assertEqual(frame.robots[0].x, -12.5)
        self.assertEqual(frame.robots[0].y, 1200.75)
        self.assertFalse(frame.robots[0].in_bounds)

    def test_rejects_unknown_color_and_non_uint8_id(self):
        with self.assertRaises(RadarParseError) as caught:
            parse_message(make_message(robots=[make_robot(color=1, robot_id=256)]))
        self.assertIn("不是 BLUE(0) 或 RED(2)", str(caught.exception))
        self.assertIn("必须是 uint8", str(caught.exception))

    def test_rejects_non_finite_coordinates(self):
        with self.assertRaises(RadarParseError) as caught:
            parse_message(make_message(robots=[make_robot(world_x=math.nan)]))
        self.assertIn("必须是有限数", str(caught.exception))

    def test_rejects_bad_timestamp(self):
        with self.assertRaises(RadarParseError) as caught:
            parse_message(make_message(sec=-1, nanosec=1_000_000_000))
        self.assertIn("sec: 不得为负数", str(caught.exception))
        self.assertIn("nanosec: 必须在", str(caught.exception))

    def test_rejects_in_bounds_flag_inconsistent_with_pixels(self):
        with self.assertRaises(RadarParseError) as caught:
            parse_message(make_message(robots=[make_robot(x=-0.1, in_bounds=True)]))
        self.assertIn("in_bounds=true", str(caught.exception))

        with self.assertRaises(RadarParseError) as caught:
            parse_message(make_message(robots=[make_robot(in_bounds=False)]))
        self.assertIn("in_bounds=false", str(caught.exception))

    def test_rejects_duplicate_radar_identity_in_same_snapshot(self):
        with self.assertRaises(RadarParseError) as caught:
            parse_message(
                make_message(
                    robots=[make_robot(color=0, robot_id=3), make_robot(color=0, robot_id=3)]
                )
            )
        self.assertIn("重复的雷达身份", str(caught.exception))

    def test_rejects_wrong_field_types(self):
        message = make_message()
        message.image_width = True
        message.meters_per_pixel = "0.1"
        with self.assertRaises(RadarParseError) as caught:
            parse_message(message)
        self.assertIn("image_width: 应为整数", str(caught.exception))
        self.assertIn("meters_per_pixel: 应为浮点数", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
