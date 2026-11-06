"""
Analiza una toma de la prueba de beamforming (WAV de 3 canales: L, R, beamformado).

1. Verifica el firmware: recalcula el hipercardioide desde L y R (misma cadena que
   firmware/components/orbita_beamforming) y lo compara con el canal 3. Si la diferencia
   es de 1-2 LSB, el firmware hace lo que dice el diseno y cualquier problema es acustico.
2. Niveles en la banda de voz (300 Hz - 3 kHz) de L, R y BF, y BF relativo al mic de adelante.
   Para el rechazo: grabar la misma voz de frente y de costado y restar los "BF - adelante".
3. Retardo de R respecto de L (retardo_por_banda). De aca sale ORBITA_BF_CORRIMIENTO_R.
4. Division en bandas (06/11): "BF - adelante" por banda hasta 7 kHz, con bandas (lo que mando el
   firmware) y sin bandas (recalculado aca desde L y R, misma toma). De frente: con bandas tiene que
   quedar dentro de ~-2 dB en todas; sin bandas cae arriba de 4 kHz.

COMO SE CALCULA EL RETARDO (3.)
   Un retardo de t muestras es una fase que crece en linea recta con la frecuencia: fase(w) = -w*t.
   - Espectro cruzado conj(L)*R (csd, Welch con bloques de 2048): su fase es la fase de R menos la de L.
   - Coherencia (0 a 1) por bin: solo se usan bins con coherencia > 0,5 (donde L y R ven la misma senal).
   - En cada banda (200-500, 500-1k, 1k-2k, 2k-4k, 4k-7k Hz) se ajusta la recta fase = -w*t por minimos
     cuadrados ponderados por la coherencia: t = -sum(coh*fase*w) / sum(coh*w^2), pasado a muestras (* FS).
   - Resultado: + = R llega despues que L. Mezcla dos cosas: lo acustico (posicion de la fuente; con
     d = 21,4 mm va de -1 a +1 muestra) y lo electrico (el bus I2S, constante).
   - Para separarlas: misma posicion, una toma normal (D1) y otra con los pines L/R de los mics
     intercambiados (D2). Electrico = (D1 + D2)/2; acustico = (D1 - D2)/2. Medido 01/11: electrico = -0,86,
     o sea R llega 0,86 muestras antes -> ORBITA_BF_CORRIMIENTO_R = 0,86 (signo cambiado).
   - Retardo plano en todas las bandas = digital/electrico; si cambia con la banda, hay acustica o ruido.
   Recalibrar (y cambiar ORBITA_BF_CORRIMIENTO_R en beamforming.h de main) si cambia la placa, el ruteo
   del bus I2S, el modelo de mic o el driver I2S. Medido en una sola placa: confirmar en 2-3 mas.

Uso: python analizar_beamforming.py toma.wav [--beta 0.5] [--fc 150] [--adelante R] [--ganancia-atras 1.0]
     [--cruce 4000]   (los parametros tienen que coincidir con los #define de firmware/main/test_bf.c)
"""

import argparse

import numpy as np
import soundfile as sf
from scipy.signal import butter, csd, lfilter, sosfilt, welch

FS = 16000   # se pisa con la del WAV en main()


def alinear(l, r, corrimiento_r):
    """Igual que orbita_bf_procesar: R por un FIR de Lagrange (retardo 1 + corrimiento), L 1 muestra pura."""
    D = 1.0 + corrimiento_r
    h = [-(D - 1) * (D - 2) * (D - 3) / 6, D * (D - 2) * (D - 3) / 2, -D * (D - 1) * (D - 3) / 2, D * (D - 1) * (D - 2) / 6]
    return np.concatenate(([0.0], l[:-1])), lfilter(h, [1.0], r)


def biquad_butter(pasa_altos, fc):
    """Igual que biquad_butter de beamforming.c: Butterworth 2.o orden (Q = 0,707), bilineal (RBJ)."""
    w0 = 2 * np.pi * fc / FS
    c, alfa = np.cos(w0), np.sin(w0) / (2 * 0.70710678)
    k = (1 + c) / 2 if pasa_altos else (1 - c) / 2
    b = [k, -2 * k if pasa_altos else 2 * k, k]
    return np.array(b) / (1 + alfa), np.array([1.0, -2 * c / (1 + alfa), (1 - alfa) / (1 + alfa)])


def lr4(x, pasa_altos, fc):
    """Linkwitz-Riley 4.o orden = dos Butterworth de 2.o orden en cascada."""
    b, a = biquad_butter(pasa_altos, fc)
    return lfilter(b, a, lfilter(b, a, x))


def hipercardioide(x1, x2, beta, fc, cruce=0.0):
    """Misma cadena que orbita_bf_procesar (en float64), con x1/x2 ya alineados por alinear().
    cruce > 0: LR4 -> hipercardioide debajo + x1[n-1] (mic de adelante) arriba."""
    x1_ant = np.concatenate(([0.0], x1[:-1]))
    x2_ant = np.concatenate(([0.0], x2[:-1]))
    c_f = x1 - x2_ant
    c_b = x2 - x1_ant
    y_h = c_f - beta * c_b
    if fc > 0:
        a = np.exp(-2 * np.pi * fc / FS)
        y_h = lfilter([1.0], [1.0, -a], y_h)      # v[n] = a*v[n-1] + y_h[n]
    y = 0.5 * y_h
    if cruce > 0:
        y = lr4(y, False, cruce) + lr4(x1_ant, True, cruce)
    return np.clip(np.rint(y), -32768, 32767)


def tabla_bandas(ad, bf, bf_sin):
    """BF - adelante por banda, con y sin division en bandas (0 dB = igual que un mic solo)."""
    print("BF - adelante por banda (0 dB = un mic solo; de frente con bandas: ~0 a -2 dB en todas):")
    print("     banda [Hz]   con bandas   sin bandas")
    for lo, hi in ((300, 1000), (1000, 2000), (2000, 3000), (3000, 4000), (4000, 5000), (5000, 7000), (300, 7000)):
        sos = butter(4, [lo, hi], btype="bandpass", fs=FS, output="sos")
        n_ad = nivel_db(ad, sos)
        print(f"  {lo:5d}-{hi:5d}   {nivel_db(bf, sos) - n_ad:+8.1f} dB  {nivel_db(bf_sin, sos) - n_ad:+8.1f} dB")


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
    p.add_argument("--corrimiento-r", type=float, default=0.86,
                   help="muestras que R llega adelantado (BF_CORRIMIENTO_R del firmware)")
    p.add_argument("--cruce", type=float, default=4000.0,
                   help="cruce del crossover en Hz (BF_CRUCE_HZ del firmware); 0 = sin bandas (tomas anteriores al 06/11)")
    p.add_argument("--firmware-viejo", action="store_true",
                   help="tomas grabadas antes del 01/11 (beamformer sin alinear L/R)")
    a = p.parse_args()

    global FS
    datos, FS = sf.read(a.wav, dtype="int16")      # FS sale del WAV: 16 kHz normal, 32 kHz en la prueba del corrimiento
    if datos.ndim != 2 or datos.shape[1] != 3:
        raise SystemExit(f"Se esperaba WAV de 3 canales; vino {datos.shape}")
    l, r, bf = (datos[:, k].astype(np.float64) for k in range(3))
    l_al, r_al = (l, r) if a.firmware_viejo else alinear(l, r, a.corrimiento_r)
    x1, x2 = (l_al, r_al) if a.adelante == "L" else (r_al, l_al)
    x2 = x2 * a.ganancia_atras

    ref = hipercardioide(x1, x2, a.beta, a.fc, a.cruce)
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
    tabla_bandas(l if a.adelante == "L" else r, bf, hipercardioide(x1, x2, a.beta, a.fc, 0.0))
    retardo_por_banda(l, r)


if __name__ == "__main__":
    main()
