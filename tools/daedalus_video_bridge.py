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
        "keyint=15:min-keyint=15:scenecut=0:repeat-headers=1",
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


def pump_rgb_frames(
    reader_stdout: BinaryIO,
    encoder_stdin: BinaryIO,
    frame_size: int,
    stop: threading.Event,
    errors: list[BaseException],
) -> None:
    frame_count = 0
    report_started = time.monotonic()
    try:
        while not stop.is_set():
            frame = read_exact(reader_stdout, frame_size)
            if frame is None:
                break
            write_all(encoder_stdin, frame)
            frame_count += 1
            now = time.monotonic()
            if now - report_started >= 5.0:
                rate = frame_count / (now - report_started)
                print(
                    f"bridge: fed {frame_count} RGB frames ({rate:.1f} fps)",
                    file=sys.stderr,
                )
                frame_count = 0
                report_started = now
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
    parser.add_argument("--reader-queue", type=int, choices=(1, 2), default=2)
    args = parser.parse_args(argv)
    if args.width <= 0 or args.height <= 0 or args.fps <= 0:
        parser.error("width, height and fps must be positive")
    return args


def run(args: argparse.Namespace) -> int:
    frame_size = args.width * args.height * 3
    reader: Optional[subprocess.Popen[bytes]] = None
    encoder: Optional[subprocess.Popen[bytes]] = None
    pump_thread: Optional[threading.Thread] = None
    sender: Optional[VideoStreamer] = None
    stop = threading.Event()
    pump_errors: list[BaseException] = []
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
            build_ffmpeg_command(args.ffmpeg, args.width, args.height, args.fps),
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
            args=(reader.stdout, encoder.stdin, frame_size, stop, pump_errors),
            name="talos-rgb-pump",
        )
        pump_thread.start()

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
                sender.send_frame(access_unit)

        for access_unit in sender.frame_extractor.flush():
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
        if sender is not None:
            sender.stop()

    return 130 if interrupted else 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
