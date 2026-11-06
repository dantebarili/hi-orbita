"""Servidor de chat de voz con wake word en el ESP: la ESP manda audio solo después de detectar "Órbita".

Uso: python tools/servidor_wakeword.py   (sin argumentos; claves y voz en tools/cloud_config.py)
Dependencias: pip install -r tools/requirements-websocket.txt  (+ anthropic si LLM = "claude").

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
from difflib import SequenceMatcher
import json
import logging
from datetime import datetime
from pathlib import Path
import re
import secrets
import shutil
import sys
import unicodedata
import wave

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from ws_audio_cloud import CloudServices, Conversation, StreamNormalizer
from ws_audio_protocol import HEADER
from ws_audio_session import Session
from ws_audio_speech import SAMPLE_RATE, play_begin, play_data, play_end

LOG = logging.getLogger("servidor_wakeword")
HERE = Path(__file__).resolve().parent
OUTPUT = HERE / "prueba_websocket"
# Por defecto, al lado del dataset (fuera de este repo). Se cambia con ACTIVACIONES_DIR en cloud_config.py.
ACTIVACIONES_DEFAULT = HERE.parent.parent / "orbita-dataset" / "dataset_FULL" / "fuentes" / "activaciones_placa"

IDLE_END_S = 8.0           # sin voz durante 8 s (tras la detección o tras una respuesta): fin de la conversación
WORD_S = 1.5               # al STT va desde 1,5 s antes de la detección: la palabra, no lo dicho antes
CLIP_AFTER_S = 1.0         # el WAV de la activación lleva 1 s después de la detección
CLIP_BEFORE_S = 3.0        # y 3 s antes (= el pre-roll de la ESP)
WAKE_WORD = "orbita"
# 06/11: el STT escribe la palabra inventada como "Orvita", "Orovida"... y se tomaba como pregunta (respuesta del
# LLM a la wake word sola, lenta). Se compara la primera palabra con b=v y d=t; 0,8 deja afuera "obvia" (0,73).
WAKE_PARECIDO = 0.8
ACK_ESPERA_S = 0.4         # "Órbita" sola: si en este tiempo no empezó la pregunta, suena el aviso (WAKE_ACK_TEXT)
ACK_TEXT_DEFAULT = "¿Sí?"

def load_config():
    try:
        import cloud_config
        return cloud_config
    except ImportError:
        shutil.copy(HERE / "cloud_config.example.py", HERE / "cloud_config.py")
        sys.exit(f"Creé {HERE / 'cloud_config.py'}: abrilo, completá las claves y volvé a ejecutar.")


def _uid(valor):
    """UID de la tarjeta NFC como lo manda la ESP (hex mayúsculas, 4/7/10 bytes) o None si no hay/no es válido."""
    return valor if isinstance(valor, str) and re.fullmatch(r"(?:[0-9A-F]{8}|[0-9A-F]{14}|[0-9A-F]{20})", valor) else None


def _sin_tildes(text):
    return "".join(c for c in unicodedata.normalize("NFD", text.lower()) if unicodedata.category(c) != "Mn")


def _es_wake(palabra):
    p = palabra.replace("v", "b").replace("d", "t")
    return len(p) >= 4 and SequenceMatcher(None, p, WAKE_WORD).ratio() >= WAKE_PARECIDO


def quitar_wake(question):
    """Saca la wake word del principio, como sea que la haya escrito el STT ("Órbita", "Orvita", "Or bita").
    Devuelve el resto ("" si era solo la wake word) o la pregunta entera si no empieza con ella."""
    palabras = list(re.finditer(r"[a-z]+", _sin_tildes(question)))  # sin tildes tiene el mismo largo que el original
    if not palabras:
        return question
    if _es_wake(palabras[0].group()):
        fin = palabras[0].end()
    elif len(palabras) > 1 and _es_wake(palabras[0].group() + palabras[1].group()):
        fin = palabras[1].end()
    else:
        return question
    return question[fin:].lstrip(" \t,.:;!?\"'").strip()


class WakeConversation(Conversation):
    """Una conversación = desde un `wake` hasta el `conversation_end`. La muestra 0 es el inicio del pre-roll."""

    def __init__(self, services, session_id, send_pcm, play_done, preroll_samples, activaciones, ack_pcm=None):
        super().__init__(services, OUTPUT, session_id, send_pcm, play_done)
        self.ack_pcm = ack_pcm          # "¿Sí?" ya generado y normalizado (None = sin aviso)
        self.t_wake = self.clock()
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
        super().feed(pcm)
        if self.state == "listening" and self.clock() - self.last_activity >= IDLE_END_S:
            self.finished = True         # nadie habló (sin texto del STT) desde la detección o la última respuesta

    def clean_question(self, question):
        if self.first_text is None and question:
            self.first_text = question
        # Sacar la wake word del principio: "Órbita, ¿qué hora es?" -> "¿qué hora es?". Si era solo "Órbita",
        # no hay pregunta todavía: suena el aviso y se sigue escuchando.
        resto = quitar_wake(question)
        if not re.search(r"\w", resto):
            LOG.info("Solo la wake word (%r): aviso y sigo escuchando la pregunta", question)
            if self.ack_pcm and (self.task is None or self.task.done()):
                self.task = asyncio.create_task(self._ack())
            return ""
        return resto

    async def _ack(self):
        """Aviso ya generado, sin LLM ni TTS: responde a "Órbita" sola en ~0,5 s en vez de ~3 s."""
        await asyncio.sleep(ACK_ESPERA_S)
        if self.state != "listening" or self.partial:
            return                       # ya empezó la pregunta: el aviso la taparía (la ESP no escucha mientras suena)
        self.state = "busy"
        try:
            LOG.info("Aviso %.2f s después del wake", self.clock() - self.t_wake)

            async def uno():
                yield self.ack_pcm
            await self.send_pcm(uno())
            await asyncio.wait_for(self.play_done.wait(), len(self.ack_pcm) / (2 * SAMPLE_RATE) + 5)
        except asyncio.TimeoutError:
            LOG.warning("El ESP no confirmó el fin del aviso")
        finally:
            self.ignore_until = self.received    # lo que llegó mientras sonaba no cuenta
            self.state = "listening"
            self.last_activity = self.clock()

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


async def handle(connection, services, activaciones, ack_pcm=None):
    session = Session(OUTPUT, record=False)
    send_lock = asyncio.Lock()
    play_done = asyncio.Event()
    active_play = None
    conversation = None
    medico = None                 # UID de la tarjeta NFC logueada en esta ESP (None = asistente general)

    async def send_json(obj):
        async with send_lock:
            await connection.send(json.dumps(obj, separators=(",", ":")))

    async def send_pcm(pedazos):
        """Manda una respuesta que llega en pedazos (async iterable de PCM): `play` con el primero, cada pedazo
        apenas llega, `play_end` al final. Si el TTS falla a mitad, cierra con lo que se mandó."""
        nonlocal active_play
        identity = secrets.randbits(64)
        seq, abierto = 0, False

        async def mandar(frame):
            async with send_lock:
                await connection.send(json.dumps(frame) if isinstance(frame, dict) else frame)

        try:
            async for pcm in pedazos:
                if not abierto:
                    active_play = f"{identity:016x}"
                    play_done.clear()
                    await mandar(play_begin(identity))
                    abierto = True
                for frame in play_data(identity, seq, pcm):
                    await mandar(frame)
                    seq += 1
                await asyncio.sleep(0)
        finally:
            if abierto:
                await mandar(play_end(identity))

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
                if kind == "nfc":                   # login/logout del médico (puede llegar antes del hello)
                    uid, evento = _uid(obj.get("uid")), obj.get("event")
                    if evento == "login" and uid:
                        medico = uid
                    elif evento == "logout" and uid == medico:
                        medico = None
                    LOG.info("NFC %s %s -> médico: %s", evento, uid, medico or "ninguno")
                    if conversation is not None:
                        conversation.nfc_uid = medico   # los turnos que siguen quedan con el médico nuevo
                    continue
                if kind in ("hello", "wake") and "uid" in obj:
                    medico = _uid(obj.get("uid"))    # el estado que manda la ESP manda (reconexión, aviso perdido)
                if kind == "wake":
                    if conversation is not None:
                        LOG.info("wake con una conversación abierta: se ignora")
                        continue
                    preroll = int(obj.get("preroll_samples", 0))
                    conversation = WakeConversation(services, session.id, send_pcm, play_done, preroll, activaciones, ack_pcm)
                    conversation.nfc_uid = medico
                    LOG.info("ÓRBITA detectada en la ESP (pre-roll %.1f s, médico: %s): escuchando",
                             preroll / SAMPLE_RATE, medico or "ninguno, asistente general")
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


def generar_ack(services, texto):
    """El aviso para "Órbita" sola se genera una vez al arrancar (un pedido al TTS) y queda en memoria."""
    if not texto:
        return None
    try:
        norm = StreamNormalizer()
        pcm = norm.push(services.tts.speak(texto)) + norm.flush()
        LOG.info("Aviso de wake word %r: %.2f s", texto, len(pcm) / (2 * SAMPLE_RATE))
        return pcm
    except Exception as error:
        LOG.warning("No se pudo generar el aviso %r (%s): sin aviso", texto, error)
        return None


async def main():
    cfg = load_config()
    try:
        services = CloudServices(cfg)
    except ValueError as error:
        raise SystemExit(f"Config: {error}")
    activaciones = Path(getattr(cfg, "ACTIVACIONES_DIR", "") or ACTIVACIONES_DEFAULT)
    ack_pcm = await asyncio.to_thread(generar_ack, services, getattr(cfg, "WAKE_ACK_TEXT", ACK_TEXT_DEFAULT))
    async with serve(lambda ws: handle(ws, services, activaciones, ack_pcm), cfg.HOST, cfg.PORT, max_size=8192,
                     max_queue=8, compression=None, ping_interval=20, ping_timeout=20, close_timeout=5):
        LOG.info("Servidor wake word en %s:%d | LLM %s | STT %s (cierre %.2f s de silencio) | voz %s | activaciones en %s"
                 " | Ctrl+C para salir", cfg.HOST, cfg.PORT, services.llm_name, services.stt.name,
                 float(getattr(cfg, "STT_VAD_SILENCE_S", 0.8)), services.voice_name, activaciones)
        await asyncio.Event().wait()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
