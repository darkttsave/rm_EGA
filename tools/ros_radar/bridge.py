"""ROS 雷达完整快照到本地 MQTT 的独立 JSON 桥接器。"""

from __future__ import annotations

import argparse
import json
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from typing import Callable, Protocol, TextIO

from . import parser as radar_parser
from . import receiver

SCHEMA = "rm26.ros_radar_positions"
VERSION = 1
DEFAULT_MQTT_TOPIC = "RosRadarPositions"
DEFAULT_CLIENT_ID = "rm26-ros-radar-bridge"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_INTERRUPTED = 130


def print_status(message: str, *, stream: TextIO) -> None:
    print(f"[ROS Bridge] {message}", file=stream, flush=True)


def frame_payload(
    frame: radar_parser.RadarFrame,
    *,
    received_at: datetime,
    source_topic: str = receiver.DEFAULT_TOPIC,
) -> dict[str, object]:
    """生成稳定的 v1 JSON；robots 始终代表本帧完整快照。"""

    return {
        "schema": SCHEMA,
        "version": VERSION,
        "source_type": receiver.MESSAGE_TYPE_NAME,
        "source_topic": source_topic,
        "received_at": received_at.isoformat(timespec="milliseconds"),
        **frame.to_dict(),
    }


class Publisher(Protocol):
    def connect(self) -> None: ...

    def publish(self, topic: str, payload: bytes, *, qos: int) -> None: ...

    def close(self) -> None: ...


class MqttPublisher:
    """小型 Paho 发布封装；模块导入本身不要求安装 paho-mqtt。"""

    def __init__(
        self,
        *,
        broker: str,
        port: int,
        client_id: str,
        connect_timeout: float = 5.0,
    ) -> None:
        try:
            import paho.mqtt.client as mqtt
        except ImportError as error:
            raise RuntimeError(
                "未安装 paho-mqtt；请在当前 Python 环境运行 "
                "pip install 'paho-mqtt>=2.0'"
            ) from error

        self._mqtt = mqtt
        self._broker = broker
        self._port = port
        self._connect_timeout = connect_timeout
        self._connected = threading.Event()
        self._connect_error: str | None = None
        self._client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=client_id,
            protocol=mqtt.MQTTv311,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect

    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        del client, userdata, flags, properties
        if reason_code == 0:
            self._connected.set()
        else:
            self._connect_error = str(reason_code)
            self._connected.set()

    def _on_disconnect(
        self, client, userdata, disconnect_flags, reason_code, properties
    ) -> None:
        del client, userdata, disconnect_flags, properties
        if reason_code != 0:
            self._connect_error = f"连接中断：{reason_code}"
        self._connected.clear()

    def connect(self) -> None:
        result = self._client.connect(self._broker, self._port, keepalive=20)
        if result != self._mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"MQTT connect 返回错误码 {result}")
        self._client.loop_start()
        if not self._connected.wait(self._connect_timeout):
            self.close()
            raise RuntimeError("等待 MQTT Broker 响应超时")
        if self._connect_error is not None:
            error = self._connect_error
            self.close()
            raise RuntimeError(f"MQTT 连接失败：{error}")

    def publish(self, topic: str, payload: bytes, *, qos: int) -> None:
        if not self._connected.is_set():
            raise RuntimeError(self._connect_error or "MQTT 未连接")
        info = self._client.publish(topic, payload, qos=qos, retain=False)
        if info.rc != self._mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"MQTT publish 返回错误码 {info.rc}")
        info.wait_for_publish(timeout=2.0)
        if not info.is_published():
            raise RuntimeError("MQTT 发布确认超时")

    def close(self) -> None:
        try:
            self._client.disconnect()
        finally:
            self._client.loop_stop()
            self._connected.clear()


@dataclass
class BridgeStats:
    received: int = 0
    parsed: int = 0
    published: int = 0
    failed: int = 0


class RadarMqttBridge:
    def __init__(
        self,
        *,
        publisher: Publisher,
        mqtt_topic: str = DEFAULT_MQTT_TOPIC,
        source_topic: str = receiver.DEFAULT_TOPIC,
        qos: int = 1,
        stream: TextIO = sys.stdout,
        error_stream: TextIO = sys.stderr,
    ) -> None:
        self.publisher = publisher
        self.mqtt_topic = mqtt_topic
        self.source_topic = source_topic
        self.qos = qos
        self.stream = stream
        self.error_stream = error_stream
        self.stats = BridgeStats()
        self._first_published = False

    def handle(self, envelope: receiver.MessageEnvelope) -> None:
        self.stats.received += 1
        try:
            frame = radar_parser.parse_message(envelope.message)
            self.stats.parsed += 1
            payload = json.dumps(
                frame_payload(
                    frame,
                    received_at=envelope.received_at,
                    source_topic=self.source_topic,
                ),
                ensure_ascii=False,
                separators=(",", ":"),
            ).encode("utf-8")
            self.publisher.publish(self.mqtt_topic, payload, qos=self.qos)
        except (radar_parser.RadarParseError, RuntimeError) as error:
            self.stats.failed += 1
            print_status(f"本帧未发布：{error}", stream=self.error_stream)
            return

        self.stats.published += 1
        if not self._first_published:
            self._first_published = True
            print_status("首帧已解析并发布到 MQTT", stream=self.stream)


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ROS 雷达位置到本地 MQTT JSON 桥接")
    parser.add_argument("--ros-topic", default=receiver.DEFAULT_TOPIC)
    parser.add_argument("--node-name", default="ros_radar_mqtt_bridge")
    parser.add_argument("--broker", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--mqtt-topic", default=DEFAULT_MQTT_TOPIC)
    parser.add_argument("--client-id", default=DEFAULT_CLIENT_ID)
    parser.add_argument("--qos", type=int, choices=(0, 1, 2), default=1)
    parser.add_argument("--timeout", type=float, default=None)
    parser.add_argument("--max-messages", type=int, default=None)
    return parser


def run_bridge(
    args: argparse.Namespace,
    *,
    publisher: Publisher | None = None,
    radar_receiver: receiver.RadarReceiver | None = None,
    clock: Callable[[], float] = time.monotonic,
    stdout: TextIO = sys.stdout,
    stderr: TextIO = sys.stderr,
) -> int:
    started = clock()
    bridge: RadarMqttBridge | None = None
    subscription_started = False
    exit_code = EXIT_OK

    try:
        publisher = publisher or MqttPublisher(
            broker=args.broker,
            port=args.port,
            client_id=args.client_id,
        )
        publisher.connect()
        print_status(
            f"MQTT 已连接：{args.broker}:{args.port}，Topic={args.mqtt_topic}",
            stream=stdout,
        )
        bridge = RadarMqttBridge(
            publisher=publisher,
            mqtt_topic=args.mqtt_topic,
            source_topic=args.ros_topic,
            qos=args.qos,
            stream=stdout,
            error_stream=stderr,
        )
        radar_receiver = radar_receiver or receiver.RadarReceiver(
            topic=args.ros_topic, node_name=args.node_name
        )

        def on_started() -> None:
            nonlocal subscription_started
            subscription_started = True
            print_status(
                f"ROS 2 订阅已创建：{args.ros_topic}（不代表已发现发布者）",
                stream=stdout,
            )
            print_status("等待雷达消息...", stream=stdout)

        result = radar_receiver.run(
            handler=bridge.handle,
            timeout=args.timeout,
            max_messages=args.max_messages,
            on_started=on_started,
        )
        if result.stopped_by_shutdown:
            print_status("用户请求停止", stream=stdout)
            exit_code = EXIT_INTERRUPTED
    except KeyboardInterrupt:
        print_status("用户请求停止", stream=stdout)
        exit_code = EXIT_INTERRUPTED
    except (RuntimeError, receiver.ReceiverError, OSError, ValueError) as error:
        print_status(f"启动或运行失败：{error}", stream=stderr)
        exit_code = EXIT_ERROR
    finally:
        if publisher is not None:
            try:
                publisher.close()
            except Exception as error:  # noqa: BLE001
                print_status(f"MQTT 清理失败：{error}", stream=stderr)
        if subscription_started:
            print_status("ROS 订阅已停止", stream=stdout)
        stats = bridge.stats if bridge is not None else BridgeStats()
        print_status(f"运行时长：{max(0.0, clock() - started):.2f} s", stream=stdout)
        print_status(f"接收：{stats.received}", stream=stdout)
        print_status(f"解析：{stats.parsed}", stream=stdout)
        print_status(f"发布：{stats.published}", stream=stdout)
        print_status(f"失败：{stats.failed}", stream=stdout)
        print_status("程序已退出", stream=stdout)

    return exit_code


def main(argv: list[str] | None = None) -> int:
    return run_bridge(build_argument_parser().parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
