"""Recorte exacto y devolucion PLAY por socket real, con uplink simultaneo."""
import asyncio
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
import ws_audio_server
from ws_audio_protocol import HEADER
from ws_audio_speech import AudioWindow, PLAY_HEADER

IDENTITY = "0123456789abcdef"


def hello():
    return json.dumps(dict(type="hello", protocol="orbita-pcm-v2", id=IDENTITY,
                           rate=16000, channels=1, format="s16le", source="mic_l"))


def pcm_for(index):
    return struct.pack("<2048h", *[((i + index * 83) % 32768) - 16384 for i in range(2048)])


def packet(index):
    pcm = pcm_for(index)
    return HEADER.pack(b"ORBA", 1, 40, 0, int(IDENTITY, 16), index,
                       index * 4, 0, 4, 2048, 0, 0, zlib.crc32(pcm)) + pcm


class ReplayWindowTests(unittest.TestCase):
    def test_exact_crop_crosses_packet_boundaries_and_only_completes_once(self):
        window = AudioWindow(10, start_seconds=2)
        source = b"".join(pcm_for(i) for i in range(100))
        results = []
        for offset in range(0, len(source), 4096):
            result = window.add(source[offset:offset + 4096])
            if result is not None:
                results.append(result)
        self.assertEqual(results, [source[2 * 32000:12 * 32000]])
        self.assertEqual(len(results[0]), 320000)
        self.assertIsNone(window.add(b"\0\0" * 16000))

    def test_incomplete_capture_is_not_returned(self):
        window = AudioWindow(10, start_seconds=2)
        self.assertIsNone(window.add(b"\0" * (11 * 32000)))
        self.assertFalse(window.complete)

    def test_invalid_start_is_rejected(self):
        for start in (-1, float("nan"), float("inf")):
            with self.subTest(start=start), self.assertRaises(ValueError):
                AudioWindow(10, start_seconds=start)


class ReplaySocketTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        ws_audio_server.LAST_SESSION.clear()

    async def test_voice_round_trip_and_uplink_continues_during_and_after_play(self):
        with tempfile.TemporaryDirectory() as output, self.assertLogs("audio_server", "INFO") as logs:
            async with serve(lambda ws: ws_audio_server.handle(ws, output, replay_window=(2, 12)),
                             "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello())
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    for index in range(96):
                        await ws.send(packet(index))
                    binary = []
                    begin = end = None
                    last_ack = -1
                    extra_sent = False
                    while end is None or last_ack < 111:
                        message = await asyncio.wait_for(ws.recv(), 10)
                        if isinstance(message, bytes):
                            binary.append(message)
                            if not extra_sent:
                                # La ESP sigue produciendo audio mientras recibe PLAY.
                                for index in range(96, 112):
                                    await ws.send(packet(index))
                                extra_sent = True
                        else:
                            obj = json.loads(message)
                            if obj["type"] == "ack":
                                last_ack = obj["index"]
                            elif obj["type"] == "play":
                                self.assertIsNone(begin)
                                begin = obj
                            elif obj["type"] == "play_end":
                                end = obj
                    self.assertEqual(begin["bytes"], 320000)
                    self.assertEqual(end["play_id"], begin["play_id"])
                    self.assertEqual(len(binary), 79)
                    received = bytearray()
                    for seq, frame in enumerate(binary):
                        magic, version, size, flags, identity, number, length = PLAY_HEADER.unpack_from(frame)
                        self.assertEqual((magic, version, size, flags, identity, number),
                                         (b"PLAY", 1, 24, 0, int(begin["play_id"], 16), seq))
                        self.assertLessEqual(len(frame), 4096)
                        self.assertEqual(length, len(frame) - 24)
                        received.extend(frame[24:])
                    expected = b"".join(pcm_for(i) for i in range(96))[64000:384000]
                    self.assertEqual(bytes(received), expected)
                    await ws.send(json.dumps(dict(type="play_done", play_id=begin["play_id"],
                                                  reason="end", played_ms=10000)))
                    for index in range(112, 120):
                        await ws.send(packet(index))
                    while last_ack < 119:
                        obj = json.loads(await asyncio.wait_for(ws.recv(), 10))
                        self.assertEqual(obj["type"], "ack")  # no segundo PLAY/eco
                        last_ack = obj["index"]
            references = list(Path(output).glob("replay_*.wav"))
            self.assertEqual(len(references), 1)
            with wave.open(str(references[0]), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
                self.assertEqual(wav.readframes(wav.getnframes()), expected)
            summary = json.loads(next(Path(output).glob("sesion_*_????????????????.json")).read_text())
            self.assertEqual(summary["messages"], 120)
            self.assertEqual(summary["gaps"], [])
            self.assertTrue(any("Playback terminado" in line for line in logs.output))

    async def test_disconnect_before_window_finishes_does_not_send_partial_audio(self):
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: ws_audio_server.handle(ws, output, replay_window=(2, 12)),
                             "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello())
                    await ws.recv()
                    for index in range(8):
                        await ws.send(packet(index))
                    self.assertEqual(json.loads(await ws.recv())["type"], "ack")
            self.assertEqual(list(Path(output).glob("replay_*.wav")), [])


if __name__ == "__main__":
    unittest.main()
