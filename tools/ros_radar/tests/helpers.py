from __future__ import annotations

from types import SimpleNamespace


def make_robot(
    *,
    color: int = 0,
    robot_id: int = 1,
    x: float = 100.25,
    y: float = 200.5,
    world_x: float = 1.234567890123,
    world_y: float = -2.345678901234,
    in_bounds: bool = True,
):
    return SimpleNamespace(
        color=color,
        id=robot_id,
        x=x,
        y=y,
        world_x=world_x,
        world_y=world_y,
        in_bounds=in_bounds,
    )


def make_message(*, robots=None, sec: int = 123, nanosec: int = 456_789_012):
    return SimpleNamespace(
        header=SimpleNamespace(
            stamp=SimpleNamespace(sec=sec, nanosec=nanosec), frame_id="rm_frame"
        ),
        image_width=1920,
        image_height=1080,
        meters_per_pixel=0.012345678901,
        robots=list(robots or []),
    )
