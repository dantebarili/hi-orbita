"""Servidor de audio real del laboratorio integrado, sin IA ni eco de PCM.

Ejecutar: python tools/ws_audio_server.py. El WAV y JSON se guardan en
tools/prueba_websocket (ignorado por Git). Ctrl+C conserva tomas incompletas.
"""
import argparse
import asyncio
import logging
from pathlib import Path
import wave
import time

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from ws_audio_protocol import Recording

LOG = logging.getLogger("audio_server")


async def handle(connection, output):
    recording = Recording(output)
    LOG.info("Conectado: %s", connection.remote_address)
    try:
        while True:
            # No mantener una toma abierta para siempre si el cliente deja de producir.
            message = await asyncio.wait_for(connection.recv(), timeout=30)
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
    async with serve(lambda ws: handle(ws, args.output), args.host, args.port,
                     max_size=8192, max_queue=8, compression=None,
                     ping_interval=20, ping_timeout=20, close_timeout=5):
        LOG.info("Escuchando %s:%d; salida %s", args.host, args.port, args.output.resolve())
        await asyncio.Event().wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "prueba_websocket")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
