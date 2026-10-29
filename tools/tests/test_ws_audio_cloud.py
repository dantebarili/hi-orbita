import asyncio
import json
import tempfile
import unittest

import numpy as np

from types import SimpleNamespace

from ws_audio_cloud import SAMPLE_RATE, CloudServices, Conversation


def rumble(seconds, amplitude):
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    return (amplitude * np.sin(2 * np.pi * 60 * t)).astype("<i2").tobytes()


def tone(seconds, amplitude):
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    return (amplitude * np.sin(2 * np.pi * 300 * t)).astype("<i2").tobytes()


class FakeServices:
    llm_name = "fake"
    save_audio = False

    class llm:
        reply = staticmethod(lambda text: ("respuesta " + text, 10, 5))

    class speech:
        name = "fake"
        transcribe = staticmethod(lambda pcm: "hola orbita")
        speak = staticmethod(lambda text: tone(0.5, 5000))


class ConversationTests(unittest.IsolatedAsyncioTestCase):
    async def test_turn_inside_conversation(self):
        with tempfile.TemporaryDirectory() as directory:
            sent = []
            play_done = asyncio.Event()

            async def send_pcm(pcm):
                sent.append(pcm)
                play_done.set()

            conv = Conversation(FakeServices, directory, "sess", send_pcm, play_done)
            clock = [100.0]
            conv.clock = lambda: clock[0]

            def live(pcm):                               # llega en tiempo real: un paquete de 0.1 s cada 0.1 s
                step = SAMPLE_RATE // 10 * 2
                for i in range(0, len(pcm), step):
                    clock[0] += 0.1
                    conv.feed(pcm[i:i + step])

            live(tone(2, 20))                            # ruido de fondo: arma el piso de ruido
            self.assertEqual(conv.state, "idle")
            live(rumble(2, 1000))                        # rumble de 60 Hz (en bruto 700 de RMS, 35x el ruido): no es voz
            self.assertEqual(conv.state, "idle")
            live(tone(1, 20))
            live(tone(1, 3000))                          # empieza a hablar
            self.assertEqual(conv.state, "capturing")
            live(tone(1.5, 20))                          # silencio: cierra la frase
            self.assertEqual(conv.state, "busy")
            await conv.task
            self.assertEqual(len(sent), 1)
            self.assertEqual(conv.state, "idle")
            record = json.loads(next(__import__("pathlib").Path(directory).glob("cloud_*.jsonl")).read_text())
            self.assertEqual(record["answer"], "respuesta hola orbita")
            self.assertAlmostEqual(record["utterance_seconds"], 2.1, delta=0.3)   # 1 s de voz + 0.5 s antes + 0.5 s después
            # La cola del parlante que llega justo después (1 s de reloj) no dispara otro turno.
            live(tone(0.5, 3000))
            self.assertEqual(conv.state, "idle")
            live(tone(1.5, 20))                          # silencio recibido: vuelve a escuchar
            live(tone(1, 3000))                          # pasado el guard, una voz nueva sí se toma
            self.assertEqual(conv.state, "capturing")

    def test_services_validate_config(self):
        cfg = SimpleNamespace(SPEECH="deepgram", DEEPGRAM_API_KEY="", DEEPGRAM_STT_MODEL="n", DEEPGRAM_VOICE="v",
                              LLM="gemini", GEMINI_API_KEY="g", GEMINI_MODEL="m", CLAUDE_MODEL="c", SYSTEM_PROMPT="p")
        with self.assertRaises(ValueError):
            CloudServices(cfg)                           # falta la clave de Deepgram
        cfg.DEEPGRAM_API_KEY = "d"
        self.assertEqual(CloudServices(cfg).llm_name, "gemini:m")
        cfg.LLM = "claude"
        with self.assertRaises(ValueError):
            CloudServices(cfg)                           # falta ANTHROPIC_API_KEY


if __name__ == "__main__":
    unittest.main()
