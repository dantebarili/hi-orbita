"""
Analiza una toma de la prueba de beamforming (WAV de 3 canales: L, R, beamformado).

1. Verifica el firmware: recalcula el hipercardioide desde L y R (misma cadena que
   firmware/components/orbita_beamforming) y lo compara con el canal 3. Si la diferencia
   es de 1-2 LSB, el firmware hace lo que dice el diseno y cualquier problema es acustico.
2. Niveles en la banda de voz (300 Hz - 3 kHz) de L, R y BF, y BF relativo al mic de adelante.
   Para el rechazo: grabar la misma voz de frente y de costado y restar los "BF - adelante".

Uso: python analizar_beamforming.py toma.wav [--beta 0.5] [--fc 150] [--adelante R] [--ganancia-atras 1.0]
     (los parametros tienen que coincidir con los #define de firmware/main/test_bf.c)
"""

import argparse

import numpy as np
import soundfile as sf
from scipy.signal import butter, csd, lfilter, sosfilt, welch

FS = 16000   # se pisa con la del WAV en main()


def hipercardioide(x1, x2, beta, fc):
    """Misma cadena que orbita_bf_procesar (en float64)."""
    x1_ant = np.concatenate(([0.0], x1[:-1]))
    x2_ant = np.concatenate(([0.0], x2[:-1]))
    c_f = x1 - x2_ant
    c_b = x2 - x1_ant
    y_h = c_f - beta * c_b
    if fc > 0:
        a = np.exp(-2 * np.pi * fc / FS)
        y_h = lfilter([1.0], [1.0, -a], y_h)      # v[n] = a*v[n-1] + y_h[n]
    return np.clip(np.rint(0.5 * y_h), -32768, 32767)


def nivel_db(x, sos):
    y = sosfilt(sos, x)
    return 10 * np.log10(np.mean(y ** 2) + 1e-12)


def retardo_por_banda(l, r):
    """Retardo de R respecto de L (en muestras, + = R llega despues). En cada banda ajusta la pendiente
    de la fase del espectro cruzado (fase = -w*retardo), ponderada por coherencia, solo con bins de
    coherencia > 0,5. Con d = 21,4 mm: fuente sobre el eje del lado de L = +1, de costado = 0, del lado
    de R = -1 (mas el corrimiento electrico). Un retardo plano y entero en todas las bandas es digital."""
    f, pxy = csd(l, r, fs=FS, nperseg=2048)        # conj(L)*R
    _, pxx = welch(l, fs=FS, nperseg=2048)
    _, pyy = welch(r, fs=FS, nperseg=2048)
    coh = np.abs(pxy) ** 2 / (pxx * pyy + 1e-30)
    print("Retardo R vs L por banda (pendiente de fase):")
    for lo, hi in ((200, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 7000)):
        m = (f >= lo) & (f < hi) & (coh > 0.5)
        if m.sum() < 4:
            print(f"  {lo:4d}-{hi:4d} Hz: sin datos (coherencia baja)")
            continue
        fase = np.unwrap(np.angle(pxy[m]))
        w, om = coh[m], 2 * np.pi * f[m]
        ret = -np.sum(w * fase * om) / np.sum(w * om ** 2) * FS
        print(f"  {lo:4d}-{hi:4d} Hz: {ret:+.2f} muestras ({m.sum()} bins)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("wav")
    p.add_argument("--beta", type=float, default=0.5)
    p.add_argument("--fc", type=float, default=150.0)
    p.add_argument("--adelante", choices=["L", "R"], default="L")
    p.add_argument("--ganancia-atras", type=float, default=1.0)
    a = p.parse_args()

    global FS
    datos, FS = sf.read(a.wav, dtype="int16")      # FS sale del WAV: 16 kHz normal, 32 kHz en la prueba del corrimiento
    if datos.ndim != 2 or datos.shape[1] != 3:
        raise SystemExit(f"Se esperaba WAV de 3 canales; vino {datos.shape}")
    l, r, bf = (datos[:, k].astype(np.float64) for k in range(3))
    x1, x2 = (l, r) if a.adelante == "L" else (r, l)
    x2 = x2 * a.ganancia_atras

    ref = hipercardioide(x1, x2, a.beta, a.fc)
    dif = np.abs(ref - bf)
    print(f"Muestras: {len(bf)} ({len(bf) / FS:.2f} s)")
    print(f"Firmware vs referencia: dif max = {dif.max():.0f} LSB, muestras con dif > 2 LSB = {(dif > 2).sum()}")
    print("  -> OK, el firmware coincide" if dif.max() <= 2 else "  -> NO coincide: revisar parametros o firmware")

    sos = butter(4, [300, 3000], btype="bandpass", fs=FS, output="sos")
    n_l, n_r, n_bf = nivel_db(l, sos), nivel_db(r, sos), nivel_db(bf, sos)
    n_ad = n_l if a.adelante == "L" else n_r
    print(f"Nivel 300 Hz - 3 kHz (dB rel. 1 LSB^2): L={n_l:.1f}  R={n_r:.1f}  BF={n_bf:.1f}")
    print(f"L - R = {n_l - n_r:+.1f} dB (de costado deberia ser ~0: si no, falta calibrar ganancia)")
    print(f"BF - adelante = {n_bf - n_ad:+.1f} dB (comparar entre tomas de frente y de costado)")
    retardo_por_banda(l, r)


if __name__ == "__main__":
    main()
