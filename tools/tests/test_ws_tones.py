import asyncio
import json
from pathlib import Path
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from ws_audio_server import handle, LAST_SESSION
from ws_audio_speech import diagnostic_tones_pcm, PLAY_HEADER
from test_ws_session import hello, packet


class TonePatternTests(unittest.TestCase):
    def test_three_seconds_of_tone_with_two_seconds_of_silence(self):
        pcm = diagnostic_tones_pcm()
        self.assertEqual(len(pcm), 160000)
        self.assertEqual(pcm[:32000], pcm[64000:96000])
        self.assertEqual(pcm[:32000], pcm[128000:])
        self.assertNotEqual(pcm[:32000], bytes(32000))
        self.assertEqual(pcm[32000:64000], bytes(32000))
        self.assertEqual(pcm[96000:128000], bytes(32000))


class ToneSocketTests(unittest.IsolatedAsyncioTestCase):
    async def test_tones_and_simultaneous_uplink_saved_without_stt(self):
        LAST_SESSION.clear()
        pcm = diagnostic_tones_pcm()
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: handle(ws, output, play_pcm=pcm), "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello())
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    for i in range(16): await ws.send(packet(i))
                    received = bytearray()
                    acks = []
                    ended = False
                    play_id = None
                    while not ended or len(acks) != 2:
                        frame = await asyncio.wait_for(ws.recv(), 3)
                        if isinstance(frame, bytes):
                            self.assertLessEqual(len(frame), 4096)
                            received.extend(frame[PLAY_HEADER.size:])
                        else:
                            obj = json.loads(frame)
                            if obj["type"] == "play": play_id = obj["play_id"]
                            elif obj["type"] == "play_end": ended = True
                            elif obj["type"] == "ack": acks.append(obj["index"])
                    self.assertEqual(bytes(received), pcm)
                    self.assertEqual(acks, [7, 15])
                    await ws.send(json.dumps(dict(type="play_done", play_id=play_id, reason="end", played_ms=5000)))
                    for i in range(16, 24): await ws.send(packet(i))
                    self.assertEqual(json.loads(await asyncio.wait_for(ws.recv(), 3))["index"], 23)
            with wave.open(str(next(Path(output).glob("*.wav")))) as wav:
                self.assertEqual(wav.getnframes(), 24 * 2048)


class PeriodicToneTests(unittest.IsolatedAsyncioTestCase):
    async def test_periodic_replies_wait_for_done_and_keep_uplink(self):
        from ws_audio_speech import periodic_tones_pcm
        pcm = periodic_tones_pcm()
        self.assertEqual(len(pcm), 40000)
        self.assertEqual(pcm[8000:16000], bytes(8000))
        self.assertEqual(pcm[24000:32000], bytes(8000))
        LAST_SESSION.clear()
        with tempfile.TemporaryDirectory() as output:
            # Intervalo reducido solo en el test, para no esperar cinco segundos por tanda.
            async with serve(lambda ws: handle(ws, output, play_pcm=pcm, tone_interval=.1), "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(hello())
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    for i in range(8): await ws.send(packet(i))
                    acked = False
                    ids = []
                    for round_index in range(2):
                        got = bytearray()
                        while True:
                            frame = await asyncio.wait_for(ws.recv(), 3)
                            if isinstance(frame, bytes):
                                got.extend(frame[PLAY_HEADER.size:])
                            else:
                                obj = json.loads(frame)
                                if obj["type"] == "ack": acked = obj["index"] == 7
                                elif obj["type"] == "play": ids.append(obj["play_id"])
                                elif obj["type"] == "play_end": break
                        self.assertEqual(bytes(got), pcm)
                        if round_index == 0:
                            self.assertTrue(acked)
                            # Sin DONE no se manda otra respuesta, aunque venzan dos intervalos.
                            with self.assertRaises(asyncio.TimeoutError):
                                await asyncio.wait_for(ws.recv(), .25)
                        await ws.send(json.dumps(dict(type="play_done", play_id=ids[-1], reason="end", played_ms=1250)))
                    self.assertNotEqual(ids[0], ids[1])
                    for i in range(8, 16): await ws.send(packet(i))
                    # Puede empezar otra tanda al mismo tiempo: esperar el ACK sin bloquear RX.
                    while True:
                        frame = await asyncio.wait_for(ws.recv(), 3)
                        if isinstance(frame, str) and json.loads(frame).get("index") == 15:
                            break
            with wave.open(str(next(Path(output).glob("*.wav")))) as wav:
                self.assertEqual(wav.getnframes(), 16 * 2048)


class DownlinkOnlyTests(unittest.IsolatedAsyncioTestCase):
    async def test_periodic_audio_without_hello_or_uplink(self):
        from ws_audio_server import handle_downlink_tones
        from ws_audio_speech import periodic_tones_pcm
        async with serve(lambda ws: handle_downlink_tones(ws, interval=.1), "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            async with connect(f"ws://127.0.0.1:{port}") as ws:
                # No se envia hello, PCM ni ningun otro mensaje al conectar.
                ids = []
                for round_index in range(2):
                    pcm = bytearray()
                    while True:
                        frame = await asyncio.wait_for(ws.recv(), 3)
                        if isinstance(frame, bytes):
                            self.assertLessEqual(len(frame), 4096)
                            pcm.extend(frame[PLAY_HEADER.size:])
                        else:
                            obj = json.loads(frame)
                            if obj["type"] == "play": ids.append(obj["play_id"])
                            elif obj["type"] == "play_end": break
                    self.assertEqual(bytes(pcm), periodic_tones_pcm())
                    if round_index == 0:
                        with self.assertRaises(asyncio.TimeoutError):
                            await asyncio.wait_for(ws.recv(), .25)
                    await ws.send(json.dumps(dict(type="play_done", play_id=ids[-1], reason="end", played_ms=1250)))
                self.assertNotEqual(ids[0], ids[1])
