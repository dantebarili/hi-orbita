import unittest

from ws_audio_speech import PLAY_HEADER, play_frames


class SpeechHelpersTests(unittest.TestCase):
    def test_play_framing(self):
        if True:
            pcm = bytes(range(256)) * 20
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


if __name__ == "__main__":
    unittest.main()
