"""Known signal through the real PLAY path, with ongoing uplink."""
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
from ws_audio_protocol import HEADER
from ws_audio_speech import PLAY_HEADER, test_tone_pcm
import ws_audio_server


class ToneTests(unittest.TestCase):
    def test_exact_dante_waveform_and_duration(self):
        pcm = test_tone_pcm()
        self.assertEqual(len(pcm), 96000)
        cycle = struct.unpack("<16h", pcm[:32])
        self.assertEqual(cycle, (0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
                                 0, -6123, -11314, -14782, -16000, -14782, -11314, -6123))
        self.assertEqual(pcm, pcm[:32] * 3000)


class ToneSocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_tone_sent_without_mic_content_and_uplink_keeps_working(self):
        ws_audio_server.LAST_SESSION.clear()
        pcm = test_tone_pcm()
        identity = 0x123456789abcdef
        silence = bytes(4096)
        def packet(index):
            return HEADER.pack(b"ORBA", 1, 40, 0, identity, index, index * 4,
                               0, 4, 2048, 0, 0, zlib.crc32(silence)) + silence
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: ws_audio_server.handle(ws, output, test_pcm=pcm),
                             "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(json.dumps(dict(type="hello", protocol="orbita-pcm-v2",
                        id=f"{identity:016x}", rate=16000, channels=1, format="s16le", source="mic_l")))
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    # Fuente silenciosa: la respuesta debe ser nuestro tono, no los mics.
                    for index in range(8):
                        await ws.send(packet(index))
                    begin = end = None
                    got_ack = False
                    frames = []
                    while end is None or not got_ack:
                        message = await asyncio.wait_for(ws.recv(), 5)
                        if isinstance(message, bytes):
                            frames.append(message)
                        else:
                            obj = json.loads(message)
                            if obj["type"] == "play": begin = obj
                            elif obj["type"] == "play_end": end = obj
                            elif obj["type"] == "ack": got_ack = obj["index"] == 7
                    self.assertEqual(begin["bytes"], 96000)
                    self.assertEqual(begin["play_id"], end["play_id"])
                    self.assertEqual(len(frames), 24)
                    received = bytearray()
                    for number, frame in enumerate(frames):
                        magic, version, header, flags, play_id, seq, length = PLAY_HEADER.unpack_from(frame)
                        self.assertEqual((magic, version, header, flags, play_id, seq),
                            (b"PLAY", 1, 24, 0, int(begin["play_id"], 16), number))
                        self.assertLessEqual(len(frame), 4096)
                        self.assertEqual(length, len(frame) - 24)
                        received.extend(frame[24:])
                    self.assertEqual(bytes(received), pcm)
                    self.assertEqual(zlib.crc32(received), zlib.crc32(pcm))
                    await ws.send(json.dumps(dict(type="play_done", play_id=begin["play_id"],
                                                  reason="end", played_ms=3000)))
                    for index in range(8, 16): await ws.send(packet(index))
                    self.assertEqual(json.loads(await asyncio.wait_for(ws.recv(), 5)), dict(type="ack", index=15))
            reference = next(Path(output).glob("test_play_*.wav"))
            with wave.open(str(reference), "rb") as wav:
                self.assertEqual(wav.readframes(wav.getnframes()), pcm)


if __name__ == "__main__":
    unittest.main()
