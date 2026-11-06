"""Respuesta de voz hacia la ESP: arma los mensajes `play` + binarios PLAY + `play_end`."""
from struct import Struct

SAMPLE_RATE = 16_000
MAX_PLAY_SECONDS = 15
MAX_WS_BYTES = 4096
PLAY_HEADER = Struct("<4sBBHQII")
assert PLAY_HEADER.size == 24
MAX_FRAME_PCM = (MAX_WS_BYTES - PLAY_HEADER.size) // 2 * 2


def play_begin(play_id: int, total_bytes=None):
    """`play` de apertura. Sin `bytes` (TTS en streaming: no se sabe el total) la ESP lo acepta igual
    (orbita_ws_downlink.h: has_total_bytes = false)."""
    msg = {"type": "play", "play_id": f"{play_id:016x}", "rate": SAMPLE_RATE, "channels": 1, "format": "s16le"}
    if total_bytes is not None:
        msg["bytes"] = total_bytes
    return msg


def play_data(play_id: int, first_seq: int, pcm: bytes):
    """Binarios PLAY de un pedazo de PCM (cortado en frames de hasta 4 KB). `seq` sigue desde first_seq."""
    if not pcm or len(pcm) % 2:
        raise ValueError("PCM de respuesta invalido")
    for i, offset in enumerate(range(0, len(pcm), MAX_FRAME_PCM)):
        chunk = pcm[offset:offset + MAX_FRAME_PCM]
        yield PLAY_HEADER.pack(b"PLAY", 1, PLAY_HEADER.size, 0, play_id, first_seq + i, len(chunk)) + chunk


def play_end(play_id: int):
    return {"type": "play_end", "play_id": f"{play_id:016x}"}

