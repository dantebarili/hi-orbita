import asyncio
import csv
import json
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

import servidor_wakeword
from servidor_wakeword import WakeConversation, handle
from test_ws_audio_cloud import tone
from ws_audio_protocol import HEADER
from ws_audio_speech import SAMPLE_RATE

PREROLL = 3 * SAMPLE_RATE


def fake_services(textos):
    textos = list(textos)

    class Services:
        llm_name = "fake"
        save_audio = False

        class llm:
            preguntas = []

            @staticmethod
            def reply(text):
                Services.llm.preguntas.append(text)
                return "respuesta", 1, 1

        class speech:
            name = "fake"
            transcribe = staticmethod(lambda pcm: textos.pop(0) if textos else "")
            speak = staticmethod(lambda text: tone(0.3, 5000))
    return Services


class WakeConversationTests(unittest.IsolatedAsyncioTestCase):
    async def test_activacion_pregunta_y_fin(self):
        with tempfile.TemporaryDirectory() as directory:
            services = fake_services(["Órbita.", "Órbita, ¿qué hora es?"])
            play_done = asyncio.Event()

            async def send_pcm(pcm):
                play_done.set()

            conv = WakeConversation(services, "sess", send_pcm, play_done, PREROLL, Path(directory))
            clock = [100.0]
            conv.clock = lambda: clock[0]
            conv.last_activity = clock[0]

            def live(pcm):
                step = SAMPLE_RATE // 10 * 2
                for i in range(0, len(pcm), step):
                    clock[0] += 0.1
                    conv.feed(pcm[i:i + step])

            async def turno():
                if conv.task is not None:
                    await conv.task

            # Pre-roll (llega de golpe): charla vieja + "Órbita" en el ultimo 1,5 s.
            conv.feed(tone(1.5, 3000) + tone(0.8, 20) + tone(0.5, 3000) + tone(0.2, 20))
            live(tone(1.5, 20))                       # silencio: cierra "Órbita" sola
            await turno()
            self.assertEqual(services.llm.preguntas, [])        # solo la wake word: no va al LLM
            self.assertIsNotNone(conv.clip_file)
            with wave.open(str(conv.clip_file)) as w:
                self.assertEqual(w.getnframes(), 4 * SAMPLE_RATE)   # 3 s antes + 1 s despues
            live(tone(0.6, 20))                        # silencio tras el turno
            live(tone(1, 3000))                        # la pregunta
            live(tone(1.5, 20))
            await turno()
            self.assertEqual(services.llm.preguntas, ["¿qué hora es?"])
            self.assertFalse(conv.finished)
            live(tone(servidor_wakeword.IDLE_END_S + 1, 20))   # nadie habla: fin
            self.assertTrue(conv.finished)
            conv.write_row("prueba")
            with (Path(directory) / "activaciones.csv").open(encoding="utf-8") as file:
                filas = list(csv.DictReader(file))
            self.assertEqual(filas[0]["transcripcion"], "Órbita.")
            self.assertEqual(filas[0]["etiqueta"], "")

    async def test_falsa_activacion_termina_sola(self):
        with tempfile.TemporaryDirectory() as directory:
            conv = WakeConversation(fake_services([]), "sess", None, asyncio.Event(), PREROLL, Path(directory))
            clock = [100.0]
            conv.clock = lambda: clock[0]
            conv.last_activity = clock[0]
            conv.feed(tone(3, 20))
            for _ in range(int(servidor_wakeword.IDLE_END_S * 10) + 5):
                clock[0] += 0.1
                conv.feed(tone(0.1, 20))
            self.assertTrue(conv.finished)
            self.assertIsNotNone(conv.clip_file)


class ServidorTests(unittest.IsolatedAsyncioTestCase):
    async def test_wake_y_conversation_end_por_websocket(self):
        old = servidor_wakeword.IDLE_END_S
        servidor_wakeword.IDLE_END_S = 0.5
        self.addCleanup(setattr, servidor_wakeword, "IDLE_END_S", old)
        with tempfile.TemporaryDirectory() as directory:
            services = fake_services([])
            async with serve(lambda ws: handle(ws, services, Path(directory)), "127.0.0.1", 0) as server:
                port = server.sockets[0].getsockname()[1]
                async with connect(f"ws://127.0.0.1:{port}") as ws:
                    await ws.send(json.dumps(dict(type="hello", protocol="orbita-pcm-v2", id="0123456789abcdef",
                                                  rate=16000, channels=1, format="s16le", source="mic_lr")))
                    self.assertEqual(json.loads(await ws.recv())["type"], "ready")
                    await ws.send(json.dumps(dict(type="wake", index=0, seq=0, preroll_samples=PREROLL)))
                    pcm = tone(0.128, 20)
                    end = None
                    for index in range(80):                  # ~10 s de silencio a ritmo real acelerado
                        header = HEADER.pack(b"ORBA", 1, 40, 0, 0x0123456789abcdef, index, index * 4, 0, 4,
                                             2048, 0, 0, zlib.crc32(pcm))
                        await ws.send(header + pcm)
                        await asyncio.sleep(0.02)
                        try:
                            while True:
                                obj = json.loads(await asyncio.wait_for(ws.recv(), 0.001))
                                if obj.get("type") == "conversation_end":
                                    end = obj
                        except asyncio.TimeoutError:
                            pass
                        if end:
                            break
                    self.assertIsNotNone(end)
                    self.assertRegex(end["id"], r"^[0-9a-f]{16}$")
            self.assertTrue((Path(directory) / "activaciones.csv").exists())


if __name__ == "__main__":
    unittest.main()
