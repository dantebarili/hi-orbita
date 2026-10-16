"""Regresiones de pausa, memoria acotada, ventanas y PLAY sobre sockets reales."""
import asyncio
import json
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from ws_audio_live import LiveControl, LiveProcessor, RollingWindows, detection_beep, handle_live
from ws_audio_protocol import HEADER
from ws_audio_session import Session
from ws_audio_speech import PLAY_HEADER

ID = "0123456789abcdef"
PCM = struct.pack("<2048h", *([300] * 2048))


def hello():
    return json.dumps(dict(type="hello", protocol="orbita-pcm-v2", id=ID,
                           rate=16000, channels=1, format="s16le", source="mic_l"))


def packet(index, skip=0):
    return HEADER.pack(b"ORBA", 1, 40, 0, int(ID, 16), index, index * 4 + skip,
                       0, 4, 2048, 0, 0, zlib.crc32(PCM)) + PCM


class FakeRecognizer:
    def __init__(self):
        self.calls = 0

    def transcribe_words(self, pcm):
        self.calls += 1
        return [dict(word=" Órbita", start=0.1, end=0.3)]


class BlockingRecognizer(FakeRecognizer):
    def __init__(self):
        super().__init__()
        self.started = threading.Event()
        self.release = threading.Event()

    def transcribe_words(self, pcm):
        self.started.set()
        if not self.release.wait(5):
            raise TimeoutError("test no libero la inferencia")
        return super().transcribe_words(pcm)


class LiveUnitTests(unittest.TestCase):
    def test_no_files_but_crc_sequence_acks_and_bounded_gaps(self):
        with tempfile.TemporaryDirectory() as output:
            session = Session(output, record=False)
            self.assertEqual(json.loads(session.process(hello()))["type"], "ready")
            for i in range(400):
                response = session.process(packet(i, skip=i))
                if (i + 1) % 8 == 0:
                    self.assertEqual(json.loads(response)["index"], i)
            self.assertEqual(len(session.gaps), 128)
            self.assertEqual(session.gap_count, 399)
            bad = bytearray(packet(400, skip=400))
            bad[-1] ^= 1
            with self.assertRaises(ValueError):
                session.process(bytes(bad))
            session.close()
            self.assertEqual(list(Path(output).iterdir()), [])

    def test_windows_exact_overlap_and_reset(self):
        windows = RollingWindows(1, 0.25)
        source = bytes(range(256)) * 400
        collected = []
        for offset in range(0, len(source), 4096):
            collected.extend(windows.add(source[offset:offset + 4096], offset // 2, 3))
        self.assertEqual(len(collected), 3)
        for index, window in enumerate(collected):
            self.assertEqual(window.pcm, source[index * 24000:index * 24000 + 32000])
            self.assertEqual(window.start_sample, index * 12000)
            self.assertEqual(window.generation, 3)
        windows.reset()
        self.assertEqual(list(windows.add(bytes(32000), 99999, 4))[0].start_sample, 99999)

    def test_beep_format_and_fade(self):
        beep = detection_beep()
        self.assertEqual(len(beep), 6400)
        samples = struct.unpack("<3200h", beep)
        self.assertEqual((samples[0], samples[-1]), (0, 0))
        self.assertLessEqual(max(map(abs, samples)), 12000)


class LiveAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_pause_discards_running_result_and_pending_and_partial(self):
        control = LiveControl(True)
        recognizer = BlockingRecognizer()
        beeps = []
        async def beep(): beeps.append(True)
        with tempfile.TemporaryDirectory() as output:
            processor = LiveProcessor(control, recognizer, output, ID, beep, 1, 0, True)
            try:
                processor.add(bytes(32000), 0)
                for _ in range(100):
                    if recognizer.started.is_set(): break
                    await asyncio.sleep(0.01)
                self.assertTrue(recognizer.started.is_set())
                processor.add(bytes(64000), 16000)
                control.set_enabled(False)
                self.assertTrue(processor.queue.empty())
                processor.add(bytes(32000), 48000)
                recognizer.release.set()
                await asyncio.wait_for(processor.queue.join(), 3)
                self.assertEqual(beeps, [])
                self.assertEqual(list(Path(output).iterdir()), [])
                control.set_enabled(True)
                processor.add(bytes(32000), 64000)
                await asyncio.wait_for(processor.queue.join(), 3)
                self.assertEqual(beeps, [True])
                self.assertEqual(len(list(Path(output).glob("*.wav"))), 1)
                self.assertEqual(len(list(Path(output).glob("*.jsonl"))), 1)
            finally:
                recognizer.release.set()
                await processor.close()
        self.assertEqual(len(control.listeners), 0)

    async def test_queue_bounded_when_cpu_is_slow(self):
        control = LiveControl(True)
        recognizer = BlockingRecognizer()
        async def beep(): pass
        with tempfile.TemporaryDirectory() as output:
            processor = LiveProcessor(control, recognizer, output, ID, beep, 1, 0)
            processor.add(bytes(32000), 0)
            await asyncio.sleep(0.05)
            try:
                for i in range(20): processor.add(bytes(32000), (i + 1) * 16000)
                self.assertEqual(processor.queue.qsize(), 2)
                self.assertEqual(processor.dropped, 18)
                control.set_enabled(False)
            finally:
                recognizer.release.set()
                await processor.close()
                if control.inference: await asyncio.shield(control.inference)

    async def test_overlap_words_not_printed_or_detected_twice(self):
        class OverlapRecognizer:
            calls = 0
            def transcribe_words(self, pcm):
                self.calls += 1
                # La misma palabra absoluta a 0.9 s en las dos ventanas.
                end = 0.9 if self.calls == 1 else 0.15
                return [dict(word=" Órbita", start=max(0, end - 0.1), end=end)]
        control = LiveControl(True)
        beeps = []
        async def beep(): beeps.append(1)
        with tempfile.TemporaryDirectory() as output:
            processor = LiveProcessor(control, OverlapRecognizer(), output, ID, beep, 1, 0.25)
            try:
                processor.add(bytes(32000), 0)
                await asyncio.wait_for(processor.queue.join(), 3)
                processor.add(bytes(24000), 16000)
                await asyncio.wait_for(processor.queue.join(), 3)
                self.assertEqual(beeps, [1])
                records = [json.loads(line) for line in next(Path(output).glob("*.jsonl")).read_text(encoding="utf-8").splitlines()]
                self.assertEqual(len(records), 1)
                self.assertEqual(list(Path(output).glob("*.wav")), [])
            finally:
                await processor.close()

    async def test_real_socket_ack_while_paused_then_beep_and_resume_same_connection(self):
        control = LiveControl()
        recognizer = FakeRecognizer()
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: handle_live(ws, output, control, recognizer, 1, 0), "127.0.0.1", 0) as server:
                uri = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}"
                async with connect(uri) as ws:
                    await ws.send(hello())
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    for i in range(16): await ws.send(packet(i))
                    for index in (7, 15):
                        self.assertEqual(json.loads(await asyncio.wait_for(ws.recv(), 3))["index"], index)
                    self.assertEqual(recognizer.calls, 0)
                    self.assertEqual(list(Path(output).iterdir()), [])
                    control.command("g")
                    for i in range(16, 24): await ws.send(packet(i))
                    begin = None
                    pcm = bytearray()
                    got_ack = ended = False
                    while not (got_ack and ended):
                        msg = await asyncio.wait_for(ws.recv(), 3)
                        if isinstance(msg, bytes):
                            self.assertLessEqual(len(msg), 4096)
                            pcm.extend(msg[PLAY_HEADER.size:])
                        else:
                            obj = json.loads(msg)
                            if obj["type"] == "play": begin = obj
                            elif obj["type"] == "play_end": ended = True
                            elif obj["type"] == "ack": got_ack = obj["index"] == 23
                    self.assertEqual(bytes(pcm), detection_beep())
                    await ws.send(json.dumps(dict(type="play_done", play_id=begin["play_id"], reason="end", played_ms=200)))
                    control.command("p")
                    for i in range(24, 32): await ws.send(packet(i))
                    self.assertEqual(json.loads(await asyncio.wait_for(ws.recv(), 3))["index"], 31)
                    self.assertIsNotNone(control.connection)
                    # El firmware sigue conectado y puede estar en silencio: no hay timeout RX de 30 s.
                    self.assertEqual(list(Path(output).glob("sesion_*.wav")), [])
            self.assertIsNone(control.connection)

    async def test_real_socket_ack_while_inference_is_blocked_and_invalid_crc_rejected(self):
        control = LiveControl(True)
        recognizer = BlockingRecognizer()
        with tempfile.TemporaryDirectory() as output:
            async with serve(lambda ws: handle_live(ws, output, control, recognizer, 1, 0), "127.0.0.1", 0) as server:
                uri = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}"
                try:
                    async with connect(uri) as ws:
                        await ws.send(hello())
                        await ws.recv()
                        for i in range(16): await ws.send(packet(i))
                        for index in (7, 15):
                            self.assertEqual(json.loads(await asyncio.wait_for(ws.recv(), 2))["index"], index)
                        for _ in range(100):
                            if recognizer.started.is_set(): break
                            await asyncio.sleep(0.01)
                        self.assertTrue(recognizer.started.is_set())
                        control.command("p")
                        recognizer.release.set()
                        await asyncio.sleep(0.05)
                        bad = bytearray(packet(16))
                        bad[-1] ^= 1
                        await ws.send(bytes(bad))
                        from websockets.exceptions import ConnectionClosed
                        with self.assertRaises(ConnectionClosed):
                            await asyncio.wait_for(ws.recv(), 2)
                        self.assertEqual(ws.close_code, 1008)
                finally:
                    recognizer.release.set()
            self.assertEqual(list(Path(output).iterdir()), [])


if __name__ == "__main__":
    unittest.main()


class KeywordRegressionTests(unittest.TestCase):
    def test_configurable_word_matches_whole_word(self):
        from ws_audio_speech import contains_keyword
        self.assertTrue(contains_keyword("El GATO está acá", "gato"))
        self.assertFalse(contains_keyword("Tengo gatos", "gato"))
        self.assertFalse(contains_keyword("órbita", "gato"))
        self.assertTrue(contains_keyword("¡ÓRBITA!", "orbita"))

    def test_recognizer_converts_numpy_timestamps_to_python_float(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy opcional: instalar requirements-speech para esta regresion")
        from types import SimpleNamespace
        from ws_audio_speech import FasterWhisperRecognizer
        recognizer = FasterWhisperRecognizer()
        word = SimpleNamespace(word=" gato", start=np.float32(.1), end=np.float32(.3))
        recognizer._segments = lambda *args, **kwargs: [SimpleNamespace(words=[word])]
        result = recognizer.transcribe_words(bytes(32000))
        self.assertIs(type(result[0]["start"]), float)
        self.assertIs(type(result[0]["end"]), float)
        json.dumps(result)


class KeywordAsyncRegressionTests(unittest.IsolatedAsyncioTestCase):
    async def test_gato_detection_with_numpy_timestamps_saves_valid_json(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy opcional: instalar requirements-speech para esta regresion")
        class NumpyRecognizer:
            def transcribe_words(self, pcm):
                return [dict(word=" gato", start=np.float32(.1), end=np.float32(.3))]
        control = LiveControl(True)
        beeps = []
        async def beep(): beeps.append(True)
        with tempfile.TemporaryDirectory() as output:
            processor = LiveProcessor(control, NumpyRecognizer(), output, ID, beep, 1, 0, True, "gato")
            try:
                processor.add(bytes(32000), 0)
                await asyncio.wait_for(processor.queue.join(), 3)
                self.assertEqual(beeps, [True])
                record = json.loads(next(Path(output).glob("*.jsonl")).read_text(encoding="utf-8"))
                self.assertIs(record["detected"], True)
                self.assertIs(record["orbita"], False)
                self.assertEqual(record["keyword"], "gato")
            finally:
                await processor.close()
