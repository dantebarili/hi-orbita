"""
Cliente PC para Proyecto Orbita - Etapa 1.

Usa DOS puertos COM (ver firmware/NOTAS_SERIAL.md):
- Puerto de COMANDOS (USB-Serial-JTAG nativo, "Puerto B"): manda 'g' para
  disparar una grabacion y recibe el RMS en vivo (texto "rms_l,rms_r" por
  linea).
- Puerto de DATOS (UART0, el mismo cable del chip puente que se usa para
  flashear/logs, "Puerto A"): recibe el .wav binario (header + audio) a
  alto baudrate una vez que el firmware avisa "WAV_ON_UART0" por el otro
  puerto. El USB-Serial-JTAG resulto tener throughput real de ~170 B/s
  para transferencias grandes -- inviable para un .wav de cientos de KB --
  por eso el audio se movio a un UART real.

IMPORTANTE: no podes tener `idf.py monitor` abierto en el puerto de datos
al mismo tiempo que este script (Windows no comparte un COM entre dos
procesos). Cerra el monitor antes de correr esto.

Requisitos: pip install pyserial matplotlib
Uso: python orbita_serial.py <puerto_comandos> <puerto_datos>
Ej:  python orbita_serial.py COM5 COM3
"""

import argparse
import struct
import threading
import time
from collections import deque
from datetime import datetime

import serial
import matplotlib.pyplot as plt

# --- Constantes: deben coincidir con firmware/main/main.c ---
CAPTURE_SECONDS = 3           # TEST_DURATION_SEC en el firmware
WAV_HEADER_SIZE = 44
PLOT_WINDOW_BLOCKS = 100      # puntos visibles en el grafico (~10s de historia)
DATA_UART_BAUD = 921600       # ORBITA_UART_DATA_BAUD en el firmware
WAV_START_MARKER = "WAV_ON_UART0"


def parse_args():
    parser = argparse.ArgumentParser(description="Cliente serial de Proyecto Orbita")
    parser.add_argument("cmd_port", help="Puerto COM del USB-Serial-JTAG nativo, Puerto B (ej. COM5) -- comandos y RMS en vivo")
    parser.add_argument("data_port", help="Puerto COM del UART0/chip puente, Puerto A (ej. COM3) -- el mismo que usas para idf.py monitor, tiene que estar cerrado")
    parser.add_argument("--cmd-baud", type=int, default=115200)
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


def find_marker(ser: serial.Serial, marker: bytes, max_search: int = 200_000) -> bool:
    # Busca `marker` (ej. b"RIFF") byte a byte en el stream entrante, sin
    # asumir que el buffer esta "limpio" en ningun momento particular.
    # Por que no alcanza con vaciar el buffer antes de leer: el puerto de
    # datos esta abierto desde el arranque del script a 921600 baud, pero
    # la placa manda sus logs de boot por el mismo cable a 115200 baud
    # ANTES de subir el baudrate -- eso queda como basura en el buffer. Y
    # vaciar el buffer justo antes de leer tampoco sirve: para ese momento
    # el header real (que viaja rapidisimo a 921600) puede haber llegado
    # YA y estar mezclado con esa basura -- vaciar lo tira tambien (visto
    # en pruebas reales, empeoro el problema). Buscar el marcador conocido
    # en vez de asumir una posicion fija es inmune a este problema de
    # timing por completo.
    window = bytearray()
    scanned = 0
    while scanned < max_search:
        b = ser.read(1)
        if not b:
            return False  # timeout sin encontrar el marcador
        window += b
        scanned += 1
        if len(window) > len(marker):
            del window[0]
        if bytes(window) == marker:
            return True
    return False


def input_listener(trigger_event: threading.Event, stop_event: threading.Event):
    while not stop_event.is_set():
        try:
            line = input()
        except EOFError:
            break
        if line.strip().lower() == "g":
            trigger_event.set()


def save_wav(ser: serial.Serial, fig) -> None:
    # flush_events() pumping the GUI event loop tiene su propio costo fijo
    # por llamada. Si el USB-Serial-JTAG entrega los datos en microlotes muy
    # seguidos, llamarlo en CADA lectura (como haciamos antes) puede sumar
    # miles de llamadas y dominar el tiempo total, mas que el USB en si.
    # Lo throttleamos por tiempo real (una vez cada ~100ms), no por lectura.
    FLUSH_INTERVAL_S = 0.1
    last_flush = [0.0]
    def throttled_flush():
        now = time.monotonic()
        if now - last_flush[0] >= FLUSH_INTERVAL_S:
            last_flush[0] = now
            fig.canvas.flush_events()

    t_start = time.monotonic()
    print("Descargando WAV de la placa (buscando el header)...")
    if not find_marker(ser, b"RIFF"):
        print("No aparecio 'RIFF' en el stream (timeout). Se descarta esta captura.")
        return

    # Ya consumimos "RIFF" (los primeros 4 bytes del header) buscandolo;
    # leemos el resto y lo reconstruimos completo.
    resto_header = read_exact(ser, WAV_HEADER_SIZE - 4, on_progress=lambda _n: throttled_flush())
    header = b"RIFF" + resto_header
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
    t_data_start = time.monotonic()
    def report_progress(bytes_so_far):
        throttled_flush()
        # Imprime cada ~5KB para poder ver si avanza (aunque sea lento) o
        # esta trabado en 0.
        if bytes_so_far - last_print[0] >= 5_000:
            last_print[0] = bytes_so_far
            elapsed = time.monotonic() - t_data_start
            rate = bytes_so_far / elapsed if elapsed > 0 else 0
            print(f"  ...{bytes_so_far}/{data_size} bytes ({rate:.0f} B/s)")

    data = read_exact(ser, data_size, on_progress=report_progress)
    if len(data) < data_size:
        print(f"Audio incompleto: {len(data)}/{data_size} bytes. Se guarda igual.")

    t_total = time.monotonic() - t_start
    avg_rate = len(data) / t_total if t_total > 0 else 0
    print(f"Descarga completa en {t_total:.1f}s ({avg_rate:.0f} B/s promedio).")

    filename = datetime.now().strftime("orbita_%Y%m%d_%H%M%S.wav")
    with open(filename, "wb") as f:
        f.write(header)
        f.write(data)
    print(f"Guardado: {filename} ({len(data)} bytes de audio)")


def main():
    args = parse_args()
    ser_cmd = serial.Serial(args.cmd_port, args.cmd_baud, timeout=1)
    # timeout mas largo aca: a diferencia de las lineas de RMS (que llegan
    # cada ~100ms), el .wav puede tardar un rato en arrancar a llegar del
    # todo si el firmware esta ocupado grabando/empaquetando -- no
    # queremos que read_exact() aborte por un timeout corto de casualidad.
    ser_data = serial.Serial(args.data_port, DATA_UART_BAUD, timeout=1)

    trigger_event = threading.Event()
    stop_event = threading.Event()
    listener = threading.Thread(target=input_listener, args=(trigger_event, stop_event), daemon=True)
    listener.start()
    print(f"Conectado: comandos/RMS en {args.cmd_port}, audio en {args.data_port} ({DATA_UART_BAUD} baud).")
    print(f"Escribi 'g' + Enter para grabar {CAPTURE_SECONDS}s.")

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
                ser_cmd.write(b"g")
                recording = True
                ax.set_title("Grabando...")
                print(f"Grabando {CAPTURE_SECONDS}s...")

            raw_line = ser_cmd.readline()
            if not raw_line:
                fig.canvas.flush_events()
                continue

            text = raw_line.decode("ascii", errors="ignore").strip()

            if recording and text == WAV_START_MARKER:
                # Marcador explicito: el firmware termino de mandar lineas
                # RMS por este puerto y el audio (header + datos) viene
                # ahora por el otro puerto (UART0), a alto baudrate.
                save_wav(ser_data, fig)
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
        ser_cmd.close()
        ser_data.close()


if __name__ == "__main__":
    main()
