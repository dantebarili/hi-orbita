"""
Compara los dos canales de una toma de Orbita: ch0 (mic L crudo) vs ch1 (salida del AFE).
Responde: ¿el AFE cambia la senal o es el mic 0 sin procesar?

Busca el retardo del AFE (hasta +-200 ms) por correlacion y, con ese retardo, mide:
  - % de muestras identicas      (100 % = el AFE devuelve el mic 0 sin tocar)
  - correlacion normalizada      (1.0 = misma forma de onda)
  - relacion de RMS ch1 / ch0    (1.0 = mismo nivel)

Uso: python comparar_canales.py toma.wav
Requisitos: pip install numpy scipy
"""

import sys

import numpy as np
from scipy import signal
from scipy.io import wavfile

MAX_RETARDO_S = 0.2


def main(ruta):
    fs, x = wavfile.read(ruta)
    if x.ndim != 2 or x.shape[1] != 2:
        sys.exit("El WAV tiene que tener 2 canales (ch0 crudo, ch1 AFE).")
    ch0 = x[:, 0].astype(np.float64)
    ch1 = x[:, 1].astype(np.float64)

    # Retardo: cuanto se atrasa ch1 respecto de ch0 (AFE llega despues).
    n = min(len(ch0), 10 * fs)                       # alcanza con los primeros 10 s
    corr = signal.correlate(ch1[:n], ch0[:n], mode="full")
    lags = signal.correlation_lags(n, n, mode="full")
    sel = np.abs(lags) <= int(MAX_RETARDO_S * fs)
    retardo = int(lags[sel][np.argmax(corr[sel])])

    if retardo >= 0:
        a, b = ch0[:len(ch0) - retardo], ch1[retardo:]
    else:
        a, b = ch0[-retardo:], ch1[:len(ch1) + retardo]
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]

    identicas = 100.0 * np.mean(a == b)
    rho = np.corrcoef(a, b)[0, 1]
    rms = np.sqrt(np.mean(b ** 2)) / max(np.sqrt(np.mean(a ** 2)), 1e-9)

    print(f"fs = {fs} Hz, {len(ch0) / fs:.1f} s")
    print(f"retardo ch1 vs ch0: {retardo} muestras ({1000 * retardo / fs:.1f} ms)")
    print(f"muestras identicas: {identicas:.2f} %")
    print(f"correlacion:        {rho:.4f}")
    print(f"RMS ch1 / ch0:      {rms:.3f}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
