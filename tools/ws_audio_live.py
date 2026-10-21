"""STT continuo de laboratorio. La pausa es local al servidor, no al transporte.

RX valida y confirma todos los mensajes. Un worker procesa ventanas con memoria
limitada; nunca ejecutamos Whisper dentro del bucle que recibe y manda ACK.
"""
import asyncio
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import logging
import math
from pathlib import Path
import secrets
import struct
import sys
import time
import wave

from websockets.exceptions import ConnectionClosed
from ws_audio_protocol import HEADER
from ws_audio_session import Session
from ws_audio_speech import SAMPLE_RATE, contains_keyword, play_frames

LOG = logging.getLogger("audio_server")
BYTES_SECOND = SAMPLE_RATE * 2


def detection_beep():
    """200 ms de seno a 1 kHz, con rampas de 5 ms para evitar clicks."""
    samples = SAMPLE_RATE // 5
    ramp = SAMPLE_RATE // 200
    values = [round(12000 * min(1, i / ramp, (samples - 1 - i) / ramp)
                    * math.sin(2 * math.pi * 1000 * i / SAMPLE_RATE))
              for i in range(samples)]
    return struct.pack(f"<{samples}h", *values)


class LiveControl:
    """Estado compartido por teclado y conexión, modificado solo desde asyncio."""
    def __init__(self, enabled=False):
        self.enabled = enabled
        self.generation = 0
        self.listeners = set()
        self.connection = None
        # Sobrevive a la desconexión: un thread de inferencia no se puede matar.
        # No se inicia otra inferencia hasta que termine la anterior.
        self.inference = None

    def set_enabled(self, enabled):
        if self.enabled != enabled:
            self.enabled = enabled
            self.generation += 1
            for reset in tuple(self.listeners):
                reset()
        self.status()

    def status(self):
        LOG.info("STT %s | ESP %s | G: grabar/transcribir, P: pausar, E: estado, Ctrl+C: salir",
                 "ACTIVO" if self.enabled else "PAUSADO",
                 "conectada" if self.connection is not None else "desconectada")

    def command(self, key):
        if key.lower() == "g":
            self.set_enabled(True)
        elif key.lower() == "p":
            self.set_enabled(False)
        elif key.lower() == "e":
            self.status()


@dataclass(frozen=True)
class Window:
    pcm: bytes
    start_sample: int
    generation: int


class RollingWindows:
    def __init__(self, seconds=4, overlap=1):
        if not (math.isfinite(seconds) and math.isfinite(overlap)
                and 1 <= seconds <= 15 and 0 <= overlap < seconds):
            raise ValueError("ventana 1..15 s y 0 <= solapamiento < ventana")
        self.size = round(seconds * SAMPLE_RATE) * 2
        self.step = self.size - round(overlap * SAMPLE_RATE) * 2
        if self.step <= 0:
            raise ValueError("el avance de ventana debe ser al menos una muestra")
        self.reset()

    def reset(self):
        self.buffer = bytearray()
        self.start = None

    def add(self, pcm, start_sample, generation):
        if self.start is None:
            self.start = start_sample
        self.buffer.extend(pcm)
        while len(self.buffer) >= self.size:
            yield Window(bytes(self.buffer[:self.size]), self.start, generation)
            del self.buffer[:self.step]
            self.start += self.step // 2


class LiveProcessor:
    def __init__(self, control, recognizer, output, session_id, send_beep,
                 seconds=4, overlap=1, save_audio=False, wake_word="orbita", on_detect=None):
        self.control = control
        self.recognizer = recognizer
        self.output = Path(output)
        self.session_id = session_id
        self.send_beep = send_beep
        self.save_audio = save_audio
        self.wake_word = wake_word
        self.on_detect = on_detect   # modo nube: reemplaza al pitido
        self.windows = RollingWindows(seconds, overlap)
        self.queue = asyncio.Queue(maxsize=2)
        self.closed = False
        self.last_word_end = -1.0
        self.last_detection_end = -10.0
        self.dropped = 0
        self.transcribed = 0
        self.control.listeners.add(self.reset)
        self.task = asyncio.create_task(self.work())

    def reset(self):
        # Descartar parciales y pendientes: jamás transcribir audio de una pausa.
        self.windows.reset()
        self.last_word_end = -1.0
        self.last_detection_end = -10.0
        while not self.queue.empty():
            self.queue.get_nowait()
            self.queue.task_done()

    def add(self, pcm, start_sample, gap=False):
        if gap:
            self.windows.reset()  # no unir artificialmente palabras a ambos lados de un hueco
        if not self.control.enabled:
            return
        for window in self.windows.add(pcm, start_sample, self.control.generation):
            if self.queue.full():
                self.queue.get_nowait()
                self.queue.task_done()
                self.dropped += 1
                LOG.warning("STT atrasado: omitida una ventana; total=%d. RX y ACK siguen activos",
                            self.dropped)
            self.queue.put_nowait(window)

    def valid(self, window):
        return (not self.closed and self.control.enabled
                and window.generation == self.control.generation)

    def save(self, window, record):
        """I/O fuera del event loop; solo datos admitidos mientras estaba activo."""
        self.output.mkdir(parents=True, exist_ok=True)
        stem = f"stt_{self.session_id}_{window.generation}_{window.start_sample:012d}"
        if self.save_audio:
            with wave.open(str(self.output / (stem + ".wav")), "wb") as wav:
                wav.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
                wav.writeframes(window.pcm)
        if record["text"]:
            with (self.output / f"stt_{self.session_id}.jsonl").open("a", encoding="utf-8") as file:
                file.write(json.dumps(record, ensure_ascii=False) + "\n")

    async def work(self):
        while True:
            window = await self.queue.get()
            try:
                if not self.valid(window):
                    continue
                if self.control.inference is not None and not self.control.inference.done():
                    self.dropped += 1
                    LOG.warning("STT: inferencia anterior sigue terminando; ventana omitida")
                    continue
                started = time.monotonic()
                future = asyncio.create_task(asyncio.to_thread(self.recognizer.transcribe_words, window.pcm))
                # Consumir excepciones aun si el cliente se desconecta mientras corre el thread.
                future.add_done_callback(lambda task: task.exception() if not task.cancelled() else None)
                self.control.inference = future
                words = await asyncio.shield(future)
                if not self.valid(window):
                    continue
                offset = window.start_sample / SAMPLE_RATE
                # Whisper devuelve tiempos por palabra. El solapamiento no debe imprimir
                # ni disparar otra vez las palabras ya aceptadas de la ventana anterior.
                fresh = [word for word in words
                         if offset + word["end"] > self.last_word_end + 0.08]
                if fresh:
                    self.last_word_end = float(max(offset + word["end"] for word in fresh))
                text = "".join(word["word"] for word in fresh).strip()
                detected = contains_keyword(text, self.wake_word)
                # Una oscilación de timestamps cerca del solapamiento no repite el pitido.
                wake_times = [offset + word["end"] for word in fresh if contains_keyword(word["word"], self.wake_word)]
                wake_end = max(wake_times, default=offset)
                detected = bool(detected and wake_end > self.last_detection_end + 1.5)
                self.transcribed += 1
                record = dict(time=datetime.now(timezone.utc).isoformat(), session=self.session_id,
                              generation=window.generation, start_seconds=float(offset),
                              duration_seconds=len(window.pcm) / BYTES_SECOND,
                              inference_seconds=round(time.monotonic() - started, 3),
                              text=text, keyword=self.wake_word, detected=detected,
                              orbita=bool(detected and contains_keyword(self.wake_word, "orbita")),
                              dropped_windows=self.dropped)
                LOG.info("STT [%.2f s, CPU %.2f s]: %s", offset, record["inference_seconds"], text or "<sin texto nuevo>")
                if detected:
                    self.last_detection_end = wake_end
                    if self.on_detect:
                        LOG.info("Detectada %s", self.wake_word.upper())
                        await self.on_detect(wake_end)
                    else:
                        LOG.info("Detectada %s: enviando pitido por PLAY", self.wake_word.upper())
                        await self.send_beep()
                if self.valid(window) and (self.save_audio or text):
                    await asyncio.to_thread(self.save, window, record)
            except asyncio.CancelledError:
                raise
            except Exception:
                LOG.exception("Fallo en STT/guardado; se continua recibiendo audio")
            finally:
                self.queue.task_done()

    async def close(self):
        self.closed = True
        self.control.listeners.discard(self.reset)
        self.reset()
        self.task.cancel()
        await asyncio.gather(self.task, return_exceptions=True)


async def handle_live(connection, output, control, recognizer, seconds=4, overlap=1, save_audio=False, wake_word="orbita", cloud=None):
    """Una ESP por consola. Sin timeout de audio; ping/pong detecta cortes reales."""
    if control.connection is not None:
        await connection.close(code=1013, reason="STT de laboratorio ocupado por otra ESP")
        return
    control.connection = connection
    session = Session(output, record=False)
    send_lock = asyncio.Lock()
    processor = None
    conversation = None
    active_play = None
    play_deadline = 0.0
    play_done = asyncio.Event()   # lo marca play_done/play_reject del ESP; el modo nube espera el fin del parlante

    async def send_pcm(pcm):
        nonlocal active_play, play_deadline
        if active_play is not None and time.monotonic() > play_deadline:
            # No acumular respuestas ni bloquear todos los pitidos para siempre.
            async with send_lock:
                await connection.send(json.dumps(dict(type="play_stop", play_id=active_play)))
            active_play = None
            LOG.warning("Sin confirmacion del pitido en 5 s; se pide stop. Se reintentara en otra deteccion")
            return
        if active_play is not None:
            LOG.warning("Pitido omitido: el anterior sigue sin play_done/play_reject")
            return
        identity = secrets.randbits(64)
        active_play = f"{identity:016x}"
        play_deadline = time.monotonic() + 5 + len(pcm) / BYTES_SECOND
        play_done.clear()
        for frame in play_frames(identity, pcm):
            async with send_lock:
                await connection.send(json.dumps(frame) if isinstance(frame, dict) else frame)
            await asyncio.sleep(0)

    async def send_beep():
        await send_pcm(detection_beep())

    try:
        async for message in connection:
            if isinstance(message, str):
                obj = json.loads(message)
                if isinstance(obj, dict) and obj.get("type") in ("play_done", "play_reject"):
                    if obj.get("play_id") == active_play:
                        LOG.info("Respuesta del parlante: %s", obj)
                        active_play = None
                        play_done.set()
                    continue
            previous_seq = session.expected_seq
            sample_start = session.samples
            response = session.process(message)
            if response:
                async with send_lock:
                    await connection.send(response)
            if processor is None:
                if cloud:
                    conversation = cloud.start(output, session.id, send_pcm, play_done, save_audio)
                processor = LiveProcessor(control, recognizer, output, session.id, send_beep,
                                          seconds, overlap, save_audio, wake_word,
                                          conversation.on_detect if conversation else None)
                LOG.info("ESP conectada: sesion=%s fuente=%s; RX/ACK continuos", session.id, session.info["source"])
                control.status()
            elif isinstance(message, bytes):
                fields = HEADER.unpack_from(message)
                gap = bool(fields[7]) or (previous_seq is not None and fields[6] != previous_seq)
                if conversation:
                    conversation.feed(message[HEADER.size:], sample_start)
                processor.add(message[HEADER.size:], sample_start, gap)
                if session.messages % 240 == 0:
                    LOG.info("RX continuo: msgs=%d, %.1f s; STT=%s cola=%d omitidas=%d huecos=%d",
                             session.messages, session.samples / SAMPLE_RATE,
                             "activo" if control.enabled else "pausado", processor.queue.qsize(),
                             processor.dropped, session.gap_count)
    except ConnectionClosed:
        pass
    except (ValueError, OSError):
        LOG.exception("Sesion de audio invalida")
        await connection.close(code=1008, reason="Audio invalido")
    finally:
        if conversation is not None:
            await conversation.close()
        if processor is not None:
            await processor.close()
        session.close()
        control.connection = None
        LOG.info("ESP desconectada; servidor sigue esperando conexiones")


async def keyboard(control):
    """Windows: una tecla sin Enter. Otras terminales: comando y Enter."""
    control.status()
    if not sys.stdin.isatty():
        LOG.warning("Sin terminal interactiva: usar --start-recording para activar STT al arrancar")
        await asyncio.Event().wait()
    if sys.platform == "win32":
        import msvcrt
        while True:
            if msvcrt.kbhit():
                key = msvcrt.getwch()
                if key == "\x03":
                    return
                if key in ("\x00", "\xe0"):
                    msvcrt.getwch()  # tecla especial: consumir su segundo código
                else:
                    control.command(key)
            await asyncio.sleep(0.05)
    else:
        loop = asyncio.get_running_loop()
        commands = asyncio.Queue()
        loop.add_reader(sys.stdin, lambda: commands.put_nowait(sys.stdin.readline()))
        try:
            while True:
                line = await commands.get()
                if not line:
                    return
                control.command(line.strip())
        finally:
            loop.remove_reader(sys.stdin)
