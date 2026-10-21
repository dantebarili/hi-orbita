"""Conversación por nube para el prototipo de laboratorio (corre en la PC, no en el ESP).

Flujo: wake word local (Whisper, ws_audio_live) -> captura hasta silencio -> STT -> LLM (Claude) -> TTS
-> PLAY al ESP. Mide la latencia de cada etapa en cada turno. Las claves se cargan una sola vez con
`python ws_audio_server.py --set-keys` (quedan en tools/.orbita_keys.json, fuera de git) o por variable de entorno.
"""
import asyncio
import getpass
import io
import json
import logging
import os
import time
import wave
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import requests

from ws_audio_speech import MAX_PLAY_SECONDS, SAMPLE_RATE

LOG = logging.getLogger("audio_cloud")
KEYS_FILE = Path(__file__).resolve().parent / ".orbita_keys.json"
KEY_NAMES = {"ELEVENLABS_API_KEY": "ElevenLabs", "ANTHROPIC_API_KEY": "Anthropic"}

BLOCK = SAMPLE_RATE // 10          # 100 ms: unidad del detector de fin de frase
END_QUIET_BLOCKS = 8               # 0.8 s de silencio cierran la frase
START_TIMEOUT_BLOCKS = 40          # 4 s sin que nadie hable: se aborta la captura
MAX_UTTERANCE_SAMPLES = 12 * SAMPLE_RATE
RING_SAMPLES = 12 * SAMPLE_RATE    # historial para recuperar lo dicho mientras Whisper detectaba
MIN_THRESHOLD = 50                 # RMS mínimo para considerar "voz" (el mic crudo es muy bajo)
POST_PLAY_GUARD = SAMPLE_RATE      # tras el PLAY se ignora el wake word 1 s (el mic oye el parlante)

SYSTEM_PROMPT = (
    "Sos Órbita, un asistente de voz de escritorio para consultorios médicos. Tu respuesta se lee en voz alta: "
    "español rioplatense, máximo dos frases cortas, sin listas, sin markdown, sin emojis. "
    "No des diagnósticos ni indicaciones de tratamiento; si te lo piden, decí que lo decide el profesional."
)


# -- claves -----------------------------------------------------------------
def load_keys():
    """Variable de entorno primero, después el archivo local. Devuelve {nombre: valor o None}."""
    saved = {}
    try:
        saved = json.loads(KEYS_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    return {name: os.environ.get(name) or saved.get(name) for name in KEY_NAMES}


def set_keys():
    """Pide las claves por consola (sin eco) y las guarda con permisos 600. Enter conserva la actual."""
    keys = load_keys()
    for name, label in KEY_NAMES.items():
        value = getpass.getpass(f"Clave de {label} ({'Enter conserva la guardada' if keys[name] else 'obligatoria'}): ").strip()
        if value:
            keys[name] = value
    fd = os.open(KEYS_FILE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as file:
        json.dump({name: value for name, value in keys.items() if value}, file)
    print(f"Claves guardadas en {KEYS_FILE}")


# -- clientes ---------------------------------------------------------------
def _check(response):
    if not response.ok:
        raise RuntimeError(f"ElevenLabs HTTP {response.status_code}: {response.text[:300]}")
    return response


class ElevenLabs:
    BASE = "https://api.elevenlabs.io/v1"

    def __init__(self, key, voice_id, tts_model, stt_model):
        self.voice_id, self.tts_model, self.stt_model = voice_id, tts_model, stt_model
        self.http = requests.Session()   # reusa la conexión TLS entre pedidos: ahorra ~100-200 ms por turno
        self.http.headers["xi-api-key"] = key

    def transcribe(self, pcm):
        wav = io.BytesIO()
        with wave.open(wav, "wb") as file:
            file.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
            file.writeframes(pcm)
        response = self.http.post(f"{self.BASE}/speech-to-text",
                                  data={"model_id": self.stt_model, "language_code": "es"},
                                  files={"file": ("voz.wav", wav.getvalue(), "audio/wav")}, timeout=30)
        return _check(response).json().get("text", "").strip()

    def speak(self, text):
        """PCM s16le mono 16 kHz, el formato que espera PLAY."""
        response = self.http.post(f"{self.BASE}/text-to-speech/{self.voice_id}",
                                  params={"output_format": "pcm_16000"},
                                  json={"text": text, "model_id": self.tts_model}, timeout=30)
        pcm = _check(response).content
        return pcm[:len(pcm) - len(pcm) % 2]


class Claude:
    def __init__(self, key, model):
        import anthropic
        self.client = anthropic.Anthropic(api_key=key)
        self.model = model
        self.history = []
        self.last_used = 0.0

    def reply(self, text):
        """Devuelve (respuesta, tokens_in, tokens_out). La conversación se olvida tras 2 min de silencio."""
        if time.monotonic() - self.last_used > 120:
            self.history.clear()
        messages = self.history + [{"role": "user", "content": text}]
        response = self.client.messages.create(
            model=self.model, max_tokens=300, system=SYSTEM_PROMPT,
            output_config={"effort": "low"},   # voz: prioriza latencia sobre razonamiento profundo
            messages=messages)
        answer = "".join(block.text for block in response.content if block.type == "text").strip()
        if response.stop_reason == "refusal" or not answer:
            answer = "No puedo ayudarte con eso."
        self.history = (messages + [{"role": "assistant", "content": answer}])[-12:]
        self.last_used = time.monotonic()
        return answer, response.usage.input_tokens, response.usage.output_tokens


class CloudServices:
    """Clientes compartidos por todas las conexiones (la historia del LLM vive acá)."""

    def __init__(self, keys, llm_model, voice_id, tts_model, stt_model, local_recognizer=None):
        missing = [label for name, label in KEY_NAMES.items() if not keys.get(name)]
        if missing:
            raise ValueError("faltan claves: " + ", ".join(missing) + ". Cargarlas con --set-keys")
        self.eleven = ElevenLabs(keys["ELEVENLABS_API_KEY"], voice_id, tts_model, stt_model)
        self.claude = Claude(keys["ANTHROPIC_API_KEY"], llm_model)
        self.local_recognizer = local_recognizer   # si no es None, el STT es Whisper local en vez de ElevenLabs

    def stt(self, pcm):
        return (self.local_recognizer or self.eleven).transcribe(pcm)

    def start(self, output, session_id, send_pcm, play_done, save_audio):
        return Conversation(self, output, session_id, send_pcm, play_done, save_audio)


# -- una conexión -----------------------------------------------------------
class Conversation:
    """Estados: idle (solo bufferea) -> capturing (hasta silencio) -> busy (nube + reproducción) -> idle."""

    def __init__(self, services, output, session_id, send_pcm, play_done, save_audio):
        self.services, self.output, self.session_id = services, Path(output), session_id
        self.send_pcm, self.play_done, self.save_audio = send_pcm, play_done, save_audio
        self.buf = bytearray()
        self.buf_start = 0            # índice (en muestras) del primer byte de buf
        self.state = "idle"
        self.cap_from = 0
        self.threshold = MIN_THRESHOLD
        self.ignore_until = 0
        self.turns = 0
        self.task = None

    @property
    def latest(self):
        return self.buf_start + len(self.buf) // 2

    def feed(self, pcm, start_sample):
        self.buf += pcm
        excess = len(self.buf) // 2 - RING_SAMPLES
        if excess > 0:
            del self.buf[:excess * 2]
            self.buf_start += excess
        if self.state == "capturing":
            self._scan()

    def _blocks_rms(self, first_sample):
        audio = np.frombuffer(bytes(self.buf), dtype="<i2")[max(first_sample - self.buf_start, 0):]
        count = len(audio) // BLOCK
        if count == 0:
            return audio, np.empty(0)
        blocks = audio[:count * BLOCK].astype(np.float32).reshape(count, BLOCK)
        return audio, np.sqrt((blocks ** 2).mean(axis=1))

    async def on_detect(self, wake_end_seconds):
        wake_end = int(wake_end_seconds * SAMPLE_RATE)
        if self.state != "idle" or wake_end < self.ignore_until:
            LOG.info("Wake word ignorada (%s)", "turno en curso" if self.state != "idle" else "eco del parlante")
            return
        _, rms = self._blocks_rms(self.buf_start)
        floor = float(np.percentile(rms, 20)) if len(rms) else 0.0
        self.threshold = max(3 * floor, MIN_THRESHOLD)
        self.cap_from, self.state = wake_end, "capturing"
        LOG.info("Capturando frase (piso de ruido RMS %.0f, umbral de voz %.0f)", floor, self.threshold)
        self._scan()

    def _scan(self):
        audio, rms = self._blocks_rms(self.cap_from)
        loud = np.nonzero(rms > self.threshold)[0]
        if len(loud):
            if len(rms) - 1 - loud[-1] >= END_QUIET_BLOCKS or len(audio) >= MAX_UTTERANCE_SAMPLES:
                self._finish(audio[:(loud[-1] + 2) * BLOCK].tobytes())
        elif len(rms) >= START_TIMEOUT_BLOCKS:
            LOG.info("Nadie habló tras la wake word; captura abortada")
            self.state = "idle"

    def _finish(self, pcm):
        self.state = "busy"
        self.task = asyncio.create_task(self._turn(pcm, time.monotonic()))

    async def _turn(self, pcm, t_end):
        record = dict(time=datetime.now(timezone.utc).isoformat(), session=self.session_id,
                      utterance_seconds=round(len(pcm) / (2 * SAMPLE_RATE), 2),
                      stt="local" if self.services.local_recognizer else "elevenlabs")
        try:
            question = await asyncio.to_thread(self.services.stt, pcm)
            t_stt = time.monotonic()
            record["question"] = question
            if not question:
                LOG.info("STT sin texto; se descarta el turno")
                return
            answer, tokens_in, tokens_out = await asyncio.to_thread(self.services.claude.reply, question)
            t_llm = time.monotonic()
            speech = (await asyncio.to_thread(self.services.eleven.speak, answer))[:MAX_PLAY_SECONDS * SAMPLE_RATE * 2]
            t_tts = time.monotonic()
            await self.send_pcm(speech)
            t_sent = time.monotonic()
            record.update(answer=answer, tokens_in=tokens_in, tokens_out=tokens_out,
                          reply_seconds=round(len(speech) / (2 * SAMPLE_RATE), 2),
                          stt_s=round(t_stt - t_end, 3), llm_s=round(t_llm - t_stt, 3),
                          tts_s=round(t_tts - t_llm, 3), send_s=round(t_sent - t_tts, 3),
                          total_s=round(t_sent - t_end, 3))
            LOG.info("Turno %d: %r -> %r | STT %.2f s, LLM %.2f s, TTS %.2f s, envío %.2f s | fin de habla -> PLAY: %.2f s "
                     "(+%.1f s de silencio que espera el detector)", self.turns + 1, question, answer, record["stt_s"],
                     record["llm_s"], record["tts_s"], record["send_s"], record["total_s"], END_QUIET_BLOCKS / 10)
            if self.save_audio:
                await asyncio.to_thread(self._save_wavs, pcm, speech)
            # Esperar el fin de la reproducción para no tomar el propio parlante como pregunta nueva.
            try:
                await asyncio.wait_for(self.play_done.wait(), len(speech) / (2 * SAMPLE_RATE) + 5)
            except asyncio.TimeoutError:
                LOG.warning("El ESP no confirmó el fin de la reproducción")
        except asyncio.CancelledError:
            raise
        except Exception as error:
            record["error"] = str(error)
            LOG.exception("Falló el turno de nube; se sigue escuchando")
        finally:
            self.turns += 1
            self.ignore_until = self.latest + POST_PLAY_GUARD
            self.state = "idle"
            await asyncio.to_thread(self._log, record)

    def _log(self, record):
        self.output.mkdir(parents=True, exist_ok=True)
        with (self.output / f"cloud_{self.session_id}.jsonl").open("a", encoding="utf-8") as file:
            file.write(json.dumps(record, ensure_ascii=False) + "\n")

    def _save_wavs(self, question_pcm, reply_pcm):
        self.output.mkdir(parents=True, exist_ok=True)
        for suffix, pcm in (("pregunta", question_pcm), ("respuesta", reply_pcm)):
            with wave.open(str(self.output / f"cloud_{self.session_id}_{self.turns:03d}_{suffix}.wav"), "wb") as file:
                file.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
                file.writeframes(pcm)

    async def close(self):
        if self.task is not None:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
