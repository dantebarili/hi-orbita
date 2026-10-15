"""Servidor de audio local del laboratorio (hace de backend mientras no exista el real).

Ejecutar: python tools/ws_audio_server.py. Guarda el audio continuo en WAV + JSON en
tools/prueba_websocket (ignorado por Git). Ctrl+C conserva tomas incompletas.
Demos de respuesta por el parlante (una por conexion, elegir una):
  --replay-window 2 12     devuelve ese intervalo del audio recibido (demo actual).
  --play-wav archivo.wav   envia un WAV conocido.
  --stt-window-seconds 8   transcribe una ventana y, si oye "Orbita", responde con --reply-wav.
"""
import argparse
import asyncio
import json
import logging
import math
from pathlib import Path
import secrets
import wave
import time

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed
from ws_audio_protocol import Recording
from ws_audio_session import Session
from ws_audio_protocol import HEADER
from ws_audio_speech import (SAMPLE_RATE, AudioWindow, FasterWhisperRecognizer,
                             contains_orbita, load_reply_wav, play_frames)

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


def playback_report(message):
    """Parse terminal reports from the ESP without treating them as audio protocol errors."""
    if not isinstance(message, str):
        return None
    try:
        obj = json.loads(message)
    except ValueError:
        return None
    if not isinstance(obj, dict) or obj.get("type") not in ("play_done", "play_reject"):
        return None
    play_id = obj.get("play_id")
    if not isinstance(play_id, str) or len(play_id) != 16 or any(c not in "0123456789abcdefABCDEF" for c in play_id):
        raise ValueError("playback report con play_id invalido")
    return obj


async def handle(connection, output, segment_seconds=None, stt_window_seconds=None,
                 recognizer=None, reply_pcm=None, replay_window=None, play_pcm=None):
    """Decide v1 (toma con start/end) o v2 (sesion continua con hello) por el primer mensaje."""
    try:
        first = await asyncio.wait_for(connection.recv(), timeout=30)
    except (ConnectionClosed, asyncio.TimeoutError):
        return
    if is_hello(first):
        await handle_session(connection, output, first, segment_seconds,
                              stt_window_seconds, recognizer, reply_pcm, replay_window, play_pcm)
    else:
        if replay_window is not None or play_pcm is not None:
            LOG.warning("La devolucion de audio requiere una sesion continua v2 (hello)")
        await handle_take(connection, output, first)


async def handle_session(connection, output, first, segment_seconds=None,
                         stt_window_seconds=None, recognizer=None, reply_pcm=None,
                         replay_window=None, play_pcm=None):
    key = str(output)
    kwargs = {} if segment_seconds is None else dict(segment_samples=int(segment_seconds * 16000))
    session = Session(output, previous=LAST_SESSION.get(key), **kwargs)
    LAST_SESSION[key] = session
    speech_window = AudioWindow(stt_window_seconds) if stt_window_seconds else None
    replay_capture = (AudioWindow(replay_window[1] - replay_window[0], replay_window[0])
                      if replay_window is not None else None)
    send_lock = asyncio.Lock()
    background_tasks = set()
    active_play_id = None
    LOG.info("Conectado (sesion continua): %s", connection.remote_address)
    reason = "conexion cerrada"

    async def send_json(obj):
        async with send_lock:
            await connection.send(json.dumps(obj, separators=(",", ":")))

    async def send_playback(pcm, label):
        """Send a burst of PLAY frames while leaving RX and ACKs able to run."""
        nonlocal active_play_id
        play_id = secrets.randbits(64)
        identity = active_play_id = f"{play_id:016x}"
        frames_sent = 0
        try:
            for frame in play_frames(play_id, pcm):
                # El lock es por mensaje: los ACK pueden intercalarse entre los PLAY.
                async with send_lock:
                    await connection.send(json.dumps(frame, separators=(",", ":"))
                                          if isinstance(frame, dict) else frame)
                if isinstance(frame, bytes):
                    frames_sent += 1
                # send() no siempre cede si el buffer TCP tiene lugar.
                await asyncio.sleep(0)
            LOG.info("%s enviado: play_id=%s, %.3f s, %d bytes PCM, %d frames PLAY; esperando play_done",
                     label, identity, len(pcm) / (SAMPLE_RATE * 2), len(pcm), frames_sent)
        except (ConnectionClosed, OSError, ValueError) as error:
            LOG.warning("No se pudo enviar %s: %s", label, error)

    def start_background(coroutine):
        task = asyncio.create_task(coroutine)
        background_tasks.add(task)
        task.add_done_callback(background_tasks.discard)

    async def analyze_window(pcm, at_index):
        try:
            transcript = await asyncio.to_thread(recognizer.transcribe, pcm)
            LOG.info("STT (ventana de %.1f s): %s", stt_window_seconds, transcript or "<sin texto>")
            if not contains_orbita(transcript):
                LOG.info("No se detecto la palabra Órbita en esta ventana")
                return
            LOG.info("Detectada la palabra Órbita; devolviendo audio de respuesta")
            await send_json({"type": "orbita", "id": session.id, "at_index": at_index})
            if reply_pcm is None:
                LOG.warning("No hay WAV de respuesta; se detecto la palabra pero no se envio audio")
                return
            await send_playback(reply_pcm, "Audio de respuesta")
        except (ConnectionClosed, OSError, ValueError) as error:
            LOG.warning("No se pudo analizar/enviar la respuesta de voz: %s", error)
        except Exception:
            LOG.exception("Fallo inesperado en el prototipo STT/TTS")

    try:
        message = first
        while True:
            report = playback_report(message)
            if report is not None:
                received_id = report["play_id"].lower()
                if received_id == active_play_id:
                    if report["type"] == "play_done":
                        LOG.info("Playback terminado: %s (%s, %s ms)", received_id,
                                 report.get("reason", "?"), report.get("played_ms", "?"))
                    else:
                        LOG.warning("ESP rechazo el audio de respuesta: %s", received_id)
                    active_play_id = None
                else:
                    LOG.info("Reporte de playback viejo ignorado: %s", received_id)
                message = await asyncio.wait_for(connection.recv(), timeout=30)
                continue

            response = session.process(message)
            if message is first:
                LOG.info("Sesion %s (%s)", session.id, session.info["source"])
                if replay_capture is not None:
                    LOG.info("Prueba PLAY: guardando segundos %.3f a %.3f del PCM recibido; devolucion unica por conexion",
                             *replay_window)
                async with send_lock:
                    await connection.send(response)
            elif response:
                async with send_lock:
                    await connection.send(response)
            if message is first and play_pcm is not None:
                LOG.info("Demo --play-wav: envio unico por conexion")
                start_background(send_playback(play_pcm, "WAV conocido"))
            if isinstance(message, bytes) and speech_window is not None and not speech_window.complete:
                # Session.process validated CRC and frame layout before this PCM enters STT.
                window_pcm = speech_window.add(message[HEADER.size:])
                if window_pcm is not None:
                    start_background(analyze_window(window_pcm, max(0, session.messages - 1)))
            if isinstance(message, bytes) and replay_capture is not None and not replay_capture.complete:
                # Solo PCM validado por Session.process: se recorta con precision de muestra.
                replay_pcm = replay_capture.add(message[HEADER.size:])
                if replay_pcm is not None:
                    reference = Path(output) / f"replay_{session.stamp}_{session.id}.wav"
                    with wave.open(str(reference), "wb") as wav:
                        wav.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
                        wav.writeframes(replay_pcm)
                    LOG.info("Fragmento a devolver listo: %s; %d bytes PCM. El uplink sigue activo",
                             reference, len(replay_pcm))
                    start_background(send_playback(replay_pcm, "Audio de prueba"))
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
        pending = list(background_tasks)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        if replay_capture is not None and not replay_capture.complete:
            LOG.warning("Conexion cerrada antes de completar la ventana de devolucion; no se envio PLAY")
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
    recognizer = FasterWhisperRecognizer(args.stt_model) if args.stt_window_seconds else None
    play_pcm = load_reply_wav(args.play_wav) if args.play_wav else None
    reply_pcm = None
    if args.stt_window_seconds and args.reply_wav.exists():
        reply_pcm = load_reply_wav(args.reply_wav)
        LOG.info("WAV de respuesta listo: %s (%.2f s)", args.reply_wav, len(reply_pcm) / 32000)
    elif args.stt_window_seconds:
        LOG.warning("No existe %s: el servidor transcribira y detectara Órbita, pero no podra responder con audio",
                    args.reply_wav)
    async with serve(lambda ws: handle(ws, args.output, args.segment_seconds,
                                       args.stt_window_seconds, recognizer, reply_pcm,
                                       args.replay_window, play_pcm), args.host, args.port,
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
    demo = parser.add_mutually_exclusive_group()
    demo.add_argument("--play-wav", type=Path,
                      help="envia una vez un WAV conocido PCM mono/16 kHz/16-bit de hasta 15 s, sin STT")
    demo.add_argument("--replay-window", type=float, nargs=2, metavar=("INICIO", "FIN"),
                      help="devuelve una vez este intervalo del PCM recibido usando PLAY; "
                           "ejemplo: --replay-window 2 12. No requiere STT. Duracion: 1..15 s")
    demo.add_argument("--stt-window-seconds", type=float, default=None,
                        help="analiza una sola ventana inicial de audio de esta duracion (1..30 s); "
                             "el flujo WebSocket continua abierto para devolver la respuesta")
    parser.add_argument("--stt-model", default="tiny",
                        help="modelo faster-whisper para la prueba local (por defecto: tiny)")
    parser.add_argument("--reply-wav", type=Path,
                        default=Path(__file__).resolve().parent / "respuesta_orbita.wav",
                        help="respuesta PCM WAV mono/16 kHz/16-bit; archivo opcional por ahora")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.replay_window is not None:
        start, end = args.replay_window
        if not math.isfinite(start) or not math.isfinite(end) or start < 0 or not 1 <= end - start <= 15:
            parser.error("--replay-window requiere 0 <= INICIO < FIN y una duracion de 1 a 15 segundos")
    if args.stt_window_seconds is not None and not 1 <= args.stt_window_seconds <= 30:
        parser.error("--stt-window-seconds debe estar entre 1 y 30")
    if args.stt_window_seconds is not None:
        try:
            import faster_whisper  # noqa: F401
        except ImportError:
            parser.error("falta faster-whisper; instalar con: python -m pip install -r tools/requirements-speech.txt")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        LOG.info("Servidor detenido")
