"""Respuesta de voz hacia la ESP: arma los mensajes `play` + binarios PLAY + `play_end`."""
from struct import Struct

SAMPLE_RATE = 16_000
MAX_PLAY_SECONDS = 15
MAX_WS_BYTES = 4096
PLAY_HEADER = Struct("<4sBBHQII")
assert PLAY_HEADER.size == 24


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
