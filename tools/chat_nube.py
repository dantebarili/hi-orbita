"""Chat de voz continuo con la nube (sin wake word): hablás y te contesta por el parlante del ESP.

Uso: python tools/chat_nube.py   (sin argumentos; todo se define en tools/cloud_config.py)
Dependencias: pip install -r tools/requirements-websocket.txt requests numpy  (+ anthropic si LLM = "claude").
Es half-duplex: mientras responde, no te escucha.
"""
import asyncio
import json
import logging
from pathlib import Path
import secrets
import shutil
import sys

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from ws_audio_protocol import HEADER
from ws_audio_session import Session
from ws_audio_speech import SAMPLE_RATE, play_frames

LOG = logging.getLogger("chat_nube")
HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "prueba_websocket"


def load_config():
    try:
        import cloud_config
        return cloud_config
    except ImportError:
        shutil.copy(HERE / "cloud_config.example.py", HERE / "cloud_config.py")
        sys.exit(f"Creé {HERE / 'cloud_config.py'}: abrilo, completá las claves y volvé a ejecutar.")


async def handle(connection, services):
    session = Session(OUTPUT, record=False)
    send_lock = asyncio.Lock()
    play_done = asyncio.Event()
    active_play = None
    conversation = None

    async def send_pcm(pcm):
        nonlocal active_play
        identity = secrets.randbits(64)
        active_play = f"{identity:016x}"
        play_done.clear()
        for frame in play_frames(identity, pcm):
            async with send_lock:
                await connection.send(json.dumps(frame) if isinstance(frame, dict) else frame)
            await asyncio.sleep(0)

    try:
        async for message in connection:
            if isinstance(message, str):
                obj = json.loads(message)
                if isinstance(obj, dict) and obj.get("type") in ("play_done", "play_reject"):
                    if obj.get("play_id") == active_play:
                        LOG.info("Parlante: %s", obj)
                        active_play = None
                        play_done.set()
                    continue
            response = session.process(message)
            if response:
                async with send_lock:
                    await connection.send(response)
            if conversation is None:
                conversation = services.start(OUTPUT, session.id, send_pcm, play_done)
                LOG.info("ESP conectada (sesion %s). Hablale; contesta tras ~0.8 s de silencio", session.id)
            elif isinstance(message, bytes):
                conversation.feed(message[HEADER.size:])
    except ConnectionClosed:
        pass
    except (ValueError, OSError):
        LOG.exception("Sesion de audio invalida")
        await connection.close(code=1008, reason="Audio invalido")
    finally:
        if conversation is not None:
            await conversation.close()
        session.close()
        LOG.info("ESP desconectada; sigo esperando conexiones")


async def main():
    from ws_audio_cloud import CloudServices
    cfg = load_config()
    try:
        services = CloudServices(cfg)
    except ValueError as error:
        sys.exit(f"Config: {error}")
    async with serve(lambda ws: handle(ws, services), cfg.HOST, cfg.PORT, max_size=8192, max_queue=8,
                     compression=None, ping_interval=20, ping_timeout=20, close_timeout=5):
        LOG.info("Chat en %s:%d | LLM %s | voz/STT %s (%s) | Ctrl+C para salir", cfg.HOST, cfg.PORT,
                 services.llm_name, services.speech.name, services.voice_name)
        await asyncio.Event().wait()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
