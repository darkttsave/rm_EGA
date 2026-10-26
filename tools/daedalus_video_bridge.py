#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Bridge Talos RGB24 frames to EGA's HEVC/UDP video protocol."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from typing import BinaryIO, Optional, Sequence


PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from sim.server.video_streamer import VideoStreamer  # noqa: E402


DEFAULT_WIDTH = 1440
DEFAULT_HEIGHT = 1080
DEFAULT_FPS = 30.0


def build_ffmpeg_command(
    executable: str,
    width: int,
    height: int,
    fps: float,
    frame_threads: int = 1,
) -> list[str]:
    """Build the bounded-latency RGB24-to-HEVC Annex-B encoder command."""
    return [
        executable,
        "-hide_banner",
        "-loglevel",
        "warning",
        "-f",
        "rawvideo",
        "-pixel_format",
        "rgb24",
        "-video_size",
        f"{width}x{height}",
        "-framerate",
        f"{fps:g}",
        "-i",
        "pipe:0",
        "-an",
        "-c:v",
        "libx265",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-pix_fmt",
        "yuv420p",
        "-x265-params",
        (
            "keyint=15:min-keyint=15:scenecut=0:repeat-headers=1:"
            f"frame-threads={frame_threads}"
        ),
        "-flush_packets",
        "1",
        "-f",
        "hevc",
        "pipe:1",
    ]


def read_exact(stream: BinaryIO, size: int) -> Optional[bytes]:
    """Read one fixed-size frame, accepting short pipe reads but not partial EOF."""
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            if not chunks:
                return None
            received = size - remaining
            raise EOFError(f"raw RGB pipe ended mid-frame ({received}/{size} bytes)")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def write_all(stream: BinaryIO, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = stream.write(view)
        if not written:
            raise BrokenPipeError("FFmpeg stdin closed")
        view = view[written:]


def terminate_owned(process: Optional[subprocess.Popen[bytes]]) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


class BridgeStats:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._started = time.monotonic()
        self._read_frames = 0
        self._fed_frames = 0
        self._stdin_write_ns = 0
        self._stdin_write_max_ns = 0
        self._encoded_frames = 0
        self._encoded_bytes = 0

    def record_read(self) -> None:
        with self._lock:
            self._read_frames += 1

    def record_fed(self, write_ns: int) -> None:
        with self._lock:
            self._fed_frames += 1
            self._stdin_write_ns += write_ns
            self._stdin_write_max_ns = max(self._stdin_write_max_ns, write_ns)

    def record_encoded(self, size: int) -> None:
        with self._lock:
            self._encoded_frames += 1
            self._encoded_bytes += size

    def snapshot_and_reset(self) -> dict[str, float]:
        with self._lock:
            now = time.monotonic()
            elapsed = max(now - self._started, 1e-9)
            values = {
                "elapsed": elapsed,
                "read_frames": float(self._read_frames),
                "fed_frames": float(self._fed_frames),
                "write_ns": float(self._stdin_write_ns),
                "write_max_ns": float(self._stdin_write_max_ns),
                "encoded_frames": float(self._encoded_frames),
                "encoded_bytes": float(self._encoded_bytes),
            }
            self._started = now
            self._read_frames = 0
            self._fed_frames = 0
            self._stdin_write_ns = 0
            self._stdin_write_max_ns = 0
            self._encoded_frames = 0
            self._encoded_bytes = 0
            return values


def report_bridge_stats(
    stats: BridgeStats,
    stop: threading.Event,
    target_fps: float,
) -> None:
    while not stop.wait(5.0):
        values = stats.snapshot_and_reset()
        elapsed = values["elapsed"]
        fed_frames = values["fed_frames"]
        encoded_fps = values["encoded_frames"] / elapsed
        write_avg_ms = (
            values["write_ns"] / fed_frames / 1_000_000.0 if fed_frames else 0.0
        )
        encoded_mbps = values["encoded_bytes"] * 8.0 / elapsed / 1_000_000.0
        print(
            "bridge: "
            f"rgb_read_fps={values['read_frames'] / elapsed:.2f} "
            f"ffmpeg_input_fps={fed_frames / elapsed:.2f} "
            f"encoded_fps={encoded_fps:.2f} "
            f"encoder_speed={encoded_fps / target_fps:.2f}x "
            f"stdin_write_avg_ms={write_avg_ms:.2f} "
            f"stdin_write_max_ms={values['write_max_ns'] / 1_000_000.0:.2f} "
            f"encoded_mbps={encoded_mbps:.2f}",
            file=sys.stderr,
        )


def pump_rgb_frames(
    reader_stdout: BinaryIO,
    encoder_stdin: BinaryIO,
    frame_size: int,
    stop: threading.Event,
    errors: list[BaseException],
    stats: BridgeStats,
) -> None:
    try:
        while not stop.is_set():
            frame = read_exact(reader_stdout, frame_size)
            if frame is None:
                break
            stats.record_read()
            write_started = time.perf_counter_ns()
            write_all(encoder_stdin, frame)
            stats.record_fed(time.perf_counter_ns() - write_started)
    except (BrokenPipeError, EOFError, OSError) as error:
        if not stop.is_set():
            errors.append(error)
    finally:
        try:
            encoder_stdin.close()
        except OSError:
            pass


def default_reader_path() -> str:
    configured = os.environ.get("TALOS_RGB_PIPE")
    if configured:
        return configured
    return str(Path.home() / "opensource/rm_AWAKENING-video/bin/talos_rgb_pipe")


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Daedalus/Talos RGB24 to EGA HEVC/UDP bridge"
    )
    parser.add_argument("--reader", default=default_reader_path())
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3334)
    parser.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    parser.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    parser.add_argument("--fps", type=float, default=DEFAULT_FPS)
    parser.add_argument("--reader-queue", type=int, choices=(1, 2), default=1)
    parser.add_argument("--x265-frame-threads", type=int, default=1)
    args = parser.parse_args(argv)
    if (
        args.width <= 0
        or args.height <= 0
        or args.fps <= 0
        or args.x265_frame_threads <= 0
    ):
        parser.error("width, height, fps and x265 frame threads must be positive")
    return args


def run(args: argparse.Namespace) -> int:
    frame_size = args.width * args.height * 3
    reader: Optional[subprocess.Popen[bytes]] = None
    encoder: Optional[subprocess.Popen[bytes]] = None
    pump_thread: Optional[threading.Thread] = None
    reporter_thread: Optional[threading.Thread] = None
    sender: Optional[VideoStreamer] = None
    stop = threading.Event()
    pump_errors: list[BaseException] = []
    stats = BridgeStats()
    interrupted = False

    try:
        reader = subprocess.Popen(
            [
                args.reader,
                "--fps",
                f"{args.fps:g}",
                "--queue-size",
                str(args.reader_queue),
            ],
            stdout=subprocess.PIPE,
            stderr=None,
            bufsize=0,
        )
        encoder = subprocess.Popen(
            build_ffmpeg_command(
                args.ffmpeg,
                args.width,
                args.height,
                args.fps,
                args.x265_frame_threads,
            ),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,
            bufsize=0,
        )
        if reader.stdout is None or encoder.stdin is None or encoder.stdout is None:
            raise RuntimeError("failed to create bridge pipes")

        sender = VideoStreamer(target_ip=args.host, port=args.port)
        sender.frame_extractor.reset()
        pump_thread = threading.Thread(
            target=pump_rgb_frames,
            args=(reader.stdout, encoder.stdin, frame_size, stop, pump_errors, stats),
            name="talos-rgb-pump",
        )
        pump_thread.start()
        reporter_thread = threading.Thread(
            target=report_bridge_stats,
            args=(stats, stop, args.fps),
            name="video-bridge-stats",
        )
        reporter_thread.start()

        print(
            f"bridge: RGB24 {args.width}x{args.height}@{args.fps:g} -> "
            f"HEVC/UDP {args.host}:{args.port}",
            file=sys.stderr,
        )
        while True:
            chunk = encoder.stdout.read(4096)
            if not chunk:
                break
            for access_unit in sender.frame_extractor.push(chunk):
                stats.record_encoded(len(access_unit))
                sender.send_frame(access_unit)

        for access_unit in sender.frame_extractor.flush():
            stats.record_encoded(len(access_unit))
            sender.send_frame(access_unit)

        if pump_thread is not None:
            pump_thread.join(timeout=2)
        if pump_errors:
            raise RuntimeError(f"RGB pump failed: {pump_errors[0]}")

        reader_code = reader.poll()
        encoder_code = encoder.poll()
        raise RuntimeError(
            "bridge child exited unexpectedly "
            f"(reader={reader_code}, ffmpeg={encoder_code})"
        )
    except KeyboardInterrupt:
        interrupted = True
        print("bridge: interrupted", file=sys.stderr)
    except (FileNotFoundError, RuntimeError, OSError) as error:
        print(f"bridge: error: {error}", file=sys.stderr)
        return 1
    finally:
        stop.set()
        terminate_owned(reader)
        terminate_owned(encoder)
        if pump_thread is not None:
            pump_thread.join(timeout=2)
        if reporter_thread is not None:
            reporter_thread.join(timeout=2)
        if sender is not None:
            sender.stop()

    return 130 if interrupted else 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
