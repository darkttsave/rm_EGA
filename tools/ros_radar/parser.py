"""``RobotMapPositions`` 的纯 Python 解析与校验层。

本模块不依赖 ROS 2 运行时。测试可以传入具有相同字段的简单对象，因此在尚未
取得 ``vision_interface`` 消息包时也能验证协议语义。解析只保留上游坐标和 ID，
不进行小地图坐标换算，也不把雷达 ID 映射成裁判系统 ID。
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass
from typing import Any

BLUE = 0
RED = 2
VALID_COLORS = frozenset({BLUE, RED})


class RadarParseError(ValueError):
    """消息字段缺失、类型错误或数值不合法。"""

    def __init__(self, issues: list[str]) -> None:
        self.issues = tuple(issues)
        super().__init__("; ".join(issues))


@dataclass(frozen=True)
class Timestamp:
    sec: int
    nanosec: int

    @property
    def nanoseconds(self) -> int:
        return self.sec * 1_000_000_000 + self.nanosec

    def to_dict(self) -> dict[str, int]:
        return asdict(self)


@dataclass(frozen=True)
class RobotPosition:
    color: int
    id: int
    x: float
    y: float
    world_x: float
    world_y: float
    in_bounds: bool

    @property
    def key(self) -> tuple[int, int]:
        """雷达快照内的身份键；不等同于裁判系统机器人 ID。"""

        return (self.color, self.id)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class RadarFrame:
    timestamp: Timestamp
    frame_id: str
    image_width: int
    image_height: int
    meters_per_pixel: float
    robots: tuple[RobotPosition, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "timestamp": self.timestamp.to_dict(),
            "frame_id": self.frame_id,
            "image_width": self.image_width,
            "image_height": self.image_height,
            "meters_per_pixel": self.meters_per_pixel,
            "robots": [robot.to_dict() for robot in self.robots],
        }


def _field(value: object, name: str, path: str, issues: list[str]) -> Any:
    if not hasattr(value, name):
        issues.append(f"{path}.{name}: 缺少字段")
        return None
    return getattr(value, name)


def _integer(value: object, path: str, issues: list[str]) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        issues.append(f"{path}: 应为整数，实际为 {type(value).__name__}")
        return None
    return value


def _float(value: object, path: str, issues: list[str]) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        issues.append(f"{path}: 应为浮点数，实际为 {type(value).__name__}")
        return None
    result = float(value)
    if not math.isfinite(result):
        issues.append(f"{path}: 必须是有限数，实际为 {result!r}")
        return None
    return result


def parse_message(message: object) -> RadarFrame:
    """解析并严格校验一帧 ``RobotMapPositions``。

    ``robots`` 被当作本帧完整快照；空数组是合法快照。任一字段异常时抛出包含所有
    已发现问题的 :class:`RadarParseError`，避免把部分坏数据伪装成有效帧。
    """

    issues: list[str] = []
    header = _field(message, "header", "message", issues)
    stamp = _field(header, "stamp", "header", issues) if header is not None else None
    sec_raw = _field(stamp, "sec", "header.stamp", issues) if stamp is not None else None
    nsec_raw = (
        _field(stamp, "nanosec", "header.stamp", issues) if stamp is not None else None
    )
    frame_id_raw = _field(header, "frame_id", "header", issues) if header is not None else None

    sec = _integer(sec_raw, "header.stamp.sec", issues) if sec_raw is not None else None
    nsec = (
        _integer(nsec_raw, "header.stamp.nanosec", issues)
        if nsec_raw is not None
        else None
    )
    if sec is not None and sec < 0:
        issues.append("header.stamp.sec: 不得为负数")
    if nsec is not None and not 0 <= nsec < 1_000_000_000:
        issues.append("header.stamp.nanosec: 必须在 [0, 1000000000) 内")
    if frame_id_raw is not None and not isinstance(frame_id_raw, str):
        issues.append("header.frame_id: 应为字符串")

    width_raw = _field(message, "image_width", "message", issues)
    height_raw = _field(message, "image_height", "message", issues)
    mpp_raw = _field(message, "meters_per_pixel", "message", issues)
    robots_raw = _field(message, "robots", "message", issues)

    width = _integer(width_raw, "image_width", issues) if width_raw is not None else None
    height = _integer(height_raw, "image_height", issues) if height_raw is not None else None
    mpp = _float(mpp_raw, "meters_per_pixel", issues) if mpp_raw is not None else None
    if width is not None and not 1 <= width <= 0xFFFFFFFF:
        issues.append("image_width: 必须是大于 0 的 uint32")
    if height is not None and not 1 <= height <= 0xFFFFFFFF:
        issues.append("image_height: 必须是大于 0 的 uint32")
    if mpp is not None and mpp <= 0:
        issues.append("meters_per_pixel: 必须大于 0")

    if robots_raw is None:
        robot_values: list[object] = []
    elif isinstance(robots_raw, (str, bytes, bytearray)):
        issues.append("robots: 应为机器人序列")
        robot_values = []
    else:
        try:
            robot_values = list(robots_raw)
        except TypeError:
            issues.append("robots: 应为机器人序列")
            robot_values = []

    robots: list[RobotPosition] = []
    seen_keys: set[tuple[int, int]] = set()
    for index, robot in enumerate(robot_values):
        prefix = f"robots[{index}]"
        color_raw = _field(robot, "color", prefix, issues)
        id_raw = _field(robot, "id", prefix, issues)
        x_raw = _field(robot, "x", prefix, issues)
        y_raw = _field(robot, "y", prefix, issues)
        world_x_raw = _field(robot, "world_x", prefix, issues)
        world_y_raw = _field(robot, "world_y", prefix, issues)
        in_bounds_raw = _field(robot, "in_bounds", prefix, issues)

        color = _integer(color_raw, f"{prefix}.color", issues) if color_raw is not None else None
        robot_id = _integer(id_raw, f"{prefix}.id", issues) if id_raw is not None else None
        x = _float(x_raw, f"{prefix}.x", issues) if x_raw is not None else None
        y = _float(y_raw, f"{prefix}.y", issues) if y_raw is not None else None
        world_x = (
            _float(world_x_raw, f"{prefix}.world_x", issues)
            if world_x_raw is not None
            else None
        )
        world_y = (
            _float(world_y_raw, f"{prefix}.world_y", issues)
            if world_y_raw is not None
            else None
        )

        if color is not None and color not in VALID_COLORS:
            issues.append(f"{prefix}.color: {color} 不是 BLUE(0) 或 RED(2)")
        if robot_id is not None and not 0 <= robot_id <= 0xFF:
            issues.append(f"{prefix}.id: 必须是 uint8")
        if not isinstance(in_bounds_raw, bool):
            issues.append(f"{prefix}.in_bounds: 应为 bool")
            in_bounds = None
        else:
            in_bounds = in_bounds_raw

        if color is not None and robot_id is not None:
            key = (color, robot_id)
            if key in seen_keys:
                issues.append(f"{prefix}: 本帧存在重复的雷达身份 color={color}, id={robot_id}")
            seen_keys.add(key)

        if x is not None and y is not None and width is not None and height is not None:
            coordinate_is_inside = 0 <= x < width and 0 <= y < height
            if in_bounds is True and not coordinate_is_inside:
                issues.append(f"{prefix}: in_bounds=true，但像素坐标不在图像范围内")
            if in_bounds is False and coordinate_is_inside:
                issues.append(f"{prefix}: in_bounds=false，但像素坐标仍在图像范围内")

        values = (color, robot_id, x, y, world_x, world_y, in_bounds)
        if all(value is not None for value in values):
            robots.append(
                RobotPosition(
                    color=color,
                    id=robot_id,
                    x=x,
                    y=y,
                    world_x=world_x,
                    world_y=world_y,
                    in_bounds=in_bounds,
                )
            )

    if issues:
        raise RadarParseError(issues)

    assert None not in (sec, nsec, width, height, mpp)
    assert isinstance(frame_id_raw, str)
    return RadarFrame(
        timestamp=Timestamp(sec=sec, nanosec=nsec),
        frame_id=frame_id_raw,
        image_width=width,
        image_height=height,
        meters_per_pixel=mpp,
        robots=tuple(robots),
    )
