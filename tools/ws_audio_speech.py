"""Helpers for the optional, one-window wake-word demo on the local server."""
from __future__ import annotations

import re
import math
import unicodedata
import wave
from pathlib import Path
from array import array
from struct import Struct

SAMPLE_RATE = 16_000
MAX_PLAY_SECONDS = 15
MAX_WS_BYTES = 4096
PLAY_HEADER = Struct("<4sBBHQII")
assert PLAY_HEADER.size == 24


def _highpass(samples, cutoff: float = 250.0):
    """Pasa-altos Butterworth de 4to orden (dos biquads) sobre una lista de numeros; devuelve floats.

    Saca el retumbo grave del mic crudo (sin AFE) y lo que un parlante chico no reproduce: sin esto la
    ganancia se gasta subiendo graves inaudibles y la voz casi no sube.
    """
    out = [float(x) for x in samples]
    k = math.tan(math.pi * cutoff / SAMPLE_RATE)
    for q in (0.5412, 1.3066):                      # polos del Butterworth de 4to orden
        norm = 1 / (1 + k / q + k * k)
        b0, b1, b2 = norm, -2 * norm, norm
        a1, a2 = 2 * (k * k - 1) * norm, (1 - k / q + k * k) * norm
        x1 = x2 = y1 = y2 = 0.0
        for i, x in enumerate(out):
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out[i] = y
    return out


def normalize_pcm(pcm: bytes, target_rms: int = 8_000, max_gain: float = 60.0, ceiling: int = 30_000) -> bytes:
    """Prepara un PCM s16le mono para el parlante: saca graves y sube el nivel promedio (RMS).

    1) Pasa-altos a 250 Hz (ver _highpass). 2) Ganancia que lleva el RMS de los tramos con voz (bloques de
    100 ms hasta 25 dB bajo el mas fuerte) a target_rms (~-12 dBFS; el tono de prueba esta en -9): normalizar
    por pico no alcanza, la voz tiene picos mucho mas altos que su promedio. 3) Limitador suave (tanh) que
    nunca pasa de `ceiling`, en vez de recorte duro. Solo amplifica; max_gain acota la subida para no inflar
    un tramo que es solo ruido.
    """
    samples = array("h")
    samples.frombytes(pcm)
    filtered = _highpass(samples)
    block = SAMPLE_RATE // 10
    rms = [math.sqrt(sum(x * x for x in filtered[i:i + block]) / block)
           for i in range(0, len(filtered) - block + 1, block)]
    loudest = max(rms, default=0)
    if loudest == 0:
        return pcm
    active = [r * r for r in rms if r >= loudest * 10 ** (-25 / 20)]
    gain = min(target_rms / math.sqrt(sum(active) / len(active)), max_gain)
    # tanh: lineal para valores chicos, se aplana hacia `ceiling` en vez de recortar.
    return array("h", (int(ceiling * math.tanh(x * gain / ceiling)) for x in filtered)).tobytes()


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
