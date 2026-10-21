"""接收层 ``receiver`` 的离线单元测试。

不依赖真实 Broker：用假客户端与假时钟驱动事件循环。
"""

from __future__ import annotations

import sys
import time
import unittest
from datetime import datetime
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[3]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.mqtt_protocol import receiver  # noqa: E402


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now

    def advance(self, delta: float) -> None:
        self.now += delta


class FakeMessage:
    def __init__(self, topic: str, payload: bytes, *, qos: int = 0, retain: bool = False):
        self.topic = topic
        self.payload = payload
        self.qos = qos
        self.retain = retain


class FakeClient:
    """模拟 paho 客户端：只在本进程内投递消息，绝不联网。"""

    def __init__(
        self,
        *,
        clock: FakeClock,
        script: list[FakeMessage] | None = None,
        loop_step: float = 0.1,
        connect_rc: int = 0,
        connect_error: Exception | None = None,
    ) -> None:
        self.clock = clock
        self.script = list(script or [])
        self.loop_step = loop_step
        self.connect_rc = connect_rc
        self.connect_error = connect_error
        self.on_connect = None
        self.on_message = None
        self.subscribed: list[str] = []
        self.published: list[tuple] = []
        self.connected = False
        self.disconnected = False
        self._announced = False

    def subscribe(self, topic, *args, **kwargs):
        self.subscribed.append(topic)

    def publish(self, *args, **kwargs):
        self.published.append((args, kwargs))
        raise AssertionError("接收层不得发布消息")

    def connect(self, host, port, keepalive=60):
        if self.connect_error is not None:
            raise self.connect_error
        return 0

    def disconnect(self):
        self.disconnected = True

    def loop(self, timeout=1.0):
        if not self._announced:
            self._announced = True
            if self.on_connect is not None:
                self.on_connect(self, None, {}, self.connect_rc)
            self.clock.advance(self.loop_step)
            return 0
        self.clock.advance(self.loop_step)
        if self.script and self.on_message is not None:
            self.on_message(self, None, self.script.pop(0))
        return 0


def make_receiver(clock: FakeClock, client: FakeClient, topics=("CustomByteBlock",)):
    return receiver.MessageReceiver(
        broker="127.0.0.1",
        port=3333,
        client_id="unit-test",
        topics=list(topics),
        client_factory=lambda client_id: client,
        clock=clock,
        loop_timeout=0.1,
    )


class MessageEnvelopeTest(unittest.TestCase):
    def test_envelope_fields(self):
        moment = datetime(2026, 1, 2, 3, 4, 5, 123000)
        envelope = receiver.MessageEnvelope(
            topic="RobotPathPlanInfo",
            payload=b"\x01\x02",
            received_at=moment,
            qos=1,
            retain=True,
        )

        self.assertEqual("RobotPathPlanInfo", envelope.topic)
        self.assertEqual(b"\x01\x02", envelope.payload)
        self.assertEqual(moment, envelope.received_at)
        self.assertEqual(1, envelope.qos)
        self.assertTrue(envelope.retain)

    def test_envelope_defaults(self):
        envelope = receiver.MessageEnvelope(
            topic="t", payload=b"", received_at=datetime.now()
        )

        self.assertEqual(0, envelope.qos)
        self.assertFalse(envelope.retain)


class MessageReceiverTest(unittest.TestCase):
    def test_subscribes_every_topic(self):
        clock = FakeClock()
        client = FakeClient(clock=clock)
        target = make_receiver(clock, client, topics=("A", "B"))

        target.run(timeout=0.2, handler=lambda envelope: None)

        self.assertEqual(["A", "B"], client.subscribed)

    def test_message_callback_receives_envelope(self):
        clock = FakeClock()
        client = FakeClient(
            clock=clock,
            script=[FakeMessage("RobotPathPlanInfo", b"\xaa\xbb", qos=1, retain=True)],
        )
        target = make_receiver(clock, client, topics=("RobotPathPlanInfo",))
        received: list[receiver.MessageEnvelope] = []

        result = target.run(
            timeout=1.0,
            handler=received.append,
            should_stop=lambda: bool(received),
        )

        self.assertEqual(1, result.received)
        self.assertEqual(1, len(received))
        self.assertEqual("RobotPathPlanInfo", received[0].topic)
        self.assertEqual(b"\xaa\xbb", received[0].payload)
        self.assertEqual(1, received[0].qos)
        self.assertTrue(received[0].retain)
        self.assertTrue(result.connected)
        self.assertFalse(result.timed_out)
        self.assertTrue(client.disconnected)

    def test_received_counter_and_early_stop(self):
        clock = FakeClock()
        script = [FakeMessage("t", b"x") for _ in range(50)]
        client = FakeClient(clock=clock, script=script)
        target = make_receiver(clock, client, topics=("t",))
        seen: list[bytes] = []

        result = target.run(
            timeout=10.0,
            handler=lambda envelope: seen.append(envelope.payload),
            should_stop=lambda: len(seen) >= 2,
        )

        self.assertEqual(2, result.received)
        self.assertEqual(2, len(seen))
        self.assertFalse(result.timed_out, "提前停止不算超时")
        self.assertLess(clock.now, 10.0)

    def test_timeout_without_messages(self):
        clock = FakeClock()
        client = FakeClient(clock=clock)
        target = make_receiver(clock, client, topics=("t",))

        result = target.run(timeout=1.0, handler=lambda envelope: None)

        self.assertTrue(result.connected)
        self.assertEqual(0, result.received)
        self.assertTrue(result.timed_out)
        self.assertGreaterEqual(result.elapsed, 1.0)
        self.assertTrue(client.disconnected)

    def test_connect_error_is_returned_not_raised(self):
        clock = FakeClock()
        client = FakeClient(clock=clock, connect_error=OSError("connection refused"))
        target = make_receiver(clock, client, topics=("t",))

        result = target.run(timeout=1.0, handler=lambda envelope: None)

        self.assertFalse(result.connected)
        self.assertIn("connection refused", result.connection_error)
        self.assertEqual(0, result.received)

    def test_handshake_timeout_without_connack(self):
        clock = FakeClock()
        client = FakeClient(clock=clock, connect_rc=5)  # 非 0 视为拒绝
        target = make_receiver(clock, client, topics=("t",))

        result = target.run(timeout=1.0, handler=lambda envelope: None)

        self.assertFalse(result.connected)
        self.assertTrue(result.timed_out)
        self.assertEqual(5, result.connect_rc)
        self.assertTrue(client.disconnected)

    def test_on_connected_receives_topics(self):
        clock = FakeClock()
        client = FakeClient(clock=clock)
        target = make_receiver(clock, client, topics=("A", "B"))
        connected: list[tuple[str, ...]] = []

        target.run(
            timeout=0.2,
            handler=lambda envelope: None,
            on_connected=lambda topics: connected.append(tuple(topics)),
        )

        self.assertEqual([("A", "B")], connected)

    def test_requires_at_least_one_topic(self):
        with self.assertRaises(receiver.ReceiverError):
            receiver.MessageReceiver(
                broker="127.0.0.1", port=3333, client_id="x", topics=[]
            )

    def test_never_publishes(self):
        clock = FakeClock()
        client = FakeClient(clock=clock, script=[FakeMessage("t", b"x")])
        target = make_receiver(clock, client, topics=("t",))

        target.run(timeout=1.0, handler=lambda envelope: None)

        self.assertEqual([], client.published)


class PortProbeTest(unittest.TestCase):
    """端口预检受 deadline 约束，且只依赖本机回环。"""

    def test_open_port_is_detected(self):
        import socket

        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        port = server.getsockname()[1]
        try:
            self.assertTrue(
                receiver.wait_for_port(
                    "127.0.0.1", port, deadline=time.monotonic() + 5.0
                )
            )
        finally:
            server.close()

    def test_expired_deadline_gives_up_immediately(self):
        started = time.monotonic()

        self.assertFalse(
            receiver.wait_for_port("127.0.0.1", 1, deadline=time.monotonic() - 1.0)
        )
        self.assertLess(time.monotonic() - started, 1.0)


if __name__ == "__main__":
    unittest.main()
