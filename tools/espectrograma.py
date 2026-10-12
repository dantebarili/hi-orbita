"""
Espectrogramas comparativos de una toma de test de Orbita.

Lee el WAV de 2 canales que arma el firmware (por defecto canal 0 = mic L crudo y
canal 1 = mic R crudo; con --canales afe, canal 1 = salida del AFE) y, opcionalmente, una grabacion hecha con el celu, y dibuja un
espectrograma por fuente con la MISMA escala de color, uno debajo del otro:

    1. Canal 0: mic L crudo
    2. Canal 1: mic R crudo (o salida del AFE con --canales afe)
    3. Toma celu            (solo si se pasa --celu)

Opcionalmente mide la relacion senal/ruido (SNR) de cada fuente en dos ventanas
de tiempo que elegis mirando el espectrograma: una de solo ruido y una de voz.

No alinea las tomas en el tiempo: el AFE llega con unas decenas de ms de retraso
y el celu empezo a grabar en otro momento. Para que las voces coincidan en el
grafico usa --offset-celu SEGUNDOS (cuanto se corre el celu hacia la derecha) o
--offset-celu auto, que sincroniza por el aplauso del principio de las dos grabaciones.

Requisitos: pip install numpy scipy matplotlib soundfile
            (soundfile lee .ogg/.flac/.wav; ffmpeg en el PATH solo hace falta para .m4a/.mp3)

Uso:
    python espectrograma.py ruido_labo/ruido_labo5.wav
    python espectrograma.py ruido_labo/ruido_labo5.wav --celu celu.m4a --offset-celu 1.3
    python espectrograma.py toma.wav --celu celu.wav --ruido 1 3 --voz 5 9
"""

import argparse
import shutil
import subprocess
import tempfile
from math import gcd
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from scipy import signal
from scipy.io import wavfile

try:
    import soundfile as sf
except ImportError:                       # opcional: solo para .ogg/.opus/.flac
    sf = None

FS = 16000          # frecuencia de muestreo del firmware (todo el sistema va a 16 kHz)
NPERSEG = 1024      # ventana de 64 ms: resolucion de 15,6 Hz
NOVERLAP = 768      # 75 % de solapamiento


def parse_args():
    p = argparse.ArgumentParser(description="Espectrogramas de una toma de test de Orbita")
    p.add_argument("wav", type=Path, help="WAV de 2 canales del firmware (ch0 crudo, ch1 AFE)")
    p.add_argument("--celu", type=Path, help="grabacion hecha con el celu (wav, o m4a/mp3 si hay ffmpeg)")
    p.add_argument("--canales", choices=("crudo", "afe"), default="crudo",
                   help="que trae el canal 1 del WAV: mic R crudo (default) o salida del AFE")
    p.add_argument("--offset-celu", default="0",
                   help="segundos que se corre el celu hacia la derecha, o 'auto' para sincronizar por el aplauso (default 0)")
    p.add_argument("--alinear", action="store_true",
                   help="mide el retardo del AFE respecto del crudo (correlacion) y lo corrige en el grafico")
    p.add_argument("--recorte", type=float, nargs=2, metavar=("T0", "T1"),
                   help="mostrar solo este tramo del eje de tiempo, en segundos")
    p.add_argument("--fmax", type=float, default=8000, help="frecuencia maxima del grafico en Hz (default 8000)")
    p.add_argument("--rango", type=float, default=80, help="rango dinamico del color en dB (default 80)")
    p.add_argument("--igualar-nivel", action="store_true",
                   help="lleva las tres senales al mismo RMS antes de graficar (compara forma, no volumen)")
    p.add_argument("--ruido", type=float, nargs=2, metavar=("T0", "T1"),
                   help="ventana de solo ruido, en segundos del grafico, para medir SNR")
    p.add_argument("--voz", type=float, nargs=2, metavar=("T0", "T1"),
                   help="ventana de voz, en segundos del grafico, para medir SNR")
    p.add_argument("--banda", type=float, nargs=2, default=(200, 4000), metavar=("F1", "F2"),
                   help="banda en Hz donde se mide el SNR (default 200 4000: la voz, sin el retumbo grave)")
    p.add_argument("--salida", type=Path, help="PNG de salida (default: <wav>_espectrograma.png)")
    p.add_argument("--no-mostrar", action="store_true", help="solo guardar el PNG, sin abrir la ventana")
    return p.parse_args()


def a_float(x):
    # Normaliza cualquier formato de WAV a float en [-1, 1].
    if x.dtype == np.int16:
        return x / 32768.0
    if x.dtype == np.int32:
        return x / 2147483648.0
    if x.dtype == np.uint8:
        return (x.astype(np.float64) - 128.0) / 128.0
    return x.astype(np.float64)


def leer_wav(path):
    fs, x = wavfile.read(path)
    return fs, a_float(x)


def leer_celu(path):
    # El celu graba casi siempre en m4a/aac: scipy solo lee wav, asi que se
    # convierte con ffmpeg a un wav mono de 16 kHz en una carpeta temporal.
    if path.suffix.lower() == ".wav":
        fs, x = leer_wav(path)
    elif sf is not None and path.suffix.lower() in (".ogg", ".opus", ".flac"):
        x, fs = sf.read(path)             # soundfile decodifica ogg/opus/flac sin ffmpeg
    else:
        if shutil.which("ffmpeg") is None:
            raise SystemExit(f"{path.name} no es .wav y no hay ffmpeg en el PATH. "
                             "Convertilo a .wav (Audacity: Archivo > Exportar) o instala ffmpeg.")
        with tempfile.TemporaryDirectory() as tmp:
            destino = Path(tmp) / "celu.wav"
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(path),
                            "-ac", "1", "-ar", str(FS), str(destino)], check=True)
            fs, x = leer_wav(destino)

    if x.ndim == 2:                       # estereo -> mono
        x = x.mean(axis=1)
    if fs != FS:                          # al mismo muestreo que el firmware
        g = gcd(FS, fs)
        x = signal.resample_poly(x, FS // g, fs // g)
    return x


def estimar_retardo(crudo, afe, max_ms=300):
    # Cuantas muestras llega el AFE despues del crudo: el desplazamiento que maximiza
    # la correlacion cruzada, buscando solo entre 0 y max_ms.
    n = int(max_ms / 1000 * FS)
    # Se correlaciona solo en la banda de la voz: el retumbo grave domina la correlacion
    # y da un retardo falso cuando el AFE ya proceso la senal.
    sos = signal.butter(4, [200, 4000], btype="band", fs=FS, output="sos")
    crudo, afe = signal.sosfiltfilt(sos, crudo), signal.sosfiltfilt(sos, afe)
    corr = signal.correlate(afe, crudo, mode="full")
    lags = signal.correlation_lags(len(afe), len(crudo))
    zona = (lags >= 0) & (lags <= n)
    return int(lags[zona][np.argmax(np.abs(corr[zona]))])


def detectar_aplauso(x, max_s=20.0):
    # Primer golpe fuerte de banda ancha: se mira solo arriba de 1,5 kHz (la voz casi no llega)
    # en tramos de 5 ms, y se toma el primero que supera 30 veces el nivel tipico y 30 % del maximo.
    sos = signal.butter(4, 1500, btype="high", fs=FS, output="sos")
    y = signal.sosfiltfilt(sos, x[: int(max_s * FS)])
    n = int(0.005 * FS)
    e = np.sqrt(np.mean(y[: len(y) // n * n].reshape(-1, n) ** 2, axis=1))
    umbral = max(30 * np.median(e), 0.3 * e.max())
    idx = np.where(e > umbral)[0]
    if len(idx) == 0:
        raise SystemExit("No se encontro un aplauso en los primeros %.0f s." % max_s)
    return idx[0] * n / FS


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


def snr_db(x, t0_senal, ruido, voz, banda):
    # Se filtra a la banda de la voz: el retumbo grave (golpes, soplido) no es ruido de habla
    # y domina la potencia total, lo que arruina la cuenta.
    sos = signal.butter(4, banda, btype="band", fs=FS, output="sos")
    x = signal.sosfiltfilt(sos, x)

    # Potencia media en cada ventana (tiempos del grafico; t0_senal = donde empieza la senal).
    def potencia(ventana):
        i0 = max(int((ventana[0] - t0_senal) * FS), 0)
        i1 = min(int((ventana[1] - t0_senal) * FS), len(x))
        if i1 - i0 < NPERSEG:
            return None
        return np.mean(x[i0:i1] ** 2)

    p_ruido, p_voz = potencia(ruido), potencia(voz)
    if p_ruido is None or p_voz is None:
        return None
    # La ventana de voz incluye ruido: se le resta su potencia para quedarnos con la voz sola.
    return 10 * np.log10(max(p_voz - p_ruido, 1e-18) / max(p_ruido, 1e-18))


def main():
    args = parse_args()

    fs, datos = leer_wav(args.wav)
    if fs != FS or datos.ndim != 2 or datos.shape[1] != 2:
        raise SystemExit("Se esperaba el WAV de test: 2 canales (crudo + AFE) a 16 kHz.")

    datos = datos - datos.mean(axis=0)     # saca el offset de continua del mic (~-37 dBFS)

    if args.alinear and args.canales != "afe":
        print("--alinear solo tiene sentido con --canales afe: se ignora.")
    elif args.alinear:
        retardo = estimar_retardo(datos[:, 0], datos[:, 1])
        print(f"Retardo del AFE medido: {retardo} muestras = {retardo / FS * 1000:.1f} ms (se corrige en el grafico)")
        if retardo > 0:   # se recortan ambos al largo comun (rellenar con ceros dibujaria una barra falsa)
            datos = np.stack([datos[:len(datos) - retardo, 0], datos[retardo:, 1]], axis=1)

    # (titulo, senal, segundo del grafico donde empieza)
    nombre1 = "Canal 1: mic R (crudo)" if args.canales == "crudo" else "Canal 1: salida del AFE"
    fuentes = [
        ("Canal 0: mic L (crudo)", datos[:, 0], 0.0),
        (nombre1, datos[:, 1], 0.0),
    ]
    if args.celu:
        celu = leer_celu(args.celu)
        celu = celu - celu.mean()
        if str(args.offset_celu).lower() == "auto":
            t_esp, t_celu = detectar_aplauso(datos[:, 0]), detectar_aplauso(celu)
            offset = t_esp - t_celu
            print(f"Aplauso: ESP en {t_esp:.3f} s, celu en {t_celu:.3f} s -> offset del celu {offset:+.3f} s")
        else:
            offset = float(args.offset_celu)
        fuentes.append(("Toma celu", celu, offset))

    if args.igualar_nivel:
        objetivo = rms_db(fuentes[0][1])
        fuentes = [(n, x * 10 ** ((objetivo - rms_db(x)) / 20), t0) for n, x, t0 in fuentes]

    # --- Espectrogramas (magnitud en dB respecto de fondo de escala) ---
    espectros = []
    for nombre, x, t0 in fuentes:
        f, t, s = signal.spectrogram(x, FS, window="hann", nperseg=NPERSEG, noverlap=NOVERLAP,
                                     scaling="spectrum", mode="magnitude")
        espectros.append((nombre, f, t + t0, 20 * np.log10(s + 1e-10)))

    # Misma escala de color para todos: el maximo lo fija el que mas se acerca al tope.
    vmax = max(np.percentile(db, 99.5) for _, _, _, db in espectros)
    vmin = vmax - args.rango

    fig, ejes = plt.subplots(len(espectros), 1, figsize=(12, 3.3 * len(espectros)),
                             sharex=True, sharey=True, constrained_layout=True)
    ejes = np.atleast_1d(ejes)
    for ax, (nombre, f, t, db) in zip(ejes, espectros):
        imagen = ax.pcolormesh(t, f, db, shading="auto", vmin=vmin, vmax=vmax, cmap="magma")
        ax.set_title(nombre, loc="left")
        ax.set_ylabel("Frecuencia (Hz)")
        ax.set_ylim(0, args.fmax)
        for ventana, color in ((args.ruido, "cyan"), (args.voz, "lime")):
            if ventana:
                ax.axvspan(*ventana, facecolor="none", edgecolor=color, linewidth=1.5)
    ejes[-1].set_xlabel("Tiempo (s)")
    if args.recorte:
        ejes[-1].set_xlim(*args.recorte)
    fig.colorbar(imagen, ax=ejes, label="dB (0 = fondo de escala)")

    salida = args.salida or args.wav.with_name(args.wav.stem + "_espectrograma.png")
    fig.savefig(salida, dpi=130)
    print(f"Guardado: {salida}")

    # --- Resumen en texto ---
    print(f"\n{'Fuente':<26}{'Duracion':>10}{'RMS (dBFS)':>13}{'SNR (dB)':>11}")
    if args.ruido and args.voz:
        print(f"(SNR en {args.banda[0]:.0f}-{args.banda[1]:.0f} Hz; ruido {args.ruido[0]}-{args.ruido[1]} s, voz {args.voz[0]}-{args.voz[1]} s)")
    for nombre, x, t0 in fuentes:
        snr = snr_db(x, t0, args.ruido, args.voz, args.banda) if (args.ruido and args.voz) else None
        print(f"{nombre:<26}{len(x) / FS:>9.1f}s{rms_db(x):>13.1f}{'-' if snr is None else f'{snr:.1f}':>11}")
    if not (args.ruido and args.voz):
        print("\nPara el SNR pasa --ruido T0 T1 y --voz T0 T1 (segundos del grafico).")

    if not args.no_mostrar:
        plt.show()


if __name__ == "__main__":
    main()
