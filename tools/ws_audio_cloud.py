"""Conversación de voz por nube para el prototipo de laboratorio (corre en la PC, no en el ESP).

Escucha todo el tiempo (sin wake word): detecta cuando hablás, espera un silencio, y hace STT -> LLM -> TTS -> PLAY
al ESP. Mide la latencia de cada etapa. Todo se configura en cloud_config.py; el servidor está en chat_nube.py.
"""
import asyncio
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

BLOCK = SAMPLE_RATE // 10          # 100 ms: unidad del detector de voz
ONSET_BLOCKS = 3                   # 0.3 s seguidos sobre el umbral = empezaste a hablar
END_QUIET_BLOCKS = 8               # 0.8 s de silencio cierran la frase
MAX_UTTERANCE_SAMPLES = 15 * SAMPLE_RATE
RING_SAMPLES = 20 * SAMPLE_RATE    # historial de audio recibido
MIN_THRESHOLD = 50                 # RMS mínimo para considerar "voz" (el mic crudo es muy bajo)
POST_PLAY_GUARD = SAMPLE_RATE      # tras el PLAY se ignora el mic 1 s (oye el propio parlante)
HISTORY_MESSAGES = 12


def _key(cfg, name):
    return getattr(cfg, name, "") or os.environ.get(name, "")


def _check(response, who):
    if not response.ok:
        raise RuntimeError(f"{who} HTTP {response.status_code}: {response.text[:300]}")
    return response


# -- clientes ---------------------------------------------------------------
class ElevenLabs:
    BASE = "https://api.elevenlabs.io/v1"
    name = "elevenlabs"

    def __init__(self, key, voice_id, tts_model, stt_model):
        self.voice_id, self.tts_model, self.stt_model = voice_id, tts_model, stt_model
        self.http = requests.Session()   # reusa la conexión TLS entre pedidos
        self.http.headers["xi-api-key"] = key

    def transcribe(self, pcm):
        wav = io.BytesIO()
        with wave.open(wav, "wb") as file:
            file.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
            file.writeframes(pcm)
        response = self.http.post(f"{self.BASE}/speech-to-text",
                                  data={"model_id": self.stt_model, "language_code": "es"},
                                  files={"file": ("voz.wav", wav.getvalue(), "audio/wav")}, timeout=30)
        return _check(response, "ElevenLabs STT").json().get("text", "").strip()

    def speak(self, text):
        """PCM s16le mono 16 kHz, el formato que espera PLAY."""
        response = self.http.post(f"{self.BASE}/text-to-speech/{self.voice_id}",
                                  params={"output_format": "pcm_16000"},
                                  json={"text": text, "model_id": self.tts_model}, timeout=30)
        pcm = _check(response, "ElevenLabs TTS").content
        return pcm[:len(pcm) - len(pcm) % 2]


class Deepgram:
    """STT Nova-3 y TTS Aura-2 (voces en español). Crédito inicial grande y sin vencimiento."""
    BASE = "https://api.deepgram.com/v1"
    name = "deepgram"

    def __init__(self, key, stt_model, voice):
        self.stt_model, self.voice = stt_model, voice
        self.http = requests.Session()
        self.http.headers["Authorization"] = f"Token {key}"

    def transcribe(self, pcm):
        wav = io.BytesIO()
        with wave.open(wav, "wb") as file:
            file.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
            file.writeframes(pcm)
        response = self.http.post(f"{self.BASE}/listen", params={"model": self.stt_model, "language": "es", "smart_format": "true"},
                                  data=wav.getvalue(), headers={"Content-Type": "audio/wav"}, timeout=30)
        channels = _check(response, "Deepgram STT").json()["results"]["channels"]
        return channels[0]["alternatives"][0]["transcript"].strip()

    def speak(self, text):
        """PCM s16le mono 16 kHz crudo (container=none), el formato que espera PLAY."""
        response = self.http.post(f"{self.BASE}/speak", params={"model": self.voice, "encoding": "linear16",
                                                                "sample_rate": SAMPLE_RATE, "container": "none"},
                                  json={"text": text}, timeout=30)
        pcm = _check(response, "Deepgram TTS").content
        return pcm[:len(pcm) - len(pcm) % 2]


class History:
    """Historial común: lista de (rol, texto) con rol 'user'/'assistant'; se olvida tras 2 min de silencio."""

    def __init__(self):
        self.turns = []
        self.last_used = 0.0

    def with_user(self, text):
        if time.monotonic() - self.last_used > 120:
            self.turns.clear()
        return self.turns + [("user", text)]

    def commit(self, turns, answer):
        self.turns = (turns + [("assistant", answer)])[-HISTORY_MESSAGES:]
        self.last_used = time.monotonic()


class Claude:
    def __init__(self, key, model, system):
        import anthropic
        self.client = anthropic.Anthropic(api_key=key)
        self.model, self.system, self.history = model, system, History()

    def reply(self, text):
        """Devuelve (respuesta, tokens_in, tokens_out)."""
        turns = self.history.with_user(text)
        response = self.client.messages.create(
            model=self.model, max_tokens=300, system=self.system,
            output_config={"effort": "low"},   # voz: prioriza latencia sobre razonamiento profundo
            messages=[{"role": role, "content": content} for role, content in turns])
        answer = "".join(block.text for block in response.content if block.type == "text").strip()
        if response.stop_reason == "refusal" or not answer:
            answer = "No puedo ayudarte con eso."
        self.history.commit(turns, answer)
        return answer, response.usage.input_tokens, response.usage.output_tokens


class Gemini:
    BASE = "https://generativelanguage.googleapis.com/v1beta/models"

    def __init__(self, key, model, system):
        self.model, self.system, self.history = model, system, History()
        self.http = requests.Session()
        self.http.headers["x-goog-api-key"] = key

    def reply(self, text):
        turns = self.history.with_user(text)
        body = dict(
            system_instruction={"parts": [{"text": self.system}]},
            contents=[{"role": "user" if role == "user" else "model", "parts": [{"text": content}]}
                      for role, content in turns],
            # Sin "thinking": para voz la latencia importa más, y el razonamiento se comería los tokens de salida.
            generationConfig={"maxOutputTokens": 300, "thinkingConfig": {"thinkingBudget": 0}})
        response = self.http.post(f"{self.BASE}/{self.model}:generateContent", json=body, timeout=30)
        data = _check(response, "Gemini").json()
        try:
            answer = "".join(part.get("text", "") for part in data["candidates"][0]["content"]["parts"]).strip()
        except (KeyError, IndexError):
            answer = ""
        answer = answer or "No pude responder eso."
        self.history.commit(turns, answer)
        usage = data.get("usageMetadata", {})
        return answer, usage.get("promptTokenCount", 0), usage.get("candidatesTokenCount", 0)


class CloudServices:
    """STT + LLM + TTS según cloud_config; compartido por las conexiones (la historia del LLM vive acá)."""

    def __init__(self, cfg):
        if cfg.SPEECH == "deepgram":
            if not _key(cfg, "DEEPGRAM_API_KEY"):
                raise ValueError("falta DEEPGRAM_API_KEY en tools/cloud_config.py (SPEECH = 'deepgram')")
            self.speech = Deepgram(_key(cfg, "DEEPGRAM_API_KEY"), cfg.DEEPGRAM_STT_MODEL, cfg.DEEPGRAM_VOICE)
            self.voice_name = cfg.DEEPGRAM_VOICE
        elif cfg.SPEECH == "elevenlabs":
            if not _key(cfg, "ELEVENLABS_API_KEY"):
                raise ValueError("falta ELEVENLABS_API_KEY en tools/cloud_config.py (SPEECH = 'elevenlabs')")
            self.speech = ElevenLabs(_key(cfg, "ELEVENLABS_API_KEY"), cfg.ELEVENLABS_VOICE_ID,
                                     cfg.ELEVENLABS_TTS_MODEL, cfg.ELEVENLABS_STT_MODEL)
            self.voice_name = cfg.ELEVENLABS_VOICE_ID
        else:
            raise ValueError('SPEECH debe ser "deepgram" o "elevenlabs"')
        provider = {"gemini": (Gemini, "GEMINI_API_KEY", cfg.GEMINI_MODEL),
                    "claude": (Claude, "ANTHROPIC_API_KEY", cfg.CLAUDE_MODEL)}.get(cfg.LLM)
        if provider is None:
            raise ValueError('LLM debe ser "gemini" o "claude"')
        cls, key_name, model = provider
        if not _key(cfg, key_name):
            raise ValueError(f"falta {key_name} en tools/cloud_config.py (LLM = {cfg.LLM!r})")
        self.llm = cls(_key(cfg, key_name), model, cfg.SYSTEM_PROMPT)
        self.llm_name = f"{cfg.LLM}:{model}"
        self.save_audio = bool(getattr(cfg, "SAVE_AUDIO", False))

    def start(self, output, session_id, send_pcm, play_done):
        return Conversation(self, output, session_id, send_pcm, play_done)


# -- una conexión -----------------------------------------------------------
class Conversation:
    """Estados: idle (espera voz) -> capturing (hasta silencio) -> busy (nube + reproducción) -> idle."""

    def __init__(self, services, output, session_id, send_pcm, play_done):
        self.services, self.output, self.session_id = services, Path(output), session_id
        self.send_pcm, self.play_done = send_pcm, play_done
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

    def feed(self, pcm):
        self.buf += pcm
        excess = len(self.buf) // 2 - RING_SAMPLES
        if excess > 0:
            del self.buf[:excess * 2]
            self.buf_start += excess
        if self.state == "idle":
            self._watch()
        if self.state == "capturing":
            self._scan()

    def _blocks_rms(self, first_sample):
        audio = np.frombuffer(bytes(self.buf), dtype="<i2")[max(first_sample - self.buf_start, 0):]
        count = len(audio) // BLOCK
        if count == 0:
            return audio, np.empty(0)
        blocks = audio[:count * BLOCK].astype(np.float32).reshape(count, BLOCK)
        return audio, np.sqrt((blocks ** 2).mean(axis=1))

    def _watch(self):
        """Busca el inicio de voz: ONSET_BLOCKS bloques seguidos sobre 3x el piso de ruido."""
        first = max(self.ignore_until, self.buf_start)
        _, ring = self._blocks_rms(self.buf_start)
        _, rms = self._blocks_rms(first)
        if len(ring) < 10 or len(rms) < ONSET_BLOCKS:
            return
        self.threshold = max(3 * float(np.percentile(ring, 20)), MIN_THRESHOLD)
        loud = rms > self.threshold
        for i in range(len(loud) - ONSET_BLOCKS + 1):
            if loud[i:i + ONSET_BLOCKS].all():
                self.cap_from = first + max(i - 3, 0) * BLOCK   # 0.3 s antes: no comerse el arranque de la frase
                self.state = "capturing"
                LOG.info("Voz detectada (umbral RMS %.0f); escuchando hasta silencio", self.threshold)
                return

    def _scan(self):
        audio, rms = self._blocks_rms(self.cap_from)
        loud = np.nonzero(rms > self.threshold)[0]
        if len(loud) and (len(rms) - 1 - loud[-1] >= END_QUIET_BLOCKS or len(audio) >= MAX_UTTERANCE_SAMPLES):
            self.state = "busy"
            self.task = asyncio.create_task(self._turn(audio[:(loud[-1] + 2) * BLOCK].tobytes(), time.monotonic()))

    async def _turn(self, pcm, t_end):
        record = dict(time=datetime.now(timezone.utc).isoformat(), session=self.session_id,
                      utterance_seconds=round(len(pcm) / (2 * SAMPLE_RATE), 2),
                      speech=self.services.speech.name, llm=self.services.llm_name)
        try:
            question = await asyncio.to_thread(self.services.speech.transcribe, pcm)
            t_stt = time.monotonic()
            record["question"] = question
            if not question:
                LOG.info("STT sin texto (ruido); se descarta")
                return
            answer, tokens_in, tokens_out = await asyncio.to_thread(self.services.llm.reply, question)
            t_llm = time.monotonic()
            speech = (await asyncio.to_thread(self.services.speech.speak, answer))[:MAX_PLAY_SECONDS * SAMPLE_RATE * 2]
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
            if self.services.save_audio:
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
            LOG.error("Falló el turno: %s. Se sigue escuchando", error)
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
