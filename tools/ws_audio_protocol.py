"""Header binario del audio que manda la ESP (orbita-pcm-v2). Lo valida ws_audio_session.Session."""
import struct

HEADER = struct.Struct("<4sBBHQIIIHHHHI")
assert HEADER.size == 40
