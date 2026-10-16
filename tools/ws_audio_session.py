"""Protocolo continuo `orbita-pcm-v2` del prototipo alfa: una sesion por conexion, sin fin.

Mensajes de texto (JSON): ESP -> servidor `hello`; servidor -> ESP `ready` y `ack`.
Audio: binarios con el mismo header ORBA de v1; el id del header es el id de sesion y el
indice de mensaje sigue creciendo sin cortes. Los huecos se anotan, no se rellenan.
El audio se guarda en archivos de SEGMENT_SECONDS; cada uno con su JSON de huecos.
"""
import json
from pathlib import Path
import re
import time
import wave
import zlib

from ws_audio_protocol import HEADER

SEGMENT_SECONDS = 30
ACK_EVERY = 8          # mensajes de 128 ms: un ack cada ~1 s
MASK = 0xffffffff


class Session:
    """record=False conserva validacion/ACK sin crear archivos; huecos acotados a 128."""
    def __init__(self, directory, previous_expected_seq=None,
                 segment_samples=SEGMENT_SECONDS * 16000, previous=None, record=True):
        self.closed = False
        self.record = record
        self.gap_count = 0
        self.directory = Path(directory)
        self.segment_samples = segment_samples
        self.previous_expected_seq = previous_expected_seq   # fijo (pruebas); si hay `previous`, manda el vivo
        self.previous = previous                             # sesion anterior: su seq se lee al llegar el 1er mensaje
        self.wav = None
        self.segment = None
        self.segments = 0
        self.id = None
        self.stamp = None
        self.started = None
        self.expected_seq = None      # seq del proximo slot esperado
        self.messages = 0
        self.samples = 0
        self.gaps = []                # posicion global (muestras) de cada hueco
        self.info = {}

    # -- archivos ---------------------------------------------------------
    def _segment_path(self, suffix):
        return self.directory / f"{self.stamp}_{self.id}_{self.segments:03d}{suffix}"

    def _open_segment(self):
        path = self._segment_path(".wav")
        self.wav = wave.open(str(path), "wb")
        self.wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        self.segment = dict(file=path.name, first_message=self.messages, start_sample=self.samples,
                            samples=0, messages=0, gaps=[], status="open")

    def _close_segment(self, status):
        if self.wav is None:
            return
        self.wav.close()
        self.wav = None
        self.segment["status"] = status
        self._segment_path(".json").write_text(
            json.dumps(self.segment, indent=2), encoding="utf-8")
        self.segments += 1

    def _write_summary(self, status, reason=""):
        self.info.update(status=status, close_reason=reason, messages=self.messages,
                         samples=self.samples, duration_seconds=self.samples / 16000,
                         segments=self.segments, next_expected_seq=self.expected_seq,
                         elapsed_seconds=time.monotonic() - self.started, gaps=self.gaps)
        (self.directory / f"{self.stamp}_{self.id}.json").write_text(
            json.dumps(self.info, indent=2), encoding="utf-8")

    # -- protocolo --------------------------------------------------------
    def hello(self, obj):
        identity = obj.get("id")
        if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{16}", identity):
            raise ValueError("id invalido")
        if (obj.get("protocol") != "orbita-pcm-v2" or obj.get("format") != "s16le" or
                obj.get("rate") != 16000 or obj.get("channels") != 1 or
                obj.get("source") not in ("mic_l", "mic_r", "afe")):
            raise ValueError("formato no soportado")
        self.id = identity
        self.stamp = time.strftime("sesion_%Y%m%d_%H%M%S")
        self.started = time.monotonic()
        if self.record:
            self.directory.mkdir(parents=True, exist_ok=True)
        self.info = dict(id=identity, source=obj["source"], protocol="orbita-pcm-v2")
        if self.record:
            self._open_segment()
        return json.dumps(dict(type="ready", id=identity), separators=(",", ":"))

    def binary(self, data):
        """Procesa un mensaje de audio. Devuelve un ack (JSON) cuando toca, si no None."""
        if self.closed or self.id is None or len(data) < HEADER.size:
            raise ValueError("audio sin hello o header incompleto")
        magic, version, size, flags, identity, index, seq, lost, slots, samples, voice, vad, crc = \
            HEADER.unpack_from(data)
        if (magic != b"ORBA" or version != 1 or size != HEADER.size or flags != 0 or
                identity != int(self.id, 16) or index != self.messages or
                not 1 <= slots <= 4 or not 1 <= samples <= slots * 512 or
                not 0 <= voice <= vad <= slots or len(data) != HEADER.size + 2 * samples):
            raise ValueError("header, secuencia de mensaje o longitud invalida")
        pcm = data[HEADER.size:]
        if zlib.crc32(pcm) != crc:
            raise ValueError("CRC del mensaje no coincide")
        if self.expected_seq is None:
            self._note_start(seq, lost)
            missing = 0
        else:
            missing = (seq - self.expected_seq) & MASK
            if missing >= 0x80000000:
                raise ValueError("slots repetidos o fuera de orden")
        if lost or missing:
            # `lost` y `missing` pueden describir el mismo hueco: no sumarlos.
            gap = dict(at_sample=self.samples, seq=seq, lost_samples=lost, missing_slots=missing)
            self.gap_count += 1
            self.gaps.append(gap)
            if self.record:
                self.segment["gaps"].append(gap)
            elif len(self.gaps) > 128:
                del self.gaps[0]  # diagnostico acotado en sesiones STT de duracion indefinida
        self.expected_seq = (seq + slots) & MASK
        if self.record:
            self.wav.writeframesraw(pcm)
        self.samples += samples
        self.messages += 1
        if self.record:
            self.segment["samples"] += samples
            self.segment["messages"] += 1
            if self.segment["samples"] >= self.segment_samples:
                self._close_segment("complete")
                self._open_segment()
        if self.messages % ACK_EVERY == 0:
            return json.dumps(dict(type="ack", index=index), separators=(",", ":"))
        return None

    def _note_start(self, seq, lost):
        """Primer mensaje: compara con donde termino la sesion anterior de este servidor."""
        self.info["first_seq"] = seq
        self.info["first_lost"] = lost
        ref = self.previous.reference_seq() if self.previous is not None else self.previous_expected_seq
        self.previous = None                                         # cortar la cadena de referencias
        if ref is None:
            self.info["gap_from_previous_session_slots"] = None     # no hay sesion anterior
            return
        missing = (seq - ref) & MASK
        # Un seq "hacia atras" indica que el equipo se reinicio (el contador vuelve a 0).
        restart = missing >= 0x80000000
        self.info["gap_from_previous_session_slots"] = None if restart else missing
        self.info["previous_session_restart"] = restart

    def reference_seq(self):
        """Donde termino esta sesion. Se lee EN VIVO: al reconectar, la sesion vieja puede seguir abierta
        en el servidor (la conexion muerta tarda en cerrarse) cuando llega el primer mensaje de la nueva."""
        if self.expected_seq is not None:
            return self.expected_seq
        return self.previous.reference_seq() if self.previous is not None else self.previous_expected_seq

    def process(self, message):
        if isinstance(message, bytes):
            return self.binary(message)
        obj = json.loads(message)
        if not isinstance(obj, dict):
            raise ValueError("control debe ser objeto JSON")
        if obj.get("type") == "hello" and self.id is None:
            return self.hello(obj)
        raise ValueError("control desconocido")

    def close(self, reason="conexion cerrada"):
        """Cierra la sesion; devuelve el seq esperado siguiente (para la sesion que venga)."""
        if self.id is None:
            return None
        if self.wav is not None:
            if self.segment["messages"] == 0:       # recien rotado: no dejar un WAV vacio
                self.wav.close()
                self.wav = None
                self._segment_path(".wav").unlink(missing_ok=True)
            else:
                self._close_segment("interrupted")
        if not self.closed and self.record:
            self._write_summary("closed", reason)
        self.closed = True
        return self.expected_seq
