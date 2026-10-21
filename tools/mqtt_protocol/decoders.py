"""业务协议解码器。

第一版**只**注册一个 MQTT 业务 Topic：``RobotPathPlanInfo``。

``sim/robomaster_pb2.py`` 里还有很多其它消息类型，但它们**不会被自动注册**——
新增协议必须有真实任务、协议定义和测试样例，然后显式加入 :data:`DECODERS`。

本模块只做「字节 -> 字段 + 检查结果」，不负责 MQTT 传输，也不发布任何消息。
"""

from __future__ import annotations

import importlib
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

PROJECT_ROOT = Path(__file__).resolve().parents[2]
GENERATED_PB2_PATH = PROJECT_ROOT / "sim/robomaster_pb2.py"
RELEASE_TOOL_DIR = PROJECT_ROOT / "tools/release"
GENERATE_HINT = "python3 tools/release/generate_sim_protobuf.py"

STATUS_PASS = "PASS"
STATUS_NOT_MATCH = "NOT_MATCH"
STATUS_PARSE_FAIL = "PARSE_FAIL"

# 2026 通信协议 V2.0.0：场地尺寸与路径点数量
FIELD_WIDTH_DM = 280
FIELD_HEIGHT_DM = 150
EXPECTED_OFFSET_COUNT = 49
INT8_MIN = -128
INT8_MAX = 127

# 协议给定的 intention 语义（仅用于说明，不参与判定）
INTENTION_MEANINGS = {
    1: "到目标点攻击",
    2: "到目标点防守",
    3: "移动到目标点",
}

Point = tuple[int, int]


class DecoderError(RuntimeError):
    """解码器运行环境问题（缺少 pb2 生成物或 protobuf 运行时）。"""


@dataclass
class DecodeResult:
    """一次解码的完整结果。

    即使语义检查失败，``fields`` 与 ``points`` 也会保留已解析出的内容。
    """

    status: str
    topic: str
    fields: dict[str, Any] = field(default_factory=dict)
    points: list[Point] = field(default_factory=list)
    issues: list[str] = field(default_factory=list)
    error: str | None = None


def _required_protobuf_requirement(pb2_path: Path) -> str | None:
    """复用仓库门禁推导 pb2 需要的 protobuf 版本范围，失败时返回 None。"""

    inserted = str(RELEASE_TOOL_DIR) not in sys.path
    if inserted:
        sys.path.insert(0, str(RELEASE_TOOL_DIR))
    try:
        checker = importlib.import_module("check_sim_protobuf_runtime")
        generated = checker.parse_generated_runtime(pb2_path)
        return checker.expected_requirement(generated)
    except Exception:  # noqa: BLE001 - 仅用于生成提示，失败就退回通用文案
        return None
    finally:
        if inserted:
            try:
                sys.path.remove(str(RELEASE_TOOL_DIR))
            except ValueError:
                pass


def load_message_class(message_name: str):
    """从仓库既有的 ``sim/robomaster_pb2.py`` 载入消息类。

    不复制、不重新生成第二份 pb2；缺生成物或缺 protobuf 运行时都给出明确提示。
    """

    if not GENERATED_PB2_PATH.is_file():
        raise DecoderError(
            f"缺少生成的 Protobuf 模块：{GENERATED_PB2_PATH}\n"
            f"请先运行： {GENERATE_HINT}"
        )

    sim_dir = str(GENERATED_PB2_PATH.parent)
    if sim_dir not in sys.path:
        sys.path.insert(0, sim_dir)
    try:
        module = importlib.import_module("robomaster_pb2")
    except Exception as error:  # noqa: BLE001 - 版本/依赖问题统一转成提示
        requirement = _required_protobuf_requirement(GENERATED_PB2_PATH)
        advice = (
            f"请安装与生成代码同主版本的 Protobuf 运行时： pip install '{requirement}'"
            if requirement
            else "请安装与生成代码同主版本的 Protobuf 运行时（protobuf>=6.33.2,<7）"
        )
        raise DecoderError(f"无法导入 {GENERATED_PB2_PATH}：{error}\n{advice}") from error

    message_class = getattr(module, message_name, None)
    if message_class is None:
        raise DecoderError(
            f"{GENERATED_PB2_PATH.name} 中不存在消息类型 {message_name}"
        )
    return message_class


def _reconstruct_points(
    start_x: int, start_y: int, offset_x: list[int], offset_y: list[int]
) -> list[Point]:
    """当前点 = 上一个点 + 当前增量；起点取自 start_pos_x/start_pos_y。"""

    points: list[Point] = [(int(start_x), int(start_y))]
    current_x, current_y = int(start_x), int(start_y)
    for delta_x, delta_y in zip(offset_x, offset_y):
        current_x += int(delta_x)
        current_y += int(delta_y)
        points.append((current_x, current_y))
    return points


def _check_offsets(values: list[int], axis: str, issues: list[str]) -> None:
    out_of_range = [value for value in values if not INT8_MIN <= value <= INT8_MAX]
    if out_of_range:
        issues.append(
            f"offset_{axis} 有 {len(out_of_range)} 个增量超出 int8 范围"
            f"[{INT8_MIN},{INT8_MAX}]，首个为 {out_of_range[0]}"
        )


def decode_robot_path_plan(
    payload: bytes, *, expected_sender_id: int | None = None
) -> DecodeResult:
    """解析 Topic ``RobotPathPlanInfo`` 并按协议做语义检查。"""

    topic = "RobotPathPlanInfo"
    message_class = load_message_class(topic)
    message = message_class()

    try:
        message.ParseFromString(payload)
    except Exception as error:  # noqa: BLE001 - 非法 Protobuf 属于正常输入
        return DecodeResult(
            status=STATUS_PARSE_FAIL,
            topic=topic,
            error=f"Protobuf 解析失败：{error}",
        )

    intention = int(message.intention)
    start_x = int(message.start_pos_x)
    start_y = int(message.start_pos_y)
    sender_id = int(message.sender_id)
    offset_x = [int(value) for value in message.offset_x]
    offset_y = [int(value) for value in message.offset_y]

    points = _reconstruct_points(start_x, start_y, offset_x, offset_y)

    fields: dict[str, Any] = {
        "intention": intention,
        "start_pos_x": start_x,
        "start_pos_y": start_y,
        "offset_x_count": len(offset_x),
        "offset_y_count": len(offset_y),
        "sender_id": sender_id,
        "point_count": len(points),
    }

    issues: list[str] = []

    if intention not in INTENTION_MEANINGS:
        issues.append(
            f"intention={intention} 不在 1/2/3 之内"
            f"（1={INTENTION_MEANINGS[1]}，2={INTENTION_MEANINGS[2]}，"
            f"3={INTENTION_MEANINGS[3]}）"
        )

    if len(offset_x) != len(offset_y):
        issues.append(
            f"offset_x({len(offset_x)}) 与 offset_y({len(offset_y)}) 数量不一致"
        )

    if EXPECTED_OFFSET_COUNT not in (len(offset_x), len(offset_y)):
        issues.append(
            f"增量数量不是 {EXPECTED_OFFSET_COUNT}"
            f"（offset_x={len(offset_x)}，offset_y={len(offset_y)}）"
        )

    _check_offsets(offset_x, "x", issues)
    _check_offsets(offset_y, "y", issues)

    if not 0 <= start_x <= FIELD_WIDTH_DM:
        issues.append(
            f"start_pos_x={start_x} 超出场地范围 [0,{FIELD_WIDTH_DM}] dm"
        )
    if not 0 <= start_y <= FIELD_HEIGHT_DM:
        issues.append(
            f"start_pos_y={start_y} 超出场地范围 [0,{FIELD_HEIGHT_DM}] dm"
        )

    out_of_field = [
        (index, point)
        for index, point in enumerate(points)
        if not (0 <= point[0] <= FIELD_WIDTH_DM and 0 <= point[1] <= FIELD_HEIGHT_DM)
    ]
    if out_of_field:
        index, point = out_of_field[0]
        issues.append(
            f"累计生成的第 {index} 个路径点 {point} 越出场地边界"
            f"（x∈[0,{FIELD_WIDTH_DM}]，y∈[0,{FIELD_HEIGHT_DM}] dm）"
        )

    if expected_sender_id is not None and sender_id != int(expected_sender_id):
        issues.append(
            f"sender_id={sender_id} 与期望的 {int(expected_sender_id)} 不一致"
        )

    return DecodeResult(
        status=STATUS_NOT_MATCH if issues else STATUS_PASS,
        topic=topic,
        fields=fields,
        points=points,
        issues=issues,
    )


DECODERS: dict[str, Callable[..., DecodeResult]] = {
    "RobotPathPlanInfo": decode_robot_path_plan,
}


def is_supported(topic: str) -> bool:
    return topic in DECODERS


def decode(
    topic: str, payload: bytes, *, expected_sender_id: int | None = None
) -> DecodeResult | None:
    """按白名单解码；未注册的 Topic 返回 None，由上层标记 UNSUPPORTED。"""

    decoder = DECODERS.get(topic)
    if decoder is None:
        return None
    return decoder(payload, expected_sender_id=expected_sender_id)
