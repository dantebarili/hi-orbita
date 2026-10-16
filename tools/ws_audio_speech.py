"""Helpers for the optional, one-window wake-word demo on the local server."""
from __future__ import annotations

import re
import math
import unicodedata
import wave
from pathlib import Path
from struct import Struct, pack

SAMPLE_RATE = 16_000
MAX_PLAY_SECONDS = 15
MAX_WS_BYTES = 4096
PLAY_HEADER = Struct("<4sBBHQII")
assert PLAY_HEADER.size == 24


def diagnostic_tones_pcm() -> bytes:
    """Tres tonos de 1 kHz y 1 s, separados por 1 s de silencio; pico 16000."""
    cycle = (0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
             0, -6123, -11314, -14782, -16000, -14782, -11314, -6123)
    tone = pack("<16h", *cycle) * 1000
    silence = bytes(SAMPLE_RATE * 2)
    return tone + silence + tone + silence + tone


def periodic_tones_pcm() -> bytes:
    """Tres pitidos de 250 ms separados por 250 ms; total 1,25 s."""
    tone = diagnostic_tones_pcm()[:SAMPLE_RATE // 4 * 2]
    silence = bytes(SAMPLE_RATE // 4 * 2)
    return tone + silence + tone + silence + tone


def contains_keyword(transcript: str, keyword: str) -> bool:
    """Palabra/frase completa, ignorando mayusculas y tildes; sin coincidencias difusas."""
    def normalize(value):
        value = unicodedata.normalize("NFKD", value.casefold())
        return "".join(c for c in value if not unicodedata.combining(c))
    keyword = normalize(keyword.strip())
    if not keyword:
        return False
    return re.search(r"(?<![\w])" + re.escape(keyword) + r"(?![\w])",
                     normalize(transcript), flags=re.UNICODE) is not None


def contains_orbita(transcript: str) -> bool:
    return contains_keyword(transcript, "orbita")


class AudioWindow:
    """Collect one exact PCM window, optionally skipping an initial interval."""

    def __init__(self, seconds: float, start_seconds: float = 0):
        if not 1 <= seconds <= 30:
            raise ValueError("la ventana de audio debe durar entre 1 y 30 segundos")
        if not math.isfinite(start_seconds) or start_seconds < 0:
            raise ValueError("el inicio debe ser un numero finito mayor o igual a cero")
        self.target_bytes = round(seconds * SAMPLE_RATE) * 2
        self._skip_bytes = round(start_seconds * SAMPLE_RATE) * 2
        self._pcm = bytearray()

    @property
    def complete(self) -> bool:
        return len(self._pcm) >= self.target_bytes

    def add(self, pcm: bytes) -> bytes | None:
        if self.complete:
            return None
        if self._skip_bytes:
            skipped = min(self._skip_bytes, len(pcm))
            self._skip_bytes -= skipped
            pcm = pcm[skipped:]
        remaining = self.target_bytes - len(self._pcm)
        self._pcm.extend(pcm[:remaining])
        if self.complete:
            return bytes(self._pcm)
        return None


class FasterWhisperRecognizer:
    """Lazy-load the small Spanish model so importing the server stays lightweight."""

    def __init__(self, model_size: str = "tiny"):
        self.model_size = model_size
        self._model = None

    def _segments(self, pcm: bytes, words=False):
        import numpy as np
        from faster_whisper import WhisperModel

        if self._model is None:
            self._model = WhisperModel(self.model_size, device="cpu", compute_type="int8")
        audio = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        segments, _ = self._model.transcribe(
            audio,
            language="es",
            beam_size=1,
            condition_on_previous_text=False,
            vad_filter=words,
            word_timestamps=words,
        )
        return list(segments)

    def transcribe(self, pcm: bytes) -> str:
        return " ".join(segment.text.strip() for segment in self._segments(pcm)).strip()

    def transcribe_words(self, pcm: bytes):
        """Timestamps de palabras para no repetir el texto del solapamiento."""
        return [dict(word=word.word, start=float(word.start), end=float(word.end))
                for segment in self._segments(pcm, words=True) for word in (segment.words or [])]


def load_reply_wav(path: Path) -> bytes:
    """Return raw PCM from a mono, 16 kHz, signed 16-bit WAV clip."""
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels() != 1 or wav.getsampwidth() != 2 or
                wav.getframerate() != SAMPLE_RATE or wav.getcomptype() != "NONE"):
            raise ValueError("el WAV de respuesta debe ser PCM mono, 16 kHz y 16-bit")
        if not 0 < wav.getnframes() <= SAMPLE_RATE * MAX_PLAY_SECONDS:
            raise ValueError("el WAV de respuesta debe durar entre 0 y 15 segundos")
        pcm = wav.readframes(wav.getnframes())
    if not pcm or len(pcm) % 2:
        raise ValueError("el WAV no contiene PCM s16le valido")
    return pcm


def play_frames(play_id: int, pcm: bytes):
    """Yield the JSON controls and PLAY binary frames expected by the ESP."""
    if not pcm or len(pcm) % 2 or len(pcm) > SAMPLE_RATE * 2 * MAX_PLAY_SECONDS:
        raise ValueError("PCM de respuesta invalido o mayor a 15 segundos")
    identity = f"{play_id:016x}"
    yield {
        "type": "play",
        "play_id": identity,
        "rate": SAMPLE_RATE,
        "channels": 1,
        "format": "s16le",
        "bytes": len(pcm),
    }
    max_pcm = MAX_WS_BYTES - PLAY_HEADER.size
    max_pcm -= max_pcm % 2
    for seq, offset in enumerate(range(0, len(pcm), max_pcm)):
        chunk = pcm[offset:offset + max_pcm]
        header = PLAY_HEADER.pack(b"PLAY", 1, PLAY_HEADER.size, 0,
                                  play_id, seq, len(chunk))
        yield header + chunk
    yield {"type": "play_end", "play_id": identity}
