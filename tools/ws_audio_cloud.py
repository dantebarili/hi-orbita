"""Conversación de voz por nube para el prototipo de laboratorio (corre en la PC, no en el ESP).

Dentro de una conversación: el audio va en vivo al STT de ElevenLabs (scribe_v2_realtime), que cierra la frase por
silencio y devuelve el texto -> LLM -> TTS -> PLAY al ESP. Una sesión de STT por turno: se cierra al llegar el texto
(no se paga mientras piensa el LLM ni mientras suena el parlante) y la siguiente lleva la última respuesta como
contexto (`previous_text`). Mide la latencia de cada etapa. Config en cloud_config.py; servidor en servidor_wakeword.py.
"""
import asyncio
import base64
import json
import logging
import os
import re
import threading
import time
import wave
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlencode

import numpy as np
import requests
from scipy.signal import butter, sosfilt
from websockets.asyncio.client import connect

from ws_audio_speech import MAX_PLAY_SECONDS, SAMPLE_RATE

LOG = logging.getLogger("audio_cloud")

STT_URL = "wss://api.elevenlabs.io/v1/speech-to-text/realtime"
STT_OPEN_TIMEOUT_S = 5
STT_RETRY_S = 2.0                  # si la sesión de STT falla, se reintenta al recibir audio después de esto
CONTEXT_CHARS = 50                 # previous_text: "funciona mejor con menos de 50 caracteres" (doc de ElevenLabs)
MAX_UTTERANCE_S = 15               # red de seguridad: si el STT no cierra la frase, se usa el último parcial
HISTORY_MESSAGES = 12
# --- Audio de la respuesta antes de mandarlo al parlante (01/11: el parlante saturaba con la voz cruda) ---
# StreamNormalizer: pasaaltos (el parlante chico no da graves pero gasta excursion en ellos) + ganancia fija
# PLAY_GAIN_DB + limitador a techo fijo PLAY_STREAM_PEAK_DBFS. Pedazo por pedazo (TTS en streaming).
PLAY_GAIN_DB = 0.0                 # volumen promedio extra. 06/11: 0, el volumen lo da el ampli (GAIN a GND, 12 dB)
PLAY_HPF_HZ = 400                  # corte del pasaaltos (2.o orden). 06/11: 200 -> 400, el parlante no da graves
                                   # y distorsiona por ellos; la voz se entiende igual (telefono: 300-3400 Hz)
# --- Compuerta antes del STT (06/11): voces de fondo que no dejaban cerrar la frase ---
GATE_BLOCK = SAMPLE_RATE // 50     # 20 ms
GATE_HOLD_BLOCKS = 15              # 300 ms abierta despues del ultimo bloque fuerte: no corta el final de las palabras
PLAY_PEAK_DBFS = -1.0              # techo absoluto: nunca se pasa (no recorta digital)
# TTS en streaming (06/11): no hay respuesta entera para medir el RMS, el limitador usa un techo fijo.
# Ajustar mirando el log de cada respuesta ("pico de entrada", "% limitado").
PLAY_STREAM_PEAK_DBFS = -6.0
TTS_CHUNK_BYTES = 3200             # 100 ms de audio por pedazo leido del stream
PLAY_HPF_SOS = butter(2, PLAY_HPF_HZ, btype='highpass', fs=SAMPLE_RATE, output='sos')
LIMIT_BLOCK = SAMPLE_RATE // 200   # 5 ms: resolucion del limitador


class StreamNormalizer:
    """Audio del TTS antes del parlante, en pedazos (TTS en streaming: no se conoce el total).
    - Pasaaltos con estado (sigue de un pedazo al otro).
    - Ganancia fija (PLAY_GAIN_DB): no hay RMS de la respuesta entera para compensar.
    - Limitador a techo fijo (PLAY_STREAM_PEAK_DBFS) en bloques de 5 ms, mirando 1 bloque adelante: la ganancia ya
      bajo cuando llega el pico. Dentro de cada bloque la ganancia va en rampa (sin clicks); sube despacio (~15 ms).
    Retiene 1 bloque (5 ms) para mirar adelante; flush() entrega el resto."""

    def __init__(self):
        self.zi = np.zeros((PLAY_HPF_SOS.shape[0], 2))
        self.gain = 10 ** (PLAY_GAIN_DB / 20)
        self.ceiling = 32767 * 10 ** (min(PLAY_STREAM_PEAK_DBFS, PLAY_PEAK_DBFS) / 20)
        self.pend = np.zeros(0)       # filtrado, esperando el bloque siguiente
        self.odd = b""                # byte suelto si un pedazo llego con largo impar
        self.g_prev = 1.0             # ganancia al final del ultimo bloque entregado
        self.t_prev = 1.0             # ganancia pedida por el ultimo bloque entregado
        self.peak_in = self.limited = self.blocks = 0

    def push(self, pcm):
        data = self.odd + pcm
        self.odd = data[len(data) - len(data) % 2:]
        x = np.frombuffer(data[:len(data) - len(data) % 2], dtype="<i2").astype(np.float64)
        if len(x):
            self.peak_in = max(self.peak_in, np.abs(x).max())
            y, self.zi = sosfilt(PLAY_HPF_SOS, x, zi=self.zi)
            self.pend = np.concatenate([self.pend, y * self.gain])
        return self._process(final=False)

    def flush(self):
        return self._process(final=True)

    def _process(self, final):
        B = LIMIT_BLOCK
        nb = -(-len(self.pend) // B) if final else len(self.pend) // B
        n_out = nb if final else max(nb - 1, 0)
        if n_out == 0:
            return b""
        x = np.concatenate([self.pend, np.zeros(nb * B - len(self.pend))]) if final else self.pend[:nb * B]
        env = np.abs(x.reshape(-1, B)).max(axis=1)
        target = np.minimum(1.0, self.ceiling / np.maximum(env, 1e-9))
        partes = []
        for i in range(n_out):
            pedido = min(self.t_prev, target[i], target[i + 1] if i + 1 < nb else 1.0)
            g = min(pedido, 0.7 * self.g_prev + 0.3 * pedido)       # baja ya, sube despacio
            partes.append(x[i * B:(i + 1) * B] * (self.g_prev + (g - self.g_prev) * np.arange(1, B + 1) / B))
            self.limited += g < 0.999
            self.blocks += 1
            self.g_prev, self.t_prev = g, target[i]
        y = np.concatenate(partes)
        if final:
            y, self.pend = y[:len(self.pend)], np.zeros(0)
        else:
            self.pend = self.pend[n_out * B:]
        return np.clip(np.round(y), -self.ceiling, self.ceiling).astype("<i2").tobytes()


def _key(cfg, name):
    return getattr(cfg, name, "") or os.environ.get(name, "")


def _check(response, who):
    if not response.ok:
        raise RuntimeError(f"{who} HTTP {response.status_code}: {response.text[:300]}")
    return response


# -- clientes ---------------------------------------------------------------
class ScribeRealtime:
    """STT en vivo de ElevenLabs. `open()` crea una sesión (un WebSocket) por turno."""

    def __init__(self, key, model, vad_silence_s, vad_threshold=None):
        if not 0.192 <= vad_silence_s <= 3.0:
            raise ValueError("STT_VAD_SILENCE_S tiene que estar entre 0.192 y 3.0 s (límite de ElevenLabs)")
        params = [("model_id", model), ("audio_format", f"pcm_{SAMPLE_RATE}"), ("language_code", "es"),
                  ("commit_strategy", "vad"), ("vad_silence_threshold_secs", vad_silence_s)]
        if vad_threshold is not None:
            if not 0.1 <= vad_threshold <= 0.9:
                raise ValueError("STT_VAD_THRESHOLD tiene que estar entre 0.1 y 0.9 (límite de ElevenLabs)")
            params.append(("vad_threshold", vad_threshold))
        self.url = f"{STT_URL}?{urlencode(params)}"
        self.headers = {"xi-api-key": key}
        self.name = model

    def open(self, previous_text, on_event):
        return SttSession(self, previous_text, on_event)


class SttSession:
    """Una sesión de STT en vivo. `send()` encola audio (se manda apenas abre); los eventos llegan a
    on_event(sesión, tipo, texto) con tipo "open" (texto = ms que tardó en abrir), "partial", "committed" o "error"."""

    def __init__(self, stt, previous_text, on_event):
        self.queue = asyncio.Queue()
        self.on_event = on_event
        self.task = asyncio.create_task(self._run(stt, previous_text))

    def send(self, pcm):
        self.queue.put_nowait(pcm)

    async def close(self):
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)

    async def _run(self, stt, previous_text):
        t0 = time.monotonic()
        try:
            async with connect(stt.url, additional_headers=stt.headers, open_timeout=STT_OPEN_TIMEOUT_S,
                               max_size=2 ** 20) as ws:
                started = json.loads(await asyncio.wait_for(ws.recv(), STT_OPEN_TIMEOUT_S))
                if started.get("message_type") != "session_started":
                    raise RuntimeError(f"esperaba session_started y llegó {str(started)[:200]}")
                self.on_event(self, "open", f"{(time.monotonic() - t0) * 1000:.0f}")
                tasks = {asyncio.create_task(self._send(ws, previous_text)), asyncio.create_task(self._receive(ws))}
                try:
                    done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                    for task in done:
                        task.result()          # propaga el error de la que terminó
                finally:
                    for task in tasks:
                        task.cancel()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.on_event(self, "error", str(error) or type(error).__name__)

    async def _send(self, ws, previous_text):
        first = True
        while True:
            pcm = await self.queue.get()
            msg = {"message_type": "input_audio_chunk", "audio_base_64": base64.b64encode(pcm).decode(),
                   "commit": False, "sample_rate": SAMPLE_RATE}
            if first and previous_text:
                msg["previous_text"] = previous_text   # solo en el primer trozo: después da error
            first = False
            await ws.send(json.dumps(msg))

    async def _receive(self, ws):
        async for raw in ws:
            msg = json.loads(raw)
            kind = msg.get("message_type", "")
            if kind in ("partial_transcript", "committed_transcript"):
                self.on_event(self, kind.split("_")[0], msg.get("text", ""))
            elif kind == "warning":
                LOG.warning("STT: %s", msg.get("warning"))
            elif "error" in msg:
                raise RuntimeError(f"{kind}: {msg['error']}")
        raise RuntimeError("ElevenLabs cerró la sesión")


class ElevenLabsTTS:
    BASE = "https://api.elevenlabs.io/v1"

    def __init__(self, key, voice_id, model):
        self.voice_id, self.model = voice_id, model
        self.http = requests.Session()   # reusa la conexión TLS entre pedidos
        self.http.headers["xi-api-key"] = key

    def speak(self, text):
        """PCM s16le mono 16 kHz, el formato que espera PLAY."""
        response = self.http.post(f"{self.BASE}/text-to-speech/{self.voice_id}",
                                  params={"output_format": "pcm_16000"},
                                  json={"text": text, "model_id": self.model}, timeout=30)
        pcm = _check(response, "ElevenLabs TTS").content
        return pcm[:len(pcm) - len(pcm) % 2]

    def stream(self, text, stop):
        """Igual que speak(), pero en pedazos a medida que ElevenLabs los genera (endpoint /stream).
        Bloqueante: se corre en un thread. `stop` (threading.Event) corta la descarga."""
        with self.http.post(f"{self.BASE}/text-to-speech/{self.voice_id}/stream",
                            params={"output_format": "pcm_16000"},
                            json={"text": text, "model_id": self.model}, timeout=30, stream=True) as response:
            if response.status_code != 200:
                _check(response, "ElevenLabs TTS")
            for chunk in response.iter_content(chunk_size=TTS_CHUNK_BYTES):
                if stop.is_set():
                    return
                if chunk:
                    yield chunk


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
    TIMEOUT_S = 6          # si el modelo tarda más (saturado), se pasa al de respaldo
    RETRY_CODES = (429, 500, 503)   # cuota agotada o modelo saturado: probar el siguiente

    def __init__(self, key, model, system, fallbacks=()):
        self.model, self.system, self.history = model, system, History()
        self.models = [model, *fallbacks]
        self.http = requests.Session()
        self.http.headers["x-goog-api-key"] = key

    def _post(self, body):
        """Prueba los modelos en orden; salta al siguiente si tarda o está saturado/sin cuota."""
        error = None
        for model in self.models:
            try:
                response = self.http.post(f"{self.BASE}/{model}:generateContent", json=body,
                                          timeout=(5, self.TIMEOUT_S))
            except requests.Timeout:
                error = RuntimeError(f"Gemini {model}: más de {self.TIMEOUT_S} s")
                LOG.warning("%s; pruebo el siguiente modelo", error)
                continue
            if response.status_code in self.RETRY_CODES:
                error = RuntimeError(f"Gemini {model} HTTP {response.status_code}")
                LOG.warning("%s; pruebo el siguiente modelo", error)
                continue
            return _check(response, f"Gemini {model}")
        raise error

    def reply(self, text):
        turns = self.history.with_user(text)
        body = dict(
            system_instruction={"parts": [{"text": self.system}]},
            contents=[{"role": "user" if role == "user" else "model", "parts": [{"text": content}]}
                      for role, content in turns],
            # Sin "thinking": para voz la latencia importa más, y el razonamiento se comería los tokens de salida.
            generationConfig={"maxOutputTokens": 300, "thinkingConfig": {"thinkingLevel": "minimal"}})
        data = self._post(body).json()
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
        if getattr(cfg, "SPEECH", "elevenlabs") != "elevenlabs":
            LOG.warning("SPEECH = %r ya no existe (se sacó Deepgram): se usa ElevenLabs", cfg.SPEECH)
        key = _key(cfg, "ELEVENLABS_API_KEY")
        if not key:
            raise ValueError("falta ELEVENLABS_API_KEY en tools/cloud_config.py")
        vad_threshold = getattr(cfg, "STT_VAD_THRESHOLD", None)
        self.stt = ScribeRealtime(key, getattr(cfg, "STT_MODEL", "scribe_v2_realtime"),
                                  float(getattr(cfg, "STT_VAD_SILENCE_S", 0.8)),
                                  None if vad_threshold is None else float(vad_threshold))
        gate = getattr(cfg, "STT_GATE_DBFS", None)
        self.gate_dbfs = None if gate is None else float(gate)
        self.stt_context = _context(getattr(cfg, "STT_CONTEXT", ""))
        self.tts = ElevenLabsTTS(key, cfg.ELEVENLABS_VOICE_ID, cfg.ELEVENLABS_TTS_MODEL)
        self.voice_name = cfg.ELEVENLABS_VOICE_ID
        provider = {"gemini": (Gemini, "GEMINI_API_KEY", cfg.GEMINI_MODEL),
                    "claude": (Claude, "ANTHROPIC_API_KEY", cfg.CLAUDE_MODEL)}.get(cfg.LLM)
        if provider is None:
            raise ValueError('LLM debe ser "gemini" o "claude"')
        cls, key_name, model = provider
        if not _key(cfg, key_name):
            raise ValueError(f"falta {key_name} en tools/cloud_config.py (LLM = {cfg.LLM!r})")
        if cls is Gemini:
            self.llm = Gemini(_key(cfg, key_name), model, cfg.SYSTEM_PROMPT, getattr(cfg, "GEMINI_RESPALDO", ()))
        else:
            self.llm = cls(_key(cfg, key_name), model, cfg.SYSTEM_PROMPT)
        self.llm_name = f"{cfg.LLM}:{model}"
        self.save_audio = bool(getattr(cfg, "SAVE_AUDIO", False))


def _context(text):
    """Últimos CONTEXT_CHARS caracteres, cortando en una palabra entera (para previous_text)."""
    text = " ".join(text.split())
    if len(text) <= CONTEXT_CHARS:
        return text
    cut = text[-CONTEXT_CHARS:]
    return cut[cut.find(" ") + 1:] if " " in cut else cut


# -- una conexión -----------------------------------------------------------
class Conversation:
    """Estados: listening (el audio va al STT en vivo) -> busy (LLM + TTS + reproducción) -> listening.
    El cierre de la frase lo decide el STT (silencio de STT_VAD_SILENCE_S); acá no hay detector de voz propio."""

    def __init__(self, services, output, session_id, send_pcm, play_done):
        self.services, self.output, self.session_id = services, Path(output), session_id
        self.send_pcm, self.play_done = send_pcm, play_done
        self.clock = time.monotonic   # inyectable en los tests
        self.state = "listening"
        self.received = 0             # muestras recibidas desde el inicio de la conversación
        self.ignore_until = 0         # las muestras anteriores a esta no van al STT (las subclases lo fijan)
        self.stt = None               # sesión de STT abierta (una por turno)
        self.stt_retry_at = 0.0
        self.stt_open_ms = None
        self.previous_text = services.stt_context
        self.pcm = bytearray()        # audio mandado al STT en este turno (para SAVE_AUDIO)
        self.partial = ""
        self.t_text = None            # última vez que cambió el texto parcial (~ fin de habla)
        self.t_speech = None          # primer parcial con texto del turno
        self.last_activity = self.clock()
        self.turns = 0
        self.task = None
        self.gate_hold = 0            # bloques que la compuerta sigue abierta
        self.gate_n = self.gate_cerrados = 0
        self.gate_max_db = -120.0     # nivel del bloque mas fuerte del turno (para afinar STT_GATE_DBFS)

    def _gate(self, pcm):
        """Compuerta por nivel (STT_GATE_DBFS): los bloques de 20 ms mas bajos que el umbral van al STT como
        silencio. Las voces de fondo (mas lejos, mas bajas) dejan de mantener abierta la frase."""
        umbral = self.services.gate_dbfs
        if umbral is None:
            return pcm
        x = np.frombuffer(pcm, dtype="<i2").copy()
        for i in range(0, len(x), GATE_BLOCK):
            b = x[i:i + GATE_BLOCK].astype(np.float64)
            db = 10 * np.log10(np.mean(b ** 2) + 1e-9) - 90.31        # dBFS (0 dB = seno a fondo de escala x raiz 2)
            self.gate_n += 1
            self.gate_max_db = max(self.gate_max_db, db)
            if db >= umbral:
                self.gate_hold = GATE_HOLD_BLOCKS
            elif self.gate_hold > 0:
                self.gate_hold -= 1
            else:
                x[i:i + GATE_BLOCK] = 0
                self.gate_cerrados += 1
        return x.tobytes()

    def feed(self, pcm):
        start, self.received = self.received, self.received + len(pcm) // 2
        if self.state != "listening":
            return                    # pensando o sonando: no se manda (ni se paga)
        pcm = pcm[max(self.ignore_until - start, 0) * 2:]
        if not pcm:
            return
        if self.stt is None:
            if self.clock() < self.stt_retry_at:
                return
            self.stt = self.services.stt.open(self.previous_text, self._on_stt)
        pcm = self._gate(pcm)
        self.stt.send(pcm)
        self.pcm += pcm
        if self.t_speech is not None and self.clock() - self.t_speech > MAX_UTTERANCE_S and self.partial:
            LOG.warning("Más de %d s sin cierre del STT: se usa el último parcial", MAX_UTTERANCE_S)
            self._committed(self.partial)

    def _on_stt(self, session, kind, text):
        if session is not self.stt:
            return                    # evento de una sesión vieja
        now = self.clock()
        if kind == "open":
            self.stt_open_ms = int(text)
            LOG.info("STT en vivo abierta en %s ms", text)
        elif kind == "partial":
            if text and text != self.partial:
                self.partial, self.t_text, self.last_activity = text, now, now
                self.t_speech = self.t_speech or now
        elif kind == "committed":
            self._committed(text)
        elif kind == "error":
            LOG.error("STT en vivo: %s. Reintento en %.0f s (se pierde lo dicho hasta acá)", text, STT_RETRY_S)
            self.stt, self.stt_retry_at = None, now + STT_RETRY_S
            self.partial, self.t_speech, self.pcm = "", None, bytearray()

    def _committed(self, text):
        if self.state != "listening":
            return
        question = re.sub(r"\[[^\]]*\]", "", text).strip()   # ElevenLabs marca "[ruido]", "[interferencia]": no es una pregunta
        question = self.clean_question(question)
        t_end = self.t_text or self.clock()
        pcm = bytes(self.pcm)
        self.partial, self.t_speech, self.pcm = "", None, bytearray()
        self.last_activity = self.clock()
        if self.services.gate_dbfs is not None and self.gate_n:
            LOG.info("Compuerta %.0f dBFS: %.0f %% del audio en silencio, bloque mas fuerte %.0f dBFS",
                     self.services.gate_dbfs, 100 * self.gate_cerrados / self.gate_n, self.gate_max_db)
            self.gate_n = self.gate_cerrados = 0
            self.gate_max_db = -120.0
        if not question:
            LOG.info("STT sin pregunta (%r): sigo escuchando", text)
            return
        self.state = "busy"
        stt, self.stt = self.stt, None
        self.task = asyncio.create_task(self._turn(stt, question, pcm, t_end))

    async def _turn(self, stt, question, pcm, t_end):
        t_stt = self.clock()
        record = dict(time=datetime.now(timezone.utc).isoformat(), session=self.session_id,
                      utterance_seconds=round(len(pcm) / (2 * SAMPLE_RATE), 2), stt=self.services.stt.name,
                      llm=self.services.llm_name, question=question, stt_open_ms=self.stt_open_ms)
        try:
            if stt is not None:
                await stt.close()     # libera la sesión: no se paga mientras piensa ni mientras suena
            answer, tokens_in, tokens_out = await asyncio.to_thread(self.services.llm.reply, question)
            t_llm = self.clock()
            # TTS en streaming: cada pedazo sale a la ESP apenas llega (la ESP junta 400 ms antes de sonar).
            pedazos, norm, t_first = [], StreamNormalizer(), []
            await self.send_pcm(self._tts_stream(answer, norm, pedazos, t_first))
            t_sent = self.clock()
            if not t_first:
                raise RuntimeError("el TTS no devolvió audio")
            speech = b"".join(pedazos)
            self.previous_text = _context(answer)    # contexto de la próxima sesión: "¿y la dosis?" se entiende
            record.update(answer=answer, tokens_in=tokens_in, tokens_out=tokens_out,
                          reply_seconds=round(len(speech) / (2 * SAMPLE_RATE), 2),
                          stt_s=round(t_stt - t_end, 3), llm_s=round(t_llm - t_stt, 3),
                          tts_s=round(t_first[0] - t_llm, 3), send_s=round(t_sent - t_first[0], 3),
                          total_s=round(t_first[0] - t_end, 3),
                          tts_peak_in_dbfs=round(20 * np.log10(max(norm.peak_in, 1) / 32768), 1),
                          tts_limited_pct=round(100 * norm.limited / max(norm.blocks, 1), 1))
            LOG.info("Turno %d: %r -> %r | STT %.2f s, LLM %.2f s, TTS al 1.er audio %.2f s, envío %.2f s | "
                     "fin de habla -> 1.er PLAY: %.2f s (el STT incluye el silencio que espera para cerrar) | "
                     "TTS pico de entrada %.1f dBFS, %.0f %% limitado a %.0f dBFS", self.turns + 1, question, answer,
                     record["stt_s"], record["llm_s"], record["tts_s"], record["send_s"], record["total_s"],
                     record["tts_peak_in_dbfs"], record["tts_limited_pct"], PLAY_STREAM_PEAK_DBFS)
            if self.services.save_audio:
                await asyncio.to_thread(self._save_wavs, pcm, speech)
            # Esperar el fin de la reproducción para no tomar el propio parlante como pregunta nueva.
            try:
                await asyncio.wait_for(self.play_done.wait(), len(speech) / (2 * SAMPLE_RATE) + 10)
            except asyncio.TimeoutError:
                LOG.warning("El ESP no confirmó el fin de la reproducción")
        except asyncio.CancelledError:
            raise
        except Exception as error:
            record["error"] = str(error)
            LOG.error("Falló el turno: %s. Se sigue escuchando", error)
        finally:
            self.turns += 1
            self.ignore_until = self.received    # lo que llegó mientras sonaba no cuenta
            self.state = "listening"
            self.last_activity = self.clock()
            await asyncio.to_thread(self._log, record)

    async def _tts_stream(self, text, norm, pedazos, t_first):
        """Generador async: baja el TTS en un thread (requests bloquea) y entrega PCM normalizado de a pedazos.
        Guarda lo entregado en `pedazos` y el instante del primero en `t_first`. Corta en MAX_PLAY_SECONDS."""
        loop = asyncio.get_running_loop()
        cola, stop = asyncio.Queue(), threading.Event()

        def bajar():
            try:
                for chunk in self.services.tts.stream(text, stop):
                    loop.call_soon_threadsafe(cola.put_nowait, chunk)
            except Exception as error:
                loop.call_soon_threadsafe(cola.put_nowait, error)
            finally:
                loop.call_soon_threadsafe(cola.put_nowait, None)

        hilo = loop.run_in_executor(None, bajar)
        quedan = MAX_PLAY_SECONDS * SAMPLE_RATE * 2
        try:
            while quedan > 0:
                item = await cola.get()
                if isinstance(item, Exception):
                    raise item
                out = norm.flush() if item is None else norm.push(item)
                out = out[:quedan]
                if out:
                    if not t_first:
                        t_first.append(self.clock())
                    pedazos.append(out)
                    quedan -= len(out)
                    yield out
                if item is None:
                    break
            else:
                LOG.warning("Respuesta de más de %d s: se corta", MAX_PLAY_SECONDS)
        finally:
            stop.set()                # si se cortó antes (error, límite), que el thread deje de bajar
            await hilo

    def clean_question(self, question):
        """Gancho para subclases: ajustar la pregunta antes del LLM ("" = no hay pregunta)."""
        return question

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
        if self.stt is not None:
            stt, self.stt = self.stt, None
            await stt.close()
