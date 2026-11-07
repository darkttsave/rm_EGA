import contextlib
import io
import json
import unittest
from unittest.mock import patch
from urllib.error import URLError

from tools import mqtt_tx_monitor as monitor


class MonitorTests(unittest.TestCase):
    def test_snapshot_and_real_interval_rate(self):
        first = monitor.Snapshot(True, "tcp://127.0.0.1:1883", 10, 2, 4, 1)
        second = monitor.Snapshot(True, "tcp://127.0.0.1:1883", 15, 3, 6, 1)
        text = monitor.format_snapshot(second, first, 2.0)
        self.assertIn("控制器 +5 (2.50 Hz)", text)
        self.assertIn("前哨站 +2 (1.00 Hz)", text)
        self.assertIn("合计 3.50 Hz", text)
        self.assertIn("不代表官方 Broker 或机器人已收到", text)

    def test_disconnected_and_missing_data(self):
        snapshot = monitor.parse_snapshot({"network_state": {"custom_control": {
            "mqtt_connected": False, "broker": None,
            "controller": {"published": 0, "dropped": 1},
            "outpost": {"published": 0, "dropped": 0}}}})
        self.assertIn("MQTT: 未连接", monitor.format_snapshot(snapshot))
        self.assertIn("暂无", monitor.format_snapshot(snapshot))
        with self.assertRaises(ValueError):
            monitor.parse_snapshot({"network_state": {}})

    def test_counter_reset_not_negative_rate(self):
        previous = monitor.Snapshot(True, None, 10, 2, 5, 1)
        current = monitor.Snapshot(True, None, 1, 0, 0, 0)
        self.assertIn("计数器已重置", monitor.format_snapshot(current, previous, 1))

    def test_unavailable_devhooks(self):
        output = io.StringIO()
        with patch.object(monitor, "fetch_snapshot", side_effect=URLError("refused")), contextlib.redirect_stdout(output):
            self.assertEqual(monitor.main(["--once"]), 1)
        self.assertIn("DevHooks 无数据或请求失败", output.getvalue())

    def test_rejects_invalid_statistics(self):
        data = json.loads('{"network_state":{"custom_control":{"mqtt_connected":true,"broker":null,"controller":{"published":true,"dropped":0},"outpost":{"published":0,"dropped":0}}}}')
        with self.assertRaises(ValueError):
            monitor.parse_snapshot(data)

    def test_request_is_loopback_without_proxy(self):
        class Reply:
            def __enter__(self):
                return io.BytesIO(b'{"network_state":{"custom_control":{"mqtt_connected":false,"broker":null,"controller":{"published":0,"dropped":0},"outpost":{"published":0,"dropped":0}}}}')
            def __exit__(self, *_args):
                pass

        with patch("urllib.request.build_opener") as build:
            build.return_value.open.return_value = Reply()
            monitor.fetch_snapshot(17711, 0.5)
            build.return_value.open.assert_called_once_with("http://127.0.0.1:17711/state", timeout=0.5)
            self.assertEqual(build.call_args.args[0].proxies, {})


if __name__ == "__main__":
    unittest.main()
