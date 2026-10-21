"""MQTT 接收层：只负责传输。

职责边界（严格）：

- 只做：连接 Broker、用独立 client-id 订阅一个或多个 Topic、接收消息、
  超时与停止、统计消息数量、把消息包装成 :class:`MessageEnvelope` 交给上层。
- 不做：解析 RobotPathPlanInfo、解析 CustomByteBlock 内部字段、判断 intention、
  还原路径、引用 GameData / QML / 客户端 Store、发布任何 MQTT 消息。

本模块不导入任何具体业务协议，可被 ``custom_byte_validator`` 与 ``inspector`` 复用。
通过注入 ``client_factory`` 与 ``clock``，可以在不启动真实 Broker 的情况下做单元测试。
"""

from __future__ import annotations

import socket
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Callable, Sequence

DEFAULT_LOOP_TIMEOUT = 0.1
DEFAULT_KEEPALIVE = 30


class ReceiverError(RuntimeError):
    """传输层自身的环境或配置问题（例如缺少 paho-mqtt）。"""


@dataclass(frozen=True)
class MessageEnvelope:
    """一条已接收的 MQTT 消息，不含任何协议解释。"""

    topic: str
    payload: bytes
    received_at: datetime
    qos: int = 0
    retain: bool = False


@dataclass
class ReceiveResult:
    """一次接收会话的结果，供上层决定日志与退出码。"""

    connected: bool
    connect_rc: int | None
    received: int
    timed_out: bool
    elapsed: float
    connection_error: str | None = None


def wait_for_port(
    broker: str,
    port: int,
    *,
    deadline: float,
    interval: float = 0.5,
    clock: Callable[[], float] = time.monotonic,
) -> bool:
    """在 deadline 之前等待端口可达，避免为不可达的 broker 白等。"""

    while True:
        remaining = deadline - clock()
        if remaining <= 0:
            return False
        try:
            with socket.create_connection((broker, port), timeout=min(2.0, remaining)):
                return True
        except OSError:
            remaining = deadline - clock()
            if remaining <= 0:
                return False
            time.sleep(min(interval, remaining))


def default_client_factory(client_id: str):
    """延迟导入 paho-mqtt 并创建客户端；未安装时给出明确提示。"""

    try:
        import paho.mqtt.client as mqtt
    except ImportError as error:
        raise ReceiverError(
            "未安装 paho-mqtt，无法连接 broker；请运行： pip install 'paho-mqtt>=2.0'"
        ) from error

    return mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=client_id,
        protocol=mqtt.MQTTv311,
    )


class MessageReceiver:
    """把 MQTT 传输细节收敛在一处；上层只实现 handler。"""

    def __init__(
        self,
        *,
        broker: str,
        port: int,
        client_id: str,
        topics: Sequence[str],
        client_factory: Callable[[str], object] = default_client_factory,
        clock: Callable[[], float] = time.monotonic,
        loop_timeout: float = DEFAULT_LOOP_TIMEOUT,
        keepalive: int = DEFAULT_KEEPALIVE,
    ) -> None:
        if not topics:
            raise ReceiverError("至少需要订阅一个 Topic")
        self._broker = broker
        self._port = port
        self._client_id = client_id
        self._topics = tuple(topics)
        self._client_factory = client_factory
        self._clock = clock
        self._loop_timeout = loop_timeout
        self._keepalive = keepalive

    @property
    def topics(self) -> tuple[str, ...]:
        return self._topics

    def run(
        self,
        *,
        timeout: float,
        handler: Callable[[MessageEnvelope], None],
        should_stop: Callable[[], bool] | None = None,
        on_connected: Callable[[Sequence[str]], None] | None = None,
        deadline: float | None = None,
    ) -> ReceiveResult:
        """连接并驱动事件循环，直到超时或 ``should_stop()`` 为真。

        只订阅，不发布。连接失败不抛异常，而是放在返回值里由上层决定退出码。
        """

        started = self._clock()
        if deadline is None:
            deadline = started + timeout

        state: dict[str, object] = {"connected": False, "rc": None, "received": 0}
        client = self._client_factory(self._client_id)

        def on_connect(active_client, userdata, flags, rc, properties=None):
            rc_value = getattr(rc, "value", rc)
            state["rc"] = rc_value
            if rc_value == 0:
                state["connected"] = True
                for topic in self._topics:
                    active_client.subscribe(topic)
                if on_connected is not None:
                    on_connected(self._topics)

        def on_message(active_client, userdata, message):
            state["received"] = int(state["received"]) + 1
            envelope = MessageEnvelope(
                topic=message.topic,
                payload=bytes(message.payload),
                received_at=datetime.now(),
                qos=int(getattr(message, "qos", 0)),
                retain=bool(getattr(message, "retain", False)),
            )
            handler(envelope)

        client.on_connect = on_connect
        client.on_message = on_message

        try:
            client.connect(self._broker, self._port, keepalive=self._keepalive)
        except Exception as error:  # noqa: BLE001 - 传输层把异常转成结果
            self._disconnect(client)
            return ReceiveResult(
                connected=False,
                connect_rc=None,
                received=int(state["received"]),
                timed_out=False,
                elapsed=self._clock() - started,
                connection_error=str(error),
            )

        while self._clock() < deadline and not state["connected"]:
            client.loop(timeout=self._loop_timeout)

        if not state["connected"]:
            self._disconnect(client)
            return ReceiveResult(
                connected=False,
                connect_rc=state["rc"],
                received=int(state["received"]),
                timed_out=True,
                elapsed=self._clock() - started,
            )

        while self._clock() < deadline:
            if should_stop is not None and should_stop():
                break
            client.loop(timeout=self._loop_timeout)

        self._disconnect(client)
        stopped_early = bool(should_stop is not None and should_stop())
        return ReceiveResult(
            connected=True,
            connect_rc=state["rc"],
            received=int(state["received"]),
            timed_out=not stopped_early,
            elapsed=self._clock() - started,
        )

    @staticmethod
    def _disconnect(client) -> None:
        disconnect = getattr(client, "disconnect", None)
        if callable(disconnect):
            disconnect()
