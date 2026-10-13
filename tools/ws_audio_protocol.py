"""Protocolo de laboratorio, independiente del servidor y de su transporte.

No es el protocolo final de la FSM. No rellena huecos con ceros: los anota
en el JSON y distingue toma completa, con pérdidas e incompleta.
"""
import json
from pathlib import Path
import re
import struct
import time
import uuid
import wave
import zlib

HEADER = struct.Struct("<4sBBHQIIIHHHHI")
assert HEADER.size == 40
ERROR_STATS = ("dma_overflows", "i2s_errors", "ring_in_overruns", "invalid_chunks",
               "invalid_fetch", "ring_out_errors", "sink_invalid_slots", "ring_discarded_total")


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name} fuera de rango")
    return value


class Recording:
    """Una toma activa por conexión. Nombres de archivos generados por el servidor."""

    def __init__(self, directory):
        self.directory = Path(directory)
        self.wav = None
        self.path = None
        self.info = None
        self.expected_seq = None
        self.started = None

    def _report(self, status, reason=""):
        self.info.update(status=status, reason=reason,
                         duration_seconds=self.info["samples"] / 16000,
                         elapsed_seconds=time.monotonic() - self.started,
                         file=self.path.name)
        self.path.with_suffix(".json").write_text(
            json.dumps(self.info, indent=2, ensure_ascii=False), encoding="utf-8")

    def close(self, reason="conexion cerrada sin fin"):
        if self.wav is not None:
            self.wav.close()
            self.wav = None
            self._report("incomplete", reason)
            return self.path
        return None

    def start(self, obj):
        identity = obj.get("id")
        if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{16}", identity):
            raise ValueError("id invalido")
        if (obj.get("protocol") != "orbita-pcm-v1" or obj.get("format") != "s16le" or
                obj.get("rate") != 16000 or obj.get("channels") != 1 or
                obj.get("source") not in ("mic_l", "afe")):
            raise ValueError("formato no soportado")
        target = integer(obj.get("target_samples"), 16000, 16000*600, "target_samples")
        dropped = integer(obj.get("dropped_before"), 0, 2**64-1, "dropped_before")
        failures = integer(obj.get("failed_attempts"), 0, 2**32-1, "failed_attempts")
        # Si se inicia otra toma en el mismo socket tras un error, no unirlas.
        self.close("reemplazada por nueva toma")
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / (time.strftime("audio_%Y%m%d_%H%M%S_") + uuid.uuid4().hex + ".incomplete.wav")
        self.wav = wave.open(str(self.path), "wb")
        self.wav.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        self.info = dict(id=identity, source=obj["source"], target_samples=target,
                         samples=0, messages=0, crc32=0, gaps=[], lost_reported=0,
                         dropped_before=dropped, failed_attempts=failures)
        self.expected_seq = None
        self.started = time.monotonic()
        return f"ready {identity}"

    def binary(self, data):
        if self.wav is None or len(data) < HEADER.size:
            raise ValueError("audio sin inicio o header incompleto")
        magic, version, size, flags, identity, index, seq, lost, slots, samples, voice, vad, crc = HEADER.unpack_from(data)
        if (magic != b"ORBA" or version != 1 or size != HEADER.size or flags != 0 or
                identity != int(self.info["id"], 16) or index != self.info["messages"] or
                not 1 <= slots <= 4 or not 1 <= samples <= slots*512 or
                not 0 <= voice <= vad <= slots or len(data) != HEADER.size+2*samples):
            raise ValueError("header, secuencia de mensaje o longitud invalida")
        # Límite por toma: se admite el último mensaje entero, hasta 2047 muestras extra.
        if self.info["samples"] >= self.info["target_samples"]:
            raise ValueError("audio posterior al limite de toma")
        pcm = data[HEADER.size:]
        if zlib.crc32(pcm) != crc:
            raise ValueError("CRC del mensaje no coincide")
        missing_slots = 0 if self.expected_seq is None else (seq-self.expected_seq) & 0xffffffff
        if missing_slots >= 0x80000000:
            raise ValueError("slots repetidos o fuera de orden")
        if lost or missing_slots:
            # No sumar ambos como pérdidas independientes: pueden describir el mismo hueco.
            self.info["gaps"].append(dict(before_sample=self.info["samples"], seq=seq,
                                         lost_samples=lost, missing_slots=missing_slots))
            self.info["lost_reported"] += lost
        self.expected_seq = (seq+slots) & 0xffffffff
        self.wav.writeframesraw(pcm)
        self.info["samples"] += samples
        self.info["messages"] += 1
        self.info["crc32"] = zlib.crc32(pcm, self.info["crc32"])

    def end(self, obj):
        if self.wav is None or obj.get("id") != self.info["id"]:
            raise ValueError("fin sin inicio o id incorrecto")
        for name in ("samples", "messages", "crc32"):
            integer(obj.get(name), 0, 2**32-1, name)
            if obj[name] != self.info[name]:
                raise ValueError(f"fin no coincide: {name}")
        if not self.info["target_samples"] <= self.info["samples"] < self.info["target_samples"]+2048:
            raise ValueError("toma demasiado corta/larga")
        stats = obj.get("stats")
        if not isinstance(stats, dict) or not all(key in stats for key in ERROR_STATS):
            raise ValueError("faltan contadores del pipeline")
        for name, value in stats.items():
            integer(value, 0, 2**32-1, name)
        self.info["stats"] = stats
        gaps = bool(self.info["gaps"] or any(stats[key] for key in ERROR_STATS))
        self.wav.close()
        self.wav = None
        destination = self.path.with_name(self.path.name.replace(".incomplete.wav", ".gaps.wav" if gaps else ".wav"))
        # Escribir reporte final antes de renombrar el WAV. Si falla cualquiera de
        # los pasos no hay ACK; un .incomplete.wav nunca se presenta como completo.
        original = self.path
        self.path = destination
        try:
            self._report("complete_with_gaps" if gaps else "complete")
            original.rename(destination)
        except OSError as error:
            self.path = original
            # El WAV ya está cerrado. Recuperar un reporte de parcial si el disco
            # lo permite, sin tapar el error original ni enviar confirmación.
            try:
                destination.with_suffix(".json").unlink(missing_ok=True)
            except OSError:
                pass
            try:
                self._report("incomplete", f"fallo al finalizar: {error}")
            except OSError:
                pass
            raise
        return f"saved {self.info['id']} {'gaps' if gaps else 'clean'}"

    def process(self, message):
        if isinstance(message, bytes):
            self.binary(message)
            return None
        obj = json.loads(message)
        if not isinstance(obj, dict):
            raise ValueError("control debe ser objeto JSON")
        if obj.get("type") == "start":
            return self.start(obj)
        if obj.get("type") == "end":
            return self.end(obj)
        raise ValueError("control desconocido")
