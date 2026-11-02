"""ROS 2 接收层：只负责节点、QoS、订阅与接收统计。"""

from __future__ import annotations

import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Callable

DEFAULT_TOPIC = "/robot_map_positions"
DEFAULT_NODE_NAME = "ros_radar_inspector"
QOS_DEPTH = 10


class ReceiverError(RuntimeError):
    """ROS 2 环境或消息包不可用。"""


@dataclass(frozen=True)
class MessageEnvelope:
    message: object
    received_at: datetime
    monotonic_at: float


@dataclass(frozen=True)
class ReceiveResult:
    received: int
    elapsed: float
    stopped_by_limit: bool

    @property
    def frequency_hz(self) -> float:
        return self.received / self.elapsed if self.elapsed > 0 else 0.0


def load_ros_dependencies():
    """延迟加载 ROS 与消息类型，让离线解析不依赖 ROS 安装。"""

    try:
        import rclpy
        from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
    except ImportError as error:
        raise ReceiverError(
            "未找到 ROS 2 Python 环境（rclpy）；请先 source ROS 2 Humble 环境"
        ) from error

    try:
        from vision_interface.msg import RobotMapPositions
    except ImportError as error:
        raise ReceiverError(
            "未找到 vision_interface/msg/RobotMapPositions；请安装并 source 雷达消息包"
        ) from error

    return rclpy, QoSProfile, ReliabilityPolicy, HistoryPolicy, RobotMapPositions


class RadarReceiver:
    """订阅一条雷达话题；不解释消息字段，也不发布任何数据。"""

    def __init__(
        self,
        *,
        topic: str = DEFAULT_TOPIC,
        node_name: str = DEFAULT_NODE_NAME,
        clock: Callable[[], float] = time.monotonic,
        wall_clock: Callable[[], datetime] = lambda: datetime.now(timezone.utc),
    ) -> None:
        if not topic.startswith("/"):
            raise ReceiverError("ROS topic 必须是绝对名称（以 / 开头）")
        self.topic = topic
        self.node_name = node_name
        self._clock = clock
        self._wall_clock = wall_clock

    def run(
        self,
        *,
        handler: Callable[[MessageEnvelope], None],
        timeout: float | None = None,
        max_messages: int | None = None,
    ) -> ReceiveResult:
        if timeout is not None and timeout <= 0:
            raise ReceiverError("timeout 必须大于 0")
        if max_messages is not None and max_messages <= 0:
            raise ReceiverError("max_messages 必须大于 0")

        rclpy, QoSProfile, ReliabilityPolicy, HistoryPolicy, message_type = (
            load_ros_dependencies()
        )
        qos = QoSProfile(
            reliability=ReliabilityPolicy.RELIABLE,
            history=HistoryPolicy.KEEP_LAST,
            depth=QOS_DEPTH,
        )

        started = self._clock()
        received = 0
        stopped_by_limit = False
        initialized_here = not rclpy.ok()
        if initialized_here:
            rclpy.init(args=None)
        node = rclpy.create_node(self.node_name)

        def callback(message: object) -> None:
            nonlocal received
            received += 1
            handler(
                MessageEnvelope(
                    message=message,
                    received_at=self._wall_clock(),
                    monotonic_at=self._clock(),
                )
            )

        subscription = node.create_subscription(message_type, self.topic, callback, qos)
        del subscription  # node 持有订阅；保留语义，不在接收层解析。
        try:
            while rclpy.ok():
                elapsed = self._clock() - started
                if timeout is not None and elapsed >= timeout:
                    break
                if max_messages is not None and received >= max_messages:
                    stopped_by_limit = True
                    break
                remaining = None if timeout is None else max(0.0, timeout - elapsed)
                spin_timeout = 0.1 if remaining is None else min(0.1, remaining)
                rclpy.spin_once(node, timeout_sec=spin_timeout)
        finally:
            node.destroy_node()
            if initialized_here and rclpy.ok():
                rclpy.shutdown()

        return ReceiveResult(
            received=received,
            elapsed=self._clock() - started,
            stopped_by_limit=stopped_by_limit,
        )
