import asyncio
import json
import tempfile
import unittest

import numpy as np

from ws_audio_cloud import SAMPLE_RATE, CloudServices, Conversation


def tone(seconds, amplitude):
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    return (amplitude * np.sin(2 * np.pi * 300 * t)).astype("<i2").tobytes()


class FakeServices:
    local_recognizer = None
    stt = staticmethod(lambda pcm: "hola orbita")

    class claude:
        reply = staticmethod(lambda text: ("respuesta " + text, 10, 5))

    class eleven:
        speak = staticmethod(lambda text: tone(0.5, 5000))


class ConversationTests(unittest.IsolatedAsyncioTestCase):
    async def test_turn_captures_until_silence_and_replies(self):
        with tempfile.TemporaryDirectory() as directory:
            sent = []
            play_done = asyncio.Event()

            async def send_pcm(pcm):
                sent.append(pcm)
                play_done.set()

            conv = Conversation(FakeServices, directory, "sess", send_pcm, play_done, False)
            # 3 s de ruido bajo, 1 s de frase y 1.5 s de silencio: la frase ya terminó cuando "Whisper" detecta.
            conv.feed(tone(3, 20) + tone(1, 3000) + tone(1.5, 20), 0)
            await conv.on_detect(3.0)
            self.assertEqual(conv.state, "busy")
            await conv.task
            self.assertEqual(len(sent), 1)
            self.assertEqual(conv.state, "idle")
            record = json.loads(next(__import__("pathlib").Path(directory).glob("cloud_*.jsonl")).read_text())
            self.assertEqual(record["answer"], "respuesta hola orbita")
            self.assertAlmostEqual(record["utterance_seconds"], 1.2, delta=0.3)
            # El eco del parlante justo después no dispara otro turno.
            await conv.on_detect(conv.latest / SAMPLE_RATE - 0.5)
            self.assertEqual(conv.state, "idle")

    async def test_aborts_when_nobody_speaks(self):
        with tempfile.TemporaryDirectory() as directory:
            conv = Conversation(FakeServices, directory, "s", None, asyncio.Event(), False)
            conv.feed(tone(2, 20), 0)
            await conv.on_detect(1.0)
            self.assertEqual(conv.state, "capturing")
            conv.feed(tone(4.5, 20), 0)
            self.assertEqual(conv.state, "idle")

    def test_missing_keys_rejected(self):
        with self.assertRaises(ValueError):
            CloudServices({"ELEVENLABS_API_KEY": "x"}, "m", "v", "t", "s")


if __name__ == "__main__":
    unittest.main()
