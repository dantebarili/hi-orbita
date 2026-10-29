"""Servidor de chat de voz con wake word en el ESP: la ESP manda audio solo después de detectar "Órbita".

Uso: python tools/servidor_wakeword.py   (sin argumentos; claves y voz en tools/cloud_config.py, como chat_nube.py)
Dependencias: las mismas que chat_nube.py.

Protocolo (además de orbita-pcm-v2):
  ESP -> servidor  {"type":"wake","index":N,"seq":S,"preroll_samples":P}  justo antes del primer audio de una
                   conversación. Ese audio arranca P muestras ANTES de la detección (pre-roll).
  servidor -> ESP  {"type":"conversation_end","id":"<16 hex>"}  la ESP deja de mandar audio.
La conversación termina cuando, después de la detección o de la última respuesta, nadie habla en IDLE_END_S.

Cada activación se guarda para el inventario de falsas: WAV de 3 s antes + 1 s después de la detección (16 kHz,
mono, 16 bit: lo mismo que ve el modelo) y una fila en activaciones.csv con lo que transcribió el STT. La columna
`etiqueta` (real/falsa) se completa a mano escuchando. NUNCA subir estos audios a un repo.
"""
import asyncio
import csv
import json
import logging
from datetime import datetime
from pathlib import Path
import re
import secrets
import unicodedata
import wave

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from chat_nube import load_config
from ws_audio_cloud import CloudServices, Conversation
from ws_audio_protocol import HEADER
from ws_audio_session import Session
from ws_audio_speech import SAMPLE_RATE, play_frames

LOG = logging.getLogger("servidor_wakeword")
HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "prueba_websocket"
# Por defecto, al lado del dataset (fuera de este repo). Se cambia con ACTIVACIONES_DIR en cloud_config.py.
ACTIVACIONES_DEFAULT = HERE.parent.parent / "orbita-dataset" / "dataset_FULL" / "fuentes" / "activaciones_placa"

IDLE_END_S = 8.0           # sin voz durante 8 s (tras la detección o tras una respuesta): fin de la conversación
WORD_S = 1.5               # el detector de voz arranca 1,5 s antes de la detección: la palabra, no lo dicho antes
CLIP_AFTER_S = 1.0         # el WAV de la activación lleva 1 s después de la detección
CLIP_BEFORE_S = 3.0        # y 3 s antes (= el pre-roll de la ESP)
WAKE_WORD = "orbita"


def _sin_tildes(text):
    return "".join(c for c in unicodedata.normalize("NFD", text.lower()) if unicodedata.category(c) != "Mn")


class WakeConversation(Conversation):
    """Una conversación = desde un `wake` hasta el `conversation_end`. La muestra 0 es el inicio del pre-roll."""

    def __init__(self, services, session_id, send_pcm, play_done, preroll_samples, activaciones):
        super().__init__(services, OUTPUT, session_id, send_pcm, play_done, sync_on_connect=False)
        self.id = secrets.randbits(64)
        self.preroll = preroll_samples
        self.ignore_until = max(preroll_samples - int(WORD_S * SAMPLE_RATE), 0)
        self.activaciones = activaciones
        self.started = datetime.now()
        self.clip = bytearray()
        self.clip_file = None
        self.first_text = None          # primera transcripción (ayuda a etiquetar real/falsa)
        self.last_activity = self.clock()
        self.finished = False

    def feed(self, pcm):
        clip_bytes = int((CLIP_BEFORE_S + CLIP_AFTER_S) * SAMPLE_RATE) * 2
        start = max(self.preroll - int(CLIP_BEFORE_S * SAMPLE_RATE), 0) * 2
        if self.clip_file is None:
            self.clip += pcm
            if len(self.clip) >= start + clip_bytes:
                self.clip_file = self._save_clip(bytes(self.clip[start:start + clip_bytes]))
        before = self.state
        super().feed(pcm)
        now = self.clock()
        if self.state != "idle" or self.settling or before != "idle":
            self.last_activity = now     # hablando, esperando la nube, sonando o recién terminó
        elif now - self.last_activity >= IDLE_END_S:
            self.finished = True

    def clean_question(self, question):
        if self.first_text is None and question:
            self.first_text = question
        # Sacar la wake word del principio: "Órbita, ¿qué hora es?" -> "¿qué hora es?". Si era solo "Órbita",
        # no hay pregunta todavía: se sigue escuchando.
        resto = re.sub(rf"^[\s\"'¡¿]*{WAKE_WORD}[\s,.:;!?]*", "", _sin_tildes(question))
        if not re.search(r"\w", resto):
            LOG.info("Solo la wake word (%r): sigo escuchando la pregunta", question)
            return ""
        return question[len(question) - len(resto):].strip()    # sin tildes tiene el mismo largo que el original

    def _save_clip(self, pcm):
        folder = self.activaciones / self.started.strftime("%Y%m%d")
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"act_{self.started:%H%M%S}_{self.id:016x}.wav"
        with wave.open(str(path), "wb") as file:
            file.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
            file.writeframes(pcm)
        LOG.info("Activación guardada: %s", path.name)
        return path

    def write_row(self, reason):
        path = self.activaciones / "activaciones.csv"
        nuevo = not path.exists()
        self.activaciones.mkdir(parents=True, exist_ok=True)
        with path.open("a", newline="", encoding="utf-8") as file:
            out = csv.writer(file)
            if nuevo:
                out.writerow(["fecha_hora", "archivo", "transcripcion", "turnos", "fin", "etiqueta"])
            archivo = self.clip_file.relative_to(self.activaciones).as_posix() if self.clip_file else ""
            out.writerow([self.started.isoformat(timespec="seconds"), archivo, self.first_text or "",
                          self.turns, reason, ""])


async def handle(connection, services, activaciones):
    session = Session(OUTPUT, record=False)
    send_lock = asyncio.Lock()
    play_done = asyncio.Event()
    active_play = None
    conversation = None

    async def send_json(obj):
        async with send_lock:
            await connection.send(json.dumps(obj, separators=(",", ":")))

    async def send_pcm(pcm):
        nonlocal active_play
        identity = secrets.randbits(64)
        active_play = f"{identity:016x}"
        play_done.clear()
        for frame in play_frames(identity, pcm):
            async with send_lock:
                await connection.send(json.dumps(frame) if isinstance(frame, dict) else frame)
            await asyncio.sleep(0)

    async def finish(reason, notify=True):
        nonlocal conversation
        conv, conversation = conversation, None
        if conv is None:
            return
        await conv.close()
        await asyncio.to_thread(conv.write_row, reason)
        if notify:
            await send_json(dict(type="conversation_end", id=f"{conv.id:016x}"))
        LOG.info("Fin de la conversación (%s, %d turnos). Esperando \"Órbita\"", reason, conv.turns)

    try:
        async for message in connection:
            if isinstance(message, str):
                obj = json.loads(message)
                kind = obj.get("type") if isinstance(obj, dict) else None
                if kind in ("play_done", "play_reject"):
                    if obj.get("play_id") == active_play:
                        LOG.info("Parlante: %s", obj)
                        active_play = None
                        play_done.set()
                    continue
                if kind == "wake":
                    if conversation is not None:
                        LOG.info("wake con una conversación abierta: se ignora")
                        continue
                    preroll = int(obj.get("preroll_samples", 0))
                    conversation = WakeConversation(services, session.id, send_pcm, play_done, preroll, activaciones)
                    LOG.info("ÓRBITA detectada en la ESP (pre-roll %.1f s): escuchando", preroll / SAMPLE_RATE)
                    continue
            response = session.process(message)
            if response:
                async with send_lock:
                    await connection.send(response)
            if isinstance(message, bytes) and conversation is not None:
                conversation.feed(message[HEADER.size:])
                if conversation.finished:
                    await finish(f"{IDLE_END_S:.0f} s sin voz")
    except ConnectionClosed:
        pass
    except (ValueError, OSError):
        LOG.exception("Sesion de audio invalida")
        await connection.close(code=1008, reason="Audio invalido")
    finally:
        await finish("conexión cerrada", notify=False)
        session.close()
        LOG.info("ESP desconectada; sigo esperando conexiones")


async def main():
    cfg = load_config()
    try:
        services = CloudServices(cfg)
    except ValueError as error:
        raise SystemExit(f"Config: {error}")
    activaciones = Path(getattr(cfg, "ACTIVACIONES_DIR", "") or ACTIVACIONES_DEFAULT)
    async with serve(lambda ws: handle(ws, services, activaciones), cfg.HOST, cfg.PORT, max_size=8192,
                     max_queue=8, compression=None, ping_interval=20, ping_timeout=20, close_timeout=5):
        LOG.info("Servidor wake word en %s:%d | LLM %s | voz/STT %s (%s) | activaciones en %s | Ctrl+C para salir",
                 cfg.HOST, cfg.PORT, services.llm_name, services.speech.name, services.voice_name, activaciones)
        await asyncio.Event().wait()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
