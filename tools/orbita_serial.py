"""
Cliente PC para Proyecto Orbita - captura en streaming.

Usa DOS puertos COM (ver firmware/NOTAS_SERIAL.md):
- Puerto de COMANDOS (USB-Serial-JTAG nativo, "Puerto B"): manda 'g' para
  disparar una grabacion y recibe el marcador WAV_ON_UART0.
- Puerto de DATOS (UART0, el cable del chip puente, "Puerto A"): recibe el
  .wav binario (header + audio) a 921600 baud mientras el firmware graba.
  Cuando termina el stream, el firmware vuelve a 115200 baud y manda una
  linea de log ("Fin: mandados X de Y bytes, overruns=...") que este script
  muestra para saber si la grabacion salio completa.

El audio se escribe a disco a medida que llega (no se junta en memoria), asi
que sirve para grabaciones largas.

IMPORTANTE: no puede haber `idf.py monitor` abierto en el puerto de datos
(Windows no comparte un COM entre dos procesos).

Requisitos: pip install pyserial
Uso: python orbita_serial.py <puerto_comandos> <puerto_datos>
Ej:  python orbita_serial.py COM5 COM3
"""

import argparse
import struct
import time
from datetime import datetime

import serial

# --- Constantes: deben coincidir con firmware/main/main.c ---
WAV_HEADER_SIZE = 44
DATA_UART_BAUD = 921600       # ORBITA_UART_DATA_BAUD
CONSOLE_UART_BAUD = 115200    # ORBITA_UART_CONSOLE_BAUD
WAV_START_MARKER = "WAV_ON_UART0"
READ_CHUNK = 8192
PROGRESS_EVERY_S = 5


def parse_args():
    parser = argparse.ArgumentParser(description="Cliente serial de Proyecto Orbita")
    parser.add_argument("cmd_port", help="Puerto COM del USB-Serial-JTAG nativo (Puerto B)")
    parser.add_argument("data_port", help="Puerto COM del UART0/chip puente (Puerto A), sin monitor abierto")
    parser.add_argument("--cmd-baud", type=int, default=115200)
    return parser.parse_args()


def wav_data_size(header: bytes) -> int:
    # Subchunk2Size (bytes de audio) en el offset 40-43, little-endian.
    return struct.unpack_from("<I", header, 40)[0]


def read_exact(ser: serial.Serial, n: int) -> bytes:
    data = b""
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def find_marker(ser: serial.Serial, marker: bytes, max_search: int = 200_000) -> bool:
    # Busca `marker` byte a byte: antes del header puede haber basura (logs
    # de boot a 115200), asi que no se asume una posicion fija.
    window = bytearray()
    scanned = 0
    while scanned < max_search:
        b = ser.read(1)
        if not b:
            return False
        window += b
        scanned += 1
        if len(window) > len(marker):
            del window[0]
        if bytes(window) == marker:
            return True
    return False


def wait_for_marker(ser_cmd: serial.Serial, timeout_s: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = ser_cmd.readline().decode("ascii", errors="ignore").strip()
        if line == WAV_START_MARKER:
            return True
    return False


def read_firmware_summary(ser_data: serial.Serial) -> None:
    # Tras el stream el firmware baja a 115200 y loguea. Cambiamos el baud
    # de este lado y buscamos la linea "Fin:". Es best-effort: puede llegar
    # algo de basura justo en el cambio de baud.
    ser_data.baudrate = CONSOLE_UART_BAUD
    deadline = time.monotonic() + 3.0
    buf = b""
    while time.monotonic() < deadline:
        buf += ser_data.read(256)
        if b"Fin:" in buf and b"\n" in buf[buf.index(b"Fin:"):]:
            break
    ser_data.baudrate = DATA_UART_BAUD
    text = buf.decode("ascii", errors="ignore")
    for line in text.splitlines():
        if "Fin:" in line:
            print("Firmware:", line[line.index("Fin:"):].strip())
            return
    print("No se vio la linea final del firmware (no implica error; mira el tamano recibido).")


def record(ser_cmd: serial.Serial, ser_data: serial.Serial) -> None:
    ser_cmd.reset_input_buffer()
    ser_cmd.write(b"g")

    if not wait_for_marker(ser_cmd):
        print("El firmware no respondio con WAV_ON_UART0. Se cancela.")
        return

    print("Grabando, buscando el header...")
    if not find_marker(ser_data, b"RIFF"):
        print("No aparecio 'RIFF' en el puerto de datos (timeout). Se descarta.")
        return

    header = b"RIFF" + read_exact(ser_data, WAV_HEADER_SIZE - 4)
    if len(header) < WAV_HEADER_SIZE:
        print(f"Header incompleto: {len(header)}/{WAV_HEADER_SIZE} bytes. Se descarta.")
        return

    total = wav_data_size(header)
    print(f"Header OK, esperando {total} bytes de audio...")

    filename = datetime.now().strftime("orbita_%Y%m%d_%H%M%S.wav")
    received = 0
    t_start = time.monotonic()
    t_progress = t_start

    with open(filename, "wb") as f:
        f.write(header)
        while received < total:
            chunk = ser_data.read(min(READ_CHUNK, total - received))
            if not chunk:
                break  # 1 s sin datos: se corto el stream
            f.write(chunk)
            received += len(chunk)

            now = time.monotonic()
            if now - t_progress >= PROGRESS_EVERY_S:
                t_progress = now
                print(f"  ...{received}/{total} bytes ({100 * received / total:.0f}%)")

    elapsed = time.monotonic() - t_start
    if received < total:
        print(f"INCOMPLETO: {received}/{total} bytes ({total - received} faltan). Guardado igual: {filename}")
    else:
        print(f"Completo: {received} bytes en {elapsed:.1f}s. Guardado: {filename}")

    read_firmware_summary(ser_data)


def main():
    args = parse_args()
    ser_cmd = serial.Serial(args.cmd_port, args.cmd_baud, timeout=1)
    ser_data = serial.Serial(args.data_port, DATA_UART_BAUD, timeout=1)
    print(f"Conectado: comandos en {args.cmd_port}, audio en {args.data_port} ({DATA_UART_BAUD} baud).")

    try:
        while True:
            line = input("Escribi 'g' + Enter para grabar (q para salir): ").strip().lower()
            if line == "g":
                record(ser_cmd, ser_data)
            elif line == "q":
                break
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        ser_cmd.close()
        ser_data.close()


if __name__ == "__main__":
    main()
