"""Servidor de audio real del laboratorio integrado, sin IA ni eco de PCM.

Ejecutar: python tools/ws_audio_server.py. El WAV y JSON se guardan en
tools/prueba_websocket (ignorado por Git). Ctrl+C conserva tomas incompletas.
"""
import argparse
import asyncio
import json
import logging
from pathlib import Path
import wave
import time

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from ws_audio_protocol import Recording
from ws_audio_session import Session

LOG = logging.getLogger("audio_server")
# Ultima sesion v2 de cada carpeta de salida, para medir el hueco entre sesiones cuando el equipo
# reconecta. Se guarda la sesion (no un numero): la vieja puede seguir abierta al llegar la nueva.
LAST_SESSION = {}


def is_hello(message):
    """True si el primer mensaje es el `hello` del protocolo continuo v2."""
    if not isinstance(message, str):
        return False
    try:
        obj = json.loads(message)
    except ValueError:
        return False
    return isinstance(obj, dict) and obj.get("type") == "hello"


async def handle(connection, output, segment_seconds=None):
    """Decide v1 (toma con start/end) o v2 (sesion continua con hello) por el primer mensaje."""
    try:
        first = await asyncio.wait_for(connection.recv(), timeout=30)
    except (ConnectionClosed, asyncio.TimeoutError):
        return
    if is_hello(first):
        await handle_session(connection, output, first, segment_seconds)
    else:
        await handle_take(connection, output, first)


async def handle_session(connection, output, first, segment_seconds=None):
    key = str(output)
    kwargs = {} if segment_seconds is None else dict(segment_samples=int(segment_seconds * 16000))
    session = Session(output, previous=LAST_SESSION.get(key), **kwargs)
    LAST_SESSION[key] = session
    LOG.info("Conectado (sesion continua): %s", connection.remote_address)
    reason = "conexion cerrada"
    try:
        message = first
        while True:
            response = session.process(message)
            if message is first:
                LOG.info("Sesion %s (%s)", session.id, session.info["source"])
                await connection.send(response)
            elif response:
                await connection.send(response)
            if session.messages and session.messages % 32 == 0 and not isinstance(message, str):
                elapsed = max(0.001, time.monotonic() - session.started)
                LOG.info("RX: %d mensajes, %.2f KB/s de PCM, %d huecos, %d archivos",
                         session.messages, session.samples * 2 / elapsed / 1000,
                         len(session.gaps), session.segments)
            message = await asyncio.wait_for(connection.recv(), timeout=30)
    except ConnectionClosed:
        pass
    except (ValueError, OSError, wave.Error, asyncio.TimeoutError) as error:
        reason = f"rechazada: {error!r}"
        LOG.warning("Sesion interrumpida/rechazada: %r", error)
        await connection.close(code=1008, reason="Sesion invalida, inactiva o fallo de almacenamiento")
    finally:
        try:
            session.close(reason)
            if session.id is not None:
                LOG.info("Sesion cerrada: %d mensajes, %.1f s, %d huecos; hueco vs sesion anterior: %s slots",
                         session.messages, session.samples / 16000, len(session.gaps),
                         session.info.get("gap_from_previous_session_slots"))
        except (OSError, wave.Error) as error:
            LOG.error("No se pudo cerrar la sesion: %s", error)
        LOG.info("Desconectado: %s", connection.remote_address)


async def handle_take(connection, output, first):
    recording = Recording(output)
    LOG.info("Conectado: %s", connection.remote_address)
    try:
        message = first
        while True:
            response = recording.process(message)
            if response:
                await connection.send(response)
                if response.startswith("saved "):
                    LOG.info("WAV GUARDADO: %s; %d muestras, %.3f s; estado=%s",
                             recording.path, recording.info["samples"],
                             recording.info["duration_seconds"], recording.info["status"])
                else:
                    LOG.info("Inicio %s (%s)", recording.info["id"], recording.info["source"])
            elif recording.info["messages"] % 32 == 0:
                elapsed = max(0.001, time.monotonic()-recording.started)
                LOG.info("RX: %d mensajes, %.2f KB/s de PCM, %d huecos",
                         recording.info["messages"], recording.info["samples"]*2/elapsed/1000,
                         len(recording.info["gaps"]))
            # No mantener una toma abierta para siempre si el cliente deja de producir.
            message = await asyncio.wait_for(connection.recv(), timeout=30)
    except ConnectionClosed:
        pass
    except (ValueError, OSError, wave.Error, asyncio.TimeoutError) as error:
        LOG.warning("Toma interrumpida/rechazada: %s", error)
        await connection.close(code=1008, reason="Toma invalida, inactiva o fallo de almacenamiento")
    finally:
        try:
            partial = recording.close()
            if partial:
                LOG.warning("WAV INCOMPLETO: %s", partial)
        except (OSError, wave.Error) as error:
            LOG.error("No se pudo finalizar el parcial: %s", error)
        LOG.info("Desconectado: %s", connection.remote_address)


async def run(args):
    async with serve(lambda ws: handle(ws, args.output, args.segment_seconds), args.host, args.port,
                     max_size=8192, max_queue=8, compression=None,
                     ping_interval=20, ping_timeout=20, close_timeout=5):
        LOG.info("Escuchando %s:%d; salida %s", args.host, args.port, args.output.resolve())
        await asyncio.Event().wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "prueba_websocket")
    parser.add_argument("--segment-seconds", type=float, default=None,
                        help="largo de cada archivo en modo continuo (por defecto 30 s). Un segmento "
                             "solo se puede abrir cuando se cerro: usar valores chicos para depurar.")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
