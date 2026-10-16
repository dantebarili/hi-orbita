"""Pruebas del protocolo continuo orbita-pcm-v2 (sesion), sin placa.

python -m unittest discover -s tools/tests -v
"""
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
import ws_audio_server
from ws_audio_protocol import HEADER
from ws_audio_server import handle
from ws_audio_session import Session, ACK_EVERY
from websockets.asyncio.server import serve
from websockets.asyncio.client import connect

IDENTITY = "0123456789abcdef"
PCM = struct.pack("<2048h", *[i - 1024 for i in range(2048)])


def hello(identity=IDENTITY):
    return json.dumps(dict(type="hello", protocol="orbita-pcm-v2", id=identity,
                           rate=16000, channels=1, format="s16le", source="mic_l"))


def packet(index, seq=None, lost=0, identity=IDENTITY):
    return HEADER.pack(b"ORBA", 1, 40, 0, int(identity, 16), index,
                       index * 4 if seq is None else seq, lost, 4, 2048, 0, 0,
                       zlib.crc32(PCM)) + PCM


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.temp.cleanup()

    def session(self, **kwargs):
        s = Session(self.temp.name, **kwargs)
        self.assertEqual(json.loads(s.process(hello())), dict(type="ready", id=IDENTITY))
        return s

    def test_right_microphone_source(self):
        s = Session(self.temp.name, record=False)
        message = json.loads(hello())
        message["source"] = "mic_r"
        self.assertEqual(json.loads(s.process(json.dumps(message)))["type"], "ready")
        self.assertEqual(s.info["source"], "mic_r")
        s.process(packet(0))
        s.close()
        self.assertEqual(list(Path(self.temp.name).iterdir()), [])

    def test_continuous_and_ack(self):
        s = self.session()
        acks = [s.process(packet(i)) for i in range(ACK_EVERY * 2)]
        sent = [json.loads(a) for a in acks if a]
        self.assertEqual(sent, [dict(type="ack", index=ACK_EVERY - 1),
                                dict(type="ack", index=ACK_EVERY * 2 - 1)])
        self.assertEqual(s.close(), ACK_EVERY * 2 * 4)
        self.assertEqual(s.gaps, [])

    def test_rotation_into_segments(self):
        s = self.session(segment_samples=2048 * 3)     # 3 mensajes por archivo
        for i in range(7):
            s.process(packet(i))
        s.close()
        wavs = sorted(Path(self.temp.name).glob("*.wav"))
        self.assertEqual(len(wavs), 3)                  # 3 + 3 + 1 mensajes
        sizes = []
        for path in wavs:
            with wave.open(str(path)) as w:
                sizes.append(w.getnframes())
        self.assertEqual(sizes, [2048 * 3, 2048 * 3, 2048])
        status = [json.loads(p.read_text())["status"]
                  for p in sorted(Path(self.temp.name).glob("*_???.json"))]
        self.assertEqual(status, ["complete", "complete", "interrupted"])

    def test_no_empty_wav_after_rotation(self):
        s = self.session(segment_samples=2048 * 2)
        for i in range(4):
            s.process(packet(i))                        # rota justo al final
        s.close()
        self.assertEqual(len(list(Path(self.temp.name).glob("*.wav"))), 2)

    def test_gap_is_recorded_not_filled(self):
        s = self.session()
        s.process(packet(0))
        s.process(packet(1, seq=4 + 40, lost=40 * 512))     # faltan 40 slots
        s.close()
        self.assertEqual(len(s.gaps), 1)
        self.assertEqual(s.gaps[0]["missing_slots"], 40)
        self.assertEqual(s.samples, 2 * 2048)               # el WAV no se rellena

    def test_rejects_wrong_index_crc_and_identity(self):
        s = self.session()
        with self.assertRaises(ValueError):
            s.process(packet(1))                            # el indice debe empezar en 0
        s.process(packet(0))
        with self.assertRaises(ValueError):
            s.process(packet(0, seq=4))                     # repetido
        with self.assertRaises(ValueError):
            s.process(packet(1, identity="ffffffffffffffff"))
        bad = bytearray(packet(1))
        bad[-1] ^= 1
        with self.assertRaises(ValueError):
            s.process(bytes(bad))
        s.close()

    def test_hello_validation_and_no_second_hello(self):
        s = Session(self.temp.name)
        with self.assertRaises(ValueError):
            s.process(json.dumps(dict(type="hello", protocol="orbita-pcm-v1", id=IDENTITY)))
        s.process(hello())
        with self.assertRaises(ValueError):
            s.process(hello())
        s.close()

    def test_gap_from_previous_session(self):
        s = self.session(previous_expected_seq=100)
        s.process(packet(0, seq=100 + 5))                   # faltan 5 slots entre sesiones
        s.close()
        self.assertEqual(s.info["gap_from_previous_session_slots"], 5)
        self.assertFalse(s.info["previous_session_restart"])

    def test_continuous_previous_session_and_restart(self):
        s = self.session(previous_expected_seq=100)
        s.process(packet(0, seq=100))
        s.close()
        self.assertEqual(s.info["gap_from_previous_session_slots"], 0)
        s = self.session(previous_expected_seq=100000)
        s.process(packet(0, seq=0))                         # el equipo se reinicio
        s.close()
        self.assertIsNone(s.info["gap_from_previous_session_slots"])
        self.assertTrue(s.info["previous_session_restart"])

    def test_previous_session_still_open_when_new_one_starts(self):
        """Corte de red: la conexion vieja sigue abierta en el servidor cuando llega la nueva."""
        vieja = self.session()
        for i in range(3):
            vieja.process(packet(i))                        # expected_seq de la vieja = 12
        nueva = Session(self.temp.name, previous=vieja)
        nueva.process(hello("fedcba9876543210"))
        vieja.process(packet(3, seq=12))                    # la vieja recibe un mensaje mas, ya creada la nueva
        nueva.process(packet(0, seq=16, identity="fedcba9876543210"))
        self.assertEqual(nueva.info["gap_from_previous_session_slots"], 0)   # 16 = fin vivo de la vieja
        self.assertFalse(nueva.info["previous_session_restart"])
        self.assertIsNone(nueva.previous)                   # la cadena se corta
        vieja.close()
        nueva.close()

    def test_close_without_audio_keeps_summary_without_wav(self):
        s = self.session()
        s.close()
        self.assertEqual(list(Path(self.temp.name).glob("*.wav")), [])
        summary = json.loads(next(Path(self.temp.name).glob("sesion_*_" + IDENTITY + ".json")).read_text())
        self.assertEqual(summary["messages"], 0)


class SocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_websocket_session_with_acks_and_reconnect(self):
        ws_audio_server.LAST_SESSION.clear()
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: handle(ws, output), "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello())
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    acks = []
                    for i in range(ACK_EVERY * 2):
                        await ws.send(packet(i))
                    for _ in range(2):
                        acks.append(json.loads(await asyncio.wait_for(ws.recv(), 5)))
                    self.assertEqual([a["index"] for a in acks],
                                     [ACK_EVERY - 1, ACK_EVERY * 2 - 1])
                # Reconecta con otra sesion que sigue 3 slots despues: el servidor mide el hueco.
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello("fedcba9876543210"))
                    await ws.recv()
                    await ws.send(packet(0, seq=ACK_EVERY * 2 * 4 + 3, identity="fedcba9876543210"))
            summaries = [json.loads(p.read_text()) for p in Path(output).glob("sesion_*_????????????????.json")]
            summaries.sort(key=lambda s: s["id"])
            self.assertEqual([s["gap_from_previous_session_slots"] for s in summaries],
                             [None, 3])                     # 0123... es la primera sesion, fedc... la segunda


if __name__ == "__main__":
    unittest.main()
