"""Pruebas del contrato C/Python y del servidor real, sin placa ni puerto fijo.

python -m unittest discover -s tools/tests -v
ORBITA_WIRE_FIXTURE opcional: paquete generado por test_audio_ws_wire.c.
"""
import asyncio
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ws_audio_protocol import Recording, HEADER, ERROR_STATS
from ws_audio_server import handle
from websockets.asyncio.server import serve
from websockets.asyncio.client import connect

IDENTITY = "0123456789abcdef"
PCM = struct.pack("<2048h", *[i-1024 for i in range(2048)])


def start(identity=IDENTITY):
    return dict(type="start", protocol="orbita-pcm-v1", id=identity, rate=16000,
                channels=1, format="s16le", source="mic_l", target_samples=16000,
                dropped_before=0, failed_attempts=0)


def packet(index, seq=None, lost=0, identity=IDENTITY):
    return HEADER.pack(b"ORBA", 1, 40, 0, int(identity, 16), index,
                       index*4 if seq is None else seq, lost, 4, 2048, 0, 0,
                       zlib.crc32(PCM)) + PCM


def end(crc=None):
    return dict(type="end", id=IDENTITY, samples=16384, messages=8,
                crc32=zlib.crc32(PCM*8) if crc is None else crc,
                stats=dict.fromkeys(ERROR_STATS, 0))


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.r = Recording(self.temp.name)
        self.r.process(json.dumps(start()))

    def tearDown(self):
        self.r.close()
        self.temp.cleanup()

    def feed(self, gap=False):
        for i in range(8):
            self.r.process(packet(i, i*4+(4 if gap and i else 0), 2048 if gap and i == 1 else 0))

    def test_complete_exact_pcm(self):
        self.feed()
        self.assertEqual(self.r.end(end()), f"saved {IDENTITY} clean")
        with wave.open(str(self.r.path)) as wav:
            self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
            self.assertEqual(wav.readframes(20000), PCM*8)
        report = json.loads(self.r.path.with_suffix(".json").read_text())
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["file"], self.r.path.name)

    def test_gap_auditable(self):
        self.feed(gap=True)
        self.assertTrue(self.r.end(end()).endswith("gaps"))
        self.assertTrue(self.r.path.name.endswith(".gaps.wav"))
        self.assertEqual(len(self.r.info["gaps"]), 1)
        self.assertEqual(self.r.info["lost_reported"], 2048)

    def test_hardware_error_marks_gap(self):
        self.feed()
        control = end()
        control["stats"]["dma_overflows"] = 1
        self.assertTrue(self.r.end(control).endswith("gaps"))

    def test_bad_crc_and_length_and_identity(self):
        bad_crc = bytearray(packet(0)); bad_crc[-1] ^= 1
        for data in (bytes(bad_crc), packet(0)[:-1], packet(0, identity="1111111111111111")):
            with self.assertRaises(ValueError):
                self.r.binary(data)
        self.assertEqual(self.r.info["samples"], 0)

    def test_duplicate_and_missing_message_rejected(self):
        self.r.binary(packet(0))
        for data in (packet(0), packet(2), packet(1, seq=0)):
            with self.assertRaises(ValueError):
                self.r.binary(data)

    def test_slot_sequence_wrap(self):
        self.r.binary(packet(0, seq=0xfffffffc))
        self.r.binary(packet(1, seq=0))
        self.assertEqual(self.r.info["gaps"], [])

    def test_storage_failure_keeps_partial_report(self):
        self.feed()
        original = self.r.path
        with patch.object(Path, "rename", side_effect=OSError("disco no disponible")):
            with self.assertRaises(OSError):
                self.r.end(end())
        self.assertEqual(self.r.path, original)
        self.assertTrue(original.exists())
        report = json.loads(original.with_suffix(".json").read_text())
        self.assertEqual(report["status"], "incomplete")
        self.assertIn("disco no disponible", report["reason"])
        self.assertEqual(len(list(Path(self.temp.name).glob("*.json"))), 1)

    def test_missing_end_preserves_partial(self):
        self.r.binary(packet(0))
        path = self.r.close()
        with wave.open(str(path)) as wav:
            self.assertEqual(wav.getnframes(), 2048)
        self.assertEqual(json.loads(path.with_suffix(".json").read_text())["status"], "incomplete")

    def test_new_start_keeps_previous_separate(self):
        self.r.binary(packet(0))
        previous = self.r.path
        self.r.start(start("2222222222222222"))
        self.assertNotEqual(previous, self.r.path)
        self.assertEqual(self.r.info["samples"], 0)
        self.assertTrue(previous.exists())

    def test_end_mismatch_and_early_end(self):
        with self.assertRaises(ValueError):
            self.r.end(end())
        self.feed()
        with self.assertRaises(ValueError):
            self.r.end(end(crc=0))
        with self.assertRaises(ValueError):
            self.r.binary(packet(8))

    @unittest.skipUnless(os.environ.get("ORBITA_WIRE_FIXTURE"), "Compilar fixture C para prueba cruzada")
    def test_c_encoder_matches_python(self):
        self.assertEqual(Path(os.environ["ORBITA_WIRE_FIXTURE"]).read_bytes(), packet(0))


class SocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_websocket_complete_and_disconnect(self):
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: handle(ws, output), "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(json.dumps(start()))
                    self.assertEqual(await ws.recv(), f"ready {IDENTITY}")
                    for i in range(8):
                        await ws.send(packet(i))
                    await ws.send(json.dumps(end()))
                    self.assertEqual(await ws.recv(), f"saved {IDENTITY} clean")
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(json.dumps(start()))
                    await ws.recv()
                    await ws.send(packet(0))
            # El cierre del servidor espera que sus handlers guarden los parciales.
            reports = [json.loads(p.read_text()) for p in Path(output).glob("*.json")]
            self.assertEqual(sorted(r["status"] for r in reports), ["complete", "incomplete"])


if __name__ == "__main__":
    unittest.main()
