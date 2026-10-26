#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import io
from pathlib import Path
import struct
import sys
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SIM_ROOT = PROJECT_ROOT / "sim"
for path in (PROJECT_ROOT, SIM_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from server.hevc_annexb import HevcAnnexBFrameExtractor
from server.video_streamer import VideoStreamer
from tools.daedalus_video_bridge import build_ffmpeg_command, parse_args, read_exact


class ShortReader:
    def __init__(self, chunks):
        self.chunks = list(chunks)

    def read(self, size):
        if not self.chunks:
            return b""
        chunk = self.chunks.pop(0)
        if len(chunk) <= size:
            return chunk
        self.chunks.insert(0, chunk[size:])
        return chunk[:size]


class FakeSocket:
    def __init__(self):
        self.packets = []

    def sendto(self, packet, address):
        self.packets.append((packet, address))


def nal(nal_type, first_slice=False):
    return b"\x00\x00\x00\x01" + bytes(
        ((nal_type << 1) & 0x7E, 1, 0x80 if first_slice else 0)
    )


class VideoBridgeTest(unittest.TestCase):
    def test_read_exact_joins_short_reads_and_rejects_partial_frame(self):
        self.assertEqual(b"abcdef", read_exact(ShortReader([b"ab", b"c", b"def"]), 6))
        with self.assertRaises(EOFError):
            read_exact(ShortReader([b"abc"]), 6)
        self.assertIsNone(read_exact(io.BytesIO(), 6))

    def test_ffmpeg_command_requests_low_latency_annex_b_hevc(self):
        command = build_ffmpeg_command("ffmpeg", 1440, 1080, 30)
        joined = " ".join(command)
        self.assertIn("-pixel_format rgb24", joined)
        self.assertIn("-video_size 1440x1080", joined)
        self.assertIn("-c:v libx265", joined)
        self.assertIn("-preset ultrafast", joined)
        self.assertIn("-tune zerolatency", joined)
        self.assertIn("-pix_fmt yuv420p", joined)
        self.assertIn(
            "keyint=15:min-keyint=15:scenecut=0:repeat-headers=1:frame-threads=1",
            joined,
        )
        self.assertIn("-flush_packets 1", joined)
        self.assertEqual(["-f", "hevc", "pipe:1"], command[-3:])

    def test_low_latency_defaults_use_one_frame_queue_and_encoder_thread(self):
        args = parse_args([])
        self.assertEqual(1, args.reader_queue)
        self.assertEqual(1, args.x265_frame_threads)

    def test_annex_b_access_units_survive_arbitrary_chunks(self):
        stream = nal(32) + nal(33) + nal(34) + nal(19, True) + nal(1, True)
        extractor = HevcAnnexBFrameExtractor()
        output = []
        for offset in range(0, len(stream), 5):
            output.extend(extractor.push(stream[offset : offset + 5]))
        output.extend(extractor.flush())
        self.assertEqual(2, len(output))
        self.assertIn(nal(19, True), output[0])
        self.assertEqual(nal(1, True), output[1])

    def test_public_sender_uses_big_endian_header_and_reassembles(self):
        sender = object.__new__(VideoStreamer)
        sender.target_ip = "127.0.0.1"
        sender.port = 3334
        sender.frame_id = 7
        sender.sock = FakeSocket()
        sender.log_callback = None

        chunk_size = sender.MAX_PAYLOAD_SIZE - sender.HEADER_SIZE
        frame = bytes((index % 251 for index in range(chunk_size * 2 + 17)))
        sender.send_frame(frame)

        rebuilt = bytearray()
        self.assertEqual(3, len(sender.sock.packets))
        for expected_slice, (packet, address) in enumerate(sender.sock.packets):
            frame_id, slice_id, total_bytes = struct.unpack(
                "!HHI", packet[: sender.HEADER_SIZE]
            )
            self.assertEqual((7, expected_slice, len(frame)), (frame_id, slice_id, total_bytes))
            self.assertEqual(("127.0.0.1", 3334), address)
            rebuilt.extend(packet[sender.HEADER_SIZE :])
        self.assertEqual(frame, bytes(rebuilt))
        self.assertEqual(8, sender.frame_id)


if __name__ == "__main__":
    unittest.main()
