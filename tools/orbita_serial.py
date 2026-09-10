"""
Cliente PC para Proyecto Orbita - Etapa 1.

Se conecta al puerto nativo USB-Serial-JTAG del ESP32-S3 (protocolo propio de
comandos/datos - NO es el puerto de logs, ver firmware/NOTAS_SERIAL.md).

Mientras no se dispara una grabacion, grafica en vivo el RMS de ambos
canales (una linea de texto "rms_l,rms_r" por bloque, ~10 veces por segundo).
Al escribir 'g' + Enter en la consola, dispara una captura de 10s en el
firmware y guarda el resultado como .wav en el directorio actual.

Requisitos: pip install pyserial matplotlib
Uso: python orbita_serial.py COM5
"""

import argparse
import struct
import threading
from collections import deque
from datetime import datetime

import serial
import matplotlib.pyplot as plt

# --- Constantes: deben coincidir con firmware/main/main.c ---
CAPTURE_SECONDS = 3           # TEST_DURATION_SEC en el firmware
WAV_HEADER_SIZE = 44
PLOT_WINDOW_BLOCKS = 100      # puntos visibles en el grafico (~10s de historia)


def parse_args():
    parser = argparse.ArgumentParser(description="Cliente serial de Proyecto Orbita")
    parser.add_argument("port", help="Puerto COM del USB-Serial-JTAG nativo (ej. COM5)")
    parser.add_argument("--baud", type=int, default=115200)
    return parser.parse_args()


def read_exact(ser: serial.Serial, n: int, on_progress=None) -> bytes:
    # ser.read(n) puede devolver menos de n bytes si se corta el timeout;
    # hay que insistir hasta juntar exactamente los n bytes esperados.
    # on_progress(bytes_hasta_ahora) es opcional -- lo usamos para bombear
    # los eventos de matplotlib durante lecturas largas (el header + los
    # ~960KB de audio), si no la ventana queda "No responde" en Windows, y
    # tambien para mostrar progreso en consola.
    data = b""
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            break
        data += chunk
        if on_progress is not None:
            on_progress(len(data))
    return data


def wav_data_size(header: bytes) -> int:
    # Subchunk2Size (cantidad de bytes de audio) vive en el offset 40-43 del
    # header, little-endian (ver wav_writer.c: wav_build_header).
    return struct.unpack_from("<I", header, 40)[0]


def input_listener(trigger_event: threading.Event, stop_event: threading.Event):
    while not stop_event.is_set():
        try:
            line = input()
        except EOFError:
            break
        if line.strip().lower() == "g":
            trigger_event.set()


def save_wav(ser: serial.Serial, fig) -> None:
    print("Descargando WAV de la placa (header)...")
    header = read_exact(ser, WAV_HEADER_SIZE, on_progress=lambda _n: fig.canvas.flush_events())
    if len(header) < WAV_HEADER_SIZE:
        print(f"Header incompleto: {len(header)}/{WAV_HEADER_SIZE} bytes. Se descarta esta captura.")
        return

    data_size = wav_data_size(header)
    # data_size esperado ~= 10s * 16000Hz * 2 canales * 3 bytes = 960000.
    # Si este numero sale disparatado (gigante o irrisorio), el header esta
    # corrido (bytes de mas/de menos antes de "RIFF") y por eso la descarga
    # se queda esperando bytes que nunca van a llegar del todo.
    print(f"Header OK, esperando {data_size} bytes de audio...")

    last_print = [0]
    def report_progress(bytes_so_far):
        fig.canvas.flush_events()
        # Imprime cada ~5KB para poder ver si avanza (aunque sea lento) o
        # esta trabado en 0.
        if bytes_so_far - last_print[0] >= 5_000:
            last_print[0] = bytes_so_far
            print(f"  ...{bytes_so_far}/{data_size} bytes")

    data = read_exact(ser, data_size, on_progress=report_progress)
    if len(data) < data_size:
        print(f"Audio incompleto: {len(data)}/{data_size} bytes. Se guarda igual.")

    filename = datetime.now().strftime("orbita_%Y%m%d_%H%M%S.wav")
    with open(filename, "wb") as f:
        f.write(header)
        f.write(data)
    print(f"Guardado: {filename} ({len(data)} bytes de audio)")


def main():
    args = parse_args()
    ser = serial.Serial(args.port, args.baud, timeout=1)

    trigger_event = threading.Event()
    stop_event = threading.Event()
    listener = threading.Thread(target=input_listener, args=(trigger_event, stop_event), daemon=True)
    listener.start()
    print(f"Conectado a {args.port}. Escribi 'g' + Enter para grabar {CAPTURE_SECONDS}s.")

    rms_l_hist = deque(maxlen=PLOT_WINDOW_BLOCKS)
    rms_r_hist = deque(maxlen=PLOT_WINDOW_BLOCKS)

    plt.ion()
    fig, ax = plt.subplots()
    line_l, = ax.plot([], [], label="L")
    line_r, = ax.plot([], [], label="R")
    ax.set_ylim(0, 2 ** 23)  # rango maximo de una muestra de 24 bits
    ax.set_xlim(0, PLOT_WINDOW_BLOCKS)
    ax.set_xlabel("bloques (~0.1s c/u)")
    ax.set_ylabel("RMS")
    ax.set_title("Monitoreo en vivo")
    ax.legend()
    plt.show(block=False)

    recording = False

    try:
        while True:
            if not recording and trigger_event.is_set():
                trigger_event.clear()
                ser.write(b"g")
                recording = True
                ax.set_title("Grabando...")
                print(f"Grabando {CAPTURE_SECONDS}s...")

            raw_line = ser.readline()
            if not raw_line:
                fig.canvas.flush_events()
                continue

            text = raw_line.decode("ascii", errors="ignore").strip()

            if recording and text == "WAV_START":
                # Marcador explicito: el firmware termino de mandar lineas
                # RMS y lo que sigue en el stream es el header WAV binario.
                # No dependemos de contar N lineas de antemano.
                save_wav(ser, fig)
                recording = False
                ax.set_title("Monitoreo en vivo")
                print("Listo, volviendo a monitoreo en vivo.")
                continue

            if "," not in text:
                continue
            try:
                rms_l_str, rms_r_str = text.split(",")
                rms_l, rms_r = float(rms_l_str), float(rms_r_str)
            except ValueError:
                continue

            rms_l_hist.append(rms_l)
            rms_r_hist.append(rms_r)
            line_l.set_data(range(len(rms_l_hist)), rms_l_hist)
            line_r.set_data(range(len(rms_r_hist)), rms_r_hist)
            fig.canvas.draw_idle()
            fig.canvas.flush_events()

    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        ser.close()


if __name__ == "__main__":
    main()
