import tempfile
import unittest
import wave
from pathlib import Path

from ws_audio_speech import AudioWindow, PLAY_HEADER, contains_orbita, load_reply_wav, play_frames


class SpeechHelpersTests(unittest.TestCase):
    def test_keyword_match_ignores_accent_case_and_punctuation(self):
        self.assertTrue(contains_orbita("¡ÓRBITA, escuchame!"))
        self.assertTrue(contains_orbita("dije orbita"))
        self.assertFalse(contains_orbita("orbitando"))
        self.assertFalse(contains_orbita("orbitas"))

    def test_fixed_window_collects_exact_bytes_across_packets(self):
        window = AudioWindow(1)
        self.assertIsNone(window.add(b"a" * 12_345))
        result = window.add(b"b" * 30_000)
        self.assertEqual(len(result), 32_000)
        self.assertEqual(result[:12_345], b"a" * 12_345)
        self.assertEqual(result[12_345:], b"b" * (32_000 - 12_345))
        self.assertTrue(window.complete)

    def test_reply_wav_format_and_play_framing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reply.wav"
            pcm = bytes(range(256)) * 20
            with wave.open(str(path), "wb") as wav:
                wav.setparams((1, 2, 16_000, 0, "NONE", "not compressed"))
                wav.writeframes(pcm)
            self.assertEqual(load_reply_wav(path), pcm)

            frames = list(play_frames(0x0123456789ABCDEF, pcm))
            self.assertEqual(frames[0]["type"], "play")
            self.assertEqual(frames[0]["play_id"], "0123456789abcdef")
            binary_frames = frames[1:-1]
            received = bytearray()
            for expected_seq, binary in enumerate(binary_frames):
                magic, version, header_size, flags, play_id, seq, length = PLAY_HEADER.unpack_from(binary)
                self.assertEqual((magic, version, header_size, flags, play_id, seq),
                                 (b"PLAY", 1, 24, 0, 0x0123456789ABCDEF, expected_seq))
                self.assertLessEqual(len(binary), 4096)
                self.assertEqual(length, len(binary) - PLAY_HEADER.size)
                received.extend(binary[PLAY_HEADER.size:])
            self.assertEqual(bytes(received), pcm)
            self.assertEqual(frames[-1], {"type": "play_end", "play_id": "0123456789abcdef"})

    def test_reply_wav_rejects_wrong_format(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "wrong.wav"
            with wave.open(str(path), "wb") as wav:
                wav.setparams((2, 2, 16_000, 0, "NONE", "not compressed"))
                wav.writeframes(b"\0\0\0\0")
            with self.assertRaises(ValueError):
                load_reply_wav(path)


if __name__ == "__main__":
    unittest.main()
