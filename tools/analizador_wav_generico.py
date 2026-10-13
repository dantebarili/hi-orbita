"""
Analizador generico de WAV: interpreta la grabacion y dibuja un informe en tema oscuro.

Por cada WAV (uno, varios o una carpeta) calcula nivel, continua, SNR estimado, ruido grave y defectos,
y los traduce a un veredicto en lenguaje llano ("Nivel adecuado", "Ruido grave alto"...). Si hay un .json
al lado (el que escribe ws_audio_server.py) suma lo que dice del enlace: huecos, reconexiones, memoria.
Con muchos archivos solo imprime una tabla comparativa y un grafico de evolucion.

SNR: no hay referencia "limpia", asi que se ESTIMA con cuadros de 32 ms (ruido = percentil 10 del nivel,
senal = percentil 95). Sin voz en el archivo no significa nada: para una medida explicita usar
--ruido T0 T1 --voz T0 T1 (segundos) sobre tramos elegidos mirando el espectrograma.

Estructura pensada para una GUI: analizar_archivo() devuelve objetos Resultado (metricas + hallazgos +
datos), dibujar() los pinta; el CLI de abajo solo los usa.

Requisitos: pip install numpy scipy matplotlib

Uso:
    python analizador_wav_generico.py toma.wav
    python analizador_wav_generico.py prueba_websocket/            (todos los .wav de la carpeta)
    python analizador_wav_generico.py a.wav b.wav --unir           (los pega en orden y analiza como uno)
    python analizador_wav_generico.py toma.wav --ruido 0.5 1.5 --voz 3 6
    python analizador_wav_generico.py carpeta/ --sin-png --csv resumen.csv
"""
import argparse
import csv
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys
import unicodedata

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import signal
from scipy.io import wavfile

FRAME_S = 0.032                                   # cuadro de nivel: 32 ms (= un slot del firmware)
BANDAS = [(0, 20), (20, 60), (60, 120), (120, 250), (250, 1000), (1000, 4000), (4000, 8000)]
RING_S = 32.8                                     # colchon del ring out del firmware (1024 slots de 32 ms)
DPI = 160

# -- tema oscuro ------------------------------------------------------------------------------------------
BG, PANEL, GRID = "#0d1117", "#161b22", "#2a3140"
TXT, MUTED = "#e6edf3", "#8b949e"
CIAN, NARANJA, VERDE, AMBAR, ROJO, VIOLETA = "#4cc9f0", "#ff9f43", "#3ecf8e", "#f5a623", "#ff5d5d", "#b388ff"
COLOR_NIVEL = {"ok": VERDE, "aviso": AMBAR, "mal": ROJO, "info": CIAN}
ETIQUETA = {"ok": "OK", "aviso": "ATENCION", "mal": "PROBLEMA", "info": "INFO"}
TEMA = {
    "figure.facecolor": BG, "savefig.facecolor": BG, "axes.facecolor": PANEL, "axes.edgecolor": GRID,
    "axes.labelcolor": MUTED, "axes.titlecolor": TXT, "axes.titlesize": 11, "axes.titleweight": "bold",
    "axes.titlelocation": "left", "axes.grid": True, "grid.color": GRID, "grid.alpha": 0.6, "grid.linewidth": 0.6,
    "xtick.color": MUTED, "ytick.color": MUTED, "text.color": TXT, "font.size": 9,
    "legend.facecolor": PANEL, "legend.edgecolor": GRID, "legend.labelcolor": TXT,
}


@dataclass
class Hallazgo:
    grupo: str        # "audio" o "enlace"
    area: str         # "Nivel", "SNR", "Graves"...
    nivel: str        # ok | aviso | mal | info
    texto: str


@dataclass
class Resultado:
    ruta: Path
    nombre: str
    fs: int
    bits: int
    canal: int
    n_canales: int
    x: np.ndarray
    m: dict
    datos: dict
    hallazgos: list = field(default_factory=list)


def db(v):
    return 20 * np.log10(max(float(v), 1e-12))


def num(v, d=1):
    """Numero con coma decimal (informes en castellano)."""
    return f"{v:.{d}f}".replace(".", ",")


def ascii_(txt):
    """Para la consola de Windows (sin acentos)."""
    return unicodedata.normalize("NFKD", txt).encode("ascii", "ignore").decode()


# -- carga y metricas -----------------------------------------------------------------------------------------
def cargar(ruta):
    """Devuelve (fs, datos float en [-1, 1] con forma (n, canales), bits)."""
    fs, x = wavfile.read(str(ruta))
    if x.dtype == np.uint8:
        y, bits = (x.astype(np.float64) - 128) / 128, 8
    elif x.dtype == np.int16:
        y, bits = x / 32768.0, 16
    elif x.dtype == np.int32:                     # 24 bits se leen alineados a la izquierda
        y, bits = x / 2147483648.0, 32
    else:
        y, bits = x.astype(np.float64), 32
    return fs, (y[:, None] if y.ndim == 1 else y), bits


def tramo(x, fs, ventana):
    a, b = int(ventana[0] * fs), int(ventana[1] * fs)
    return x[a:b] if b > a else x[:0]


def metricas(x, fs, ruido=None, voz=None):
    """Mide un canal. Devuelve (metricas, datos para graficar)."""
    m = {"dur_s": len(x) / fs, "pico_dbfs": db(np.abs(x).max()), "rms_dbfs": db(x.std())}
    m["saturadas"] = int((np.abs(x) >= 0.999).sum())
    m["dc_pct"] = 100 * x.mean()
    n1 = min(len(x) // 4, fs)                     # continua del primer segundo vs el resto
    m["dc_inicio_pct"] = 100 * x[:n1].mean() if n1 else 0.0
    m["dc_resto_pct"] = 100 * x[n1:].mean() if len(x) > n1 else 0.0
    # SNR estimado por cuadros de 32 ms
    n = int(FRAME_S * fs)
    k = max(len(x) // n, 1)
    rms = x[:k * n].reshape(k, -1).std(axis=1) if len(x) >= n else np.array([x.std()])
    rms_db = 20 * np.log10(np.maximum(rms, 1e-12))
    m["ruido_dbfs"], m["senal_dbfs"] = np.percentile(rms_db, 10), np.percentile(rms_db, 95)
    m["snr_est_db"] = m["senal_dbfs"] - m["ruido_dbfs"]
    m["pct_activo"] = 100 * float((rms_db > m["ruido_dbfs"] + 10).mean())
    m["snr_ventanas_db"] = None
    if ruido and voz:
        xr, xv = tramo(x, fs, ruido), tramo(x, fs, voz)
        if len(xr) and len(xv) and xr.std() > 0:
            m["snr_ventanas_db"] = db(xv.std()) - db(xr.std())
    # Espectro (Welch) y reparto por banda
    f, P = signal.welch(x, fs, nperseg=min(8192, len(x)))
    df = f[1] - f[0] if len(f) > 1 else 1.0
    total = max(P.sum() * df, 1e-18)
    m["bandas_dbfs"] = []
    for lo, hi in BANDAS:
        sel = (f >= lo) & (f < hi)
        m["bandas_dbfs"].append(10 * np.log10(max(P[sel].sum() * df, 1e-18)) if sel.any() else -120.0)
    m["grave60_pct"] = 100 * P[f < 60].sum() * df / total
    Pdb = 10 * np.log10(P + 1e-18)
    pk, pr = signal.find_peaks(Pdb, prominence=15)
    graves = [(f[i], p) for i, p in zip(pk, pr["prominences"]) if 20 < f[i] < 60]
    m["tono_grave_hz"] = float(max(graves, key=lambda c: c[1])[0]) if graves else None
    m["zumbido"] = {}
    for h in (50, 60, 100, 120):
        cerca = (f > h - 1.5) & (f < h + 1.5)
        vecino = (f > h - 12) & (f < h + 12) & ~cerca
        if cerca.any() and vecino.any():
            m["zumbido"][h] = float(Pdb[cerca].max() - np.median(Pdb[vecino]))
    # Union de mensajes del firmware (cada 2048 muestras): solo se menciona si es claramente anormal
    dif = np.abs(np.diff(x))
    m["union_ratio"] = float(dif[2047::2048].mean() / max(dif.mean(), 1e-12)) if len(dif) > 4096 else None
    return m, dict(rms_db=rms_db, nfr=n, f=f, Pdb=Pdb)


# -- interpretacion -------------------------------------------------------------------------------------------
def interpretar_audio(m):
    h = []
    add = lambda area, nivel, texto: h.append(Hallazgo("audio", area, nivel, texto))
    # Nivel: se juzga la voz (percentil 95 de los cuadros), no el promedio con los silencios
    voz, pico = m["senal_dbfs"], m["pico_dbfs"]
    if m["saturadas"] or pico > -1:
        add("Nivel", "mal", f"Satura: pico en {num(pico)} dBFS ({m['saturadas']} muestras al tope). Bajar la ganancia.")
    elif voz < -50:
        add("Nivel", "mal", f"Muy bajo: la señal llega a {num(voz)} dBFS. Casi no hay audio útil.")
    elif voz < -40:
        add("Nivel", "aviso", f"Bajo: la señal llega a {num(voz)} dBFS (pico {num(pico)}). Margen de sobra, poca señal.")
    elif voz > -12:
        add("Nivel", "aviso", f"Alto: la señal llega a {num(voz)} dBFS, cerca del tope (pico {num(pico)}).")
    else:
        add("Nivel", "ok", f"Adecuado: señal en {num(voz)} dBFS, pico {num(pico)} dBFS, sin saturar.")
    # SNR
    if m["snr_ventanas_db"] is not None:
        s = m["snr_ventanas_db"]
        nv = "ok" if s >= 20 else "aviso" if s >= 10 else "mal"
        add("SNR", nv, f"{num(s)} dB entre los tramos de ruido y de voz que elegiste.")
    elif m["pct_activo"] < 5:
        add("SNR", "info", "No hay voz clara en el archivo: el SNR estimado no es representativo.")
    else:
        s = m["snr_est_db"]
        nv = "ok" if s >= 20 else "aviso" if s >= 10 else "mal"
        calif = "bueno" if s >= 20 else "aceptable" if s >= 10 else "bajo"
        add("SNR", nv, f"Estimado {num(s)} dB ({calif}); hay actividad en el {num(m['pct_activo'], 0)} % del tiempo.")
    # Graves
    g = m["grave60_pct"]
    tono = f" Tono estable en {num(m['tono_grave_hz'], 0)} Hz." if m["tono_grave_hz"] else ""
    if g > 40:
        add("Graves", "mal", f"Ruido grave alto: el {num(g, 0)} % de la energía está bajo 60 Hz.{tono} Conviene un pasa-altos.")
    elif g > 15:
        add("Graves", "aviso", f"Ruido grave moderado: {num(g, 0)} % de la energía bajo 60 Hz.{tono}")
    else:
        add("Graves", "ok", f"Controlado: solo el {num(g, 0)} % de la energía bajo 60 Hz.{tono}")
    # Continua
    salto = abs(m["dc_inicio_pct"] - m["dc_resto_pct"])
    if salto > 1:
        add("Continua", "aviso", f"Transitorio de arranque: el primer segundo está {num(salto)} % del fondo de escala fuera del resto.")
    elif abs(m["dc_pct"]) > 1:
        add("Continua", "aviso", f"Offset de continua de {num(m['dc_pct'])} % del fondo de escala.")
    else:
        add("Continua", "ok", f"Sin offset ({num(m['dc_pct'], 2)} %) ni transitorio de arranque.")
    # Solo si algo anda mal
    fuertes = {k: v for k, v in m["zumbido"].items() if v > 10}
    if fuertes:
        k = max(fuertes, key=fuertes.get)
        add("Zumbido", "mal", f"Zumbido de red en {k} Hz ({num(fuertes[k], 0)} dB sobre su entorno).")
    if m["union_ratio"] is not None and m["union_ratio"] > 2:
        add("Empaquetado", "mal", f"Hay saltos en las uniones de mensajes (x{num(m['union_ratio'])} lo normal): posible click del enlace.")
    return h


def _cargar_json(p):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _perdida_s(gaps):
    return sum(max(g.get("lost_samples", 0), g.get("missing_slots", 0) * 512) for g in gaps) / 16000


def interpretar_json(ruta):
    """Lee el .json del propio WAV y el resumen de la sesion (si es un segmento) y lo interpreta."""
    h = []
    add = lambda area, nivel, texto: h.append(Hallazgo("enlace", area, nivel, texto))
    propio = ruta.with_suffix(".json")
    mt = re.fullmatch(r"(.+)_(\d{3})", ruta.stem)
    resumen = ruta.parent / (mt.group(1) + ".json") if mt else None
    d = _cargar_json(propio) if propio.exists() else None
    r = _cargar_json(resumen) if resumen and resumen.exists() else None
    if d is None and r is None:
        return h
    if d is not None:
        gaps = d.get("gaps", [])
        perdido = _perdida_s(gaps)
        if not gaps:
            add("Huecos", "ok", "Sin huecos: el audio es continuo.")
        else:
            nv = "mal" if perdido > 1 else "aviso"
            add("Huecos", nv, f"{len(gaps)} hueco(s), {num(perdido)} s de audio perdidos (marcados, no rellenados).")
        if d.get("status") in ("interrupted", "incomplete"):
            motivo = (r or d).get("close_reason") or d.get("reason") or "conexión cerrada"
            add("Estado", "aviso", f"Cortado antes de completarse ({motivo}).")
        st = d.get("stats")
        if st:                                     # json de toma v1: trae las estadisticas del firmware
            if st.get("dma_overflows"):
                add("Captura", "mal", f"{st['dma_overflows']} overflow(s) del DMA durante la toma: se perdió audio en el I2S.")
            if st.get("reconnects"):
                add("Red", "aviso", f"{st['reconnects']} reconexión(es); espera máxima sin red {num(st.get('wait_max_ms', 0) / 1000)} s.")
            if "ring_max_slots" in st:
                s = st["ring_max_slots"] * 0.032
                nv = "aviso" if s > RING_S * 0.5 else "ok"
                add("Colchón", nv, f"El ring llegó a {num(s)} s de atraso (de {num(RING_S)} s disponibles).")
            if st.get("stack_sink") is not None:
                nv = "mal" if st["stack_sink"] < 500 else "aviso" if st["stack_sink"] < 1000 else "ok"
                add("Memoria", nv, f"Pila del sink: {st['stack_sink']} B libres; RAM interna libre {num(st.get('heap_internal_free', 0) / 1024, 0)} KB.")
    if r is not None:
        add("Sesión", "info", f"{num(r.get('duration_seconds', 0), 0)} s en {r.get('segments', 0)} segmento(s); fuente {r.get('source', '?')}.")
        gp = r.get("gap_from_previous_session_slots")
        if r.get("previous_session_restart"):
            add("Entre sesiones", "info", "El equipo se reinició desde la sesión anterior.")
        elif gp is None:
            add("Entre sesiones", "info", "Primera sesión que ve este servidor.")
        elif gp == 0:
            add("Entre sesiones", "ok", "Continúa sin cortes la sesión anterior.")
        else:
            add("Entre sesiones", "aviso", f"Faltan {gp} slots ({num(gp * 0.032)} s) respecto de la sesión anterior.")
    return h


# -- analisis de un archivo -----------------------------------------------------------------------------------
def analizar_archivo(ruta, ruido=None, voz=None, datos=None):
    """Lista de Resultado (uno por canal). `datos`=(fs, x2d, bits) permite analizar audio ya en memoria."""
    ruta = Path(ruta)
    fs, x2d, bits = datos if datos is not None else cargar(ruta)
    enlace = interpretar_json(ruta) if (datos is None and ruta.exists()) else []
    out = []
    for c in range(x2d.shape[1]):
        m, d = metricas(x2d[:, c], fs, ruido, voz)
        res = Resultado(ruta, ruta.name, fs, bits, c, x2d.shape[1], x2d[:, c], m, d)
        res.hallazgos = interpretar_audio(m) + enlace
        out.append(res)
    return out


# -- dibujo ---------------------------------------------------------------------------------------------------
def _envolvente(ax, x, fs, ancho=3000):
    """Forma de onda como banda min-max (no dibuja millones de puntos)."""
    bloque = max(len(x) // ancho, 1)
    k = len(x) // bloque
    xb = x[:k * bloque].reshape(k, bloque)
    t = (np.arange(k) + 0.5) * bloque / fs
    ax.fill_between(t, xb.min(1), xb.max(1), color=CIAN, alpha=0.9, lw=0)
    ax.set_xlim(0, len(x) / fs)
    ax.set_ylim(-1, 1)


def _tarjetas(ax, res):
    ax.axis("off")
    ax.text(0, 1.0, res.nombre + (f"   ·   canal {res.canal}" if res.n_canales > 1 else ""),
            fontsize=15, fontweight="bold", va="top", transform=ax.transAxes)
    ax.text(0, 0.84, f"{res.fs} Hz · {res.bits} bit · {num(res.m['dur_s'])} s", fontsize=9.5, color=MUTED,
            va="top", transform=ax.transAxes)
    columnas = [[h for h in res.hallazgos if h.grupo == "audio"], [h for h in res.hallazgos if h.grupo == "enlace"]]
    titulos = ["AUDIO", "ENLACE Y FIRMWARE"]
    if not columnas[1]:
        columnas, titulos = [columnas[0]], ["AUDIO"]
    ancho = 1.0 / len(columnas)
    cols_txt = 70 if len(columnas) == 2 else 150
    for c, (hs, tit) in enumerate(zip(columnas, titulos)):
        x0 = c * ancho
        ax.text(x0, 0.68, tit, fontsize=8.5, color=MUTED, fontweight="bold", va="top", transform=ax.transAxes)
        textos = [_ajustar(hz.texto, cols_txt) for hz in hs]
        renglon, hueco = 0.062, 0.05                       # alto de un renglon y espacio entre hallazgos (fraccion del eje)
        total = sum(tx.count("\n") + 1 for tx in textos) * renglon + hueco * len(hs)
        k = min(1.0, 0.60 / total) if total else 1.0         # si no entra, se comprime
        y = 0.60
        for hz, tx in zip(hs, textos):
            ax.text(x0, y, f"● {hz.area}", fontsize=9.5, fontweight="bold", color=COLOR_NIVEL[hz.nivel],
                    va="top", transform=ax.transAxes)
            ax.text(x0 + 0.115 * (1 if len(columnas) == 2 else 0.5), y, tx, fontsize=9, color=TXT,
                    va="top", transform=ax.transAxes)
            y -= ((tx.count("\n") + 1) * renglon + hueco) * k


def _ajustar(texto, ancho):
    """Corta un texto largo en renglones."""
    palabras, lineas, actual = texto.split(), [], ""
    for p in palabras:
        if len(actual) + len(p) + 1 > ancho:
            lineas.append(actual)
            actual = p
        else:
            actual = f"{actual} {p}".strip()
    lineas.append(actual)
    return "\n".join(lineas)


def dibujar(res, destino):
    m, d, fs, x = res.m, res.datos, res.fs, res.x
    with plt.rc_context(TEMA):
        fig = plt.figure(figsize=(16, 12.2))
        gs = fig.add_gridspec(5, 2, height_ratios=[1.85, 1.0, 1.0, 1.35, 1.65], width_ratios=[1.6, 1],
                              hspace=0.6, wspace=0.2, left=0.05, right=0.97, top=0.97, bottom=0.05)
        _tarjetas(fig.add_subplot(gs[0, :]), res)

        a = fig.add_subplot(gs[1, :])
        _envolvente(a, x, fs)
        a.set_title("Señal en el tiempo")
        a.set_ylabel("amplitud (FS)")

        a = fig.add_subplot(gs[2, :], sharex=a)
        tt = (np.arange(len(d["rms_db"])) + 0.5) * d["nfr"] / fs
        a.fill_between(tt, -120, d["rms_db"], color=VIOLETA, alpha=0.25, lw=0)
        a.plot(tt, d["rms_db"], color=VIOLETA, lw=1)
        a.axhline(m["ruido_dbfs"], color=VERDE, ls="--", lw=1, label=f"ruido (p10) {num(m['ruido_dbfs'], 0)} dBFS")
        a.axhline(m["senal_dbfs"], color=NARANJA, ls="--", lw=1, label=f"señal (p95) {num(m['senal_dbfs'], 0)} dBFS")
        a.set_ylim(max(m["ruido_dbfs"] - 15, -110), min(m["pico_dbfs"] + 6, 0))
        a.set_title(f"Nivel por cuadro de 32 ms · señal − ruido (p95 − p10) = {num(m['snr_est_db'])} dB")
        a.set_ylabel("dBFS")
        a.legend(loc="lower right", fontsize=8)

        a = fig.add_subplot(gs[3, 0])
        f, P = d["f"], d["Pdb"]
        a.semilogx(f[1:], P[1:], color=CIAN, lw=1)
        a.set_xlim(max(f[1], 1), fs / 2)
        for h in (50, 60, 100, 120):
            a.axvline(h, color=ROJO, alpha=0.18, lw=1)
        a.axvspan(1, 60, color=ROJO, alpha=0.07)
        a.set_title("Espectro (Welch) · zona roja: bajo 60 Hz")
        a.set_xlabel("Hz")
        a.set_ylabel("dB")

        a = fig.add_subplot(gs[3, 1])
        etiquetas = [f"{lo // 1000}k–{hi // 1000}k" if lo >= 1000 else f"{lo}–{hi}" for lo, hi in BANDAS]
        colores = [ROJO if hi <= 60 else CIAN for lo, hi in BANDAS]
        piso = min(m["bandas_dbfs"]) - 10                    # las barras crecen desde el piso hacia la energia
        a.barh(range(len(BANDAS)), [v - piso for v in m["bandas_dbfs"]], left=piso, color=colores, height=0.62)
        a.set_yticks(range(len(BANDAS)), [e + " Hz" for e in etiquetas])
        a.invert_yaxis()
        for i, v in enumerate(m["bandas_dbfs"]):
            a.text(v + 0.8, i, f"{num(v, 0)}", va="center", color=TXT, fontsize=8)
        a.set_xlim(piso, max(m["bandas_dbfs"]) + 8)
        a.set_title("Energía por banda (dBFS)")
        a.grid(axis="y", visible=False)

        a = fig.add_subplot(gs[4, :])
        ff, st, S = signal.spectrogram(x, fs, nperseg=1024, noverlap=768)
        paso = max(int(np.ceil(S.shape[1] / 2500)), 1)         # archivos largos: promedio en el tiempo
        if paso > 1:
            k = S.shape[1] // paso
            S = S[:, :k * paso].reshape(S.shape[0], k, paso).mean(2)
            st = st[:k * paso].reshape(k, paso).mean(1)
        Sdb = 10 * np.log10(S + 1e-18)
        im = a.pcolormesh(st, ff, Sdb, shading="auto", cmap="magma",
                          vmin=np.percentile(Sdb, 5), vmax=np.percentile(Sdb, 99.7), rasterized=True)
        a.set_ylim(0, min(fs / 2, 8000))
        a.set_title("Espectrograma")
        a.set_xlabel("s")
        a.set_ylabel("Hz")
        a.grid(False)
        cb = fig.colorbar(im, ax=a, pad=0.008, fraction=0.025)
        cb.set_label("dB", color=MUTED)
        cb.ax.tick_params(colors=MUTED)
        cb.outline.set_edgecolor(GRID)
        fig.savefig(destino, dpi=DPI)
        plt.close(fig)


def dibujar_evolucion(destino, filas):
    campos = [("snr_est_db", "SNR estimado (dB)", CIAN), ("senal_dbfs", "señal (dBFS)", NARANJA),
              ("grave60_pct", "energía bajo 60 Hz (%)", ROJO), ("huecos_s", "audio perdido (s)", AMBAR)]
    with plt.rc_context(TEMA):
        fig, ax = plt.subplots(len(campos), 1, figsize=(14, 2.7 * len(campos)), sharex=True)
        for a, (k, tit, col) in zip(ax, campos):
            a.plot(range(len(filas)), [r[k] for r in filas], ".-", color=col, lw=1.2, ms=4)
            a.set_title(tit, fontsize=10)
        ax[-1].set_xlabel("archivo (orden alfabético)")
        fig.suptitle(f"Evolución en {len(filas)} archivos", color=TXT, fontsize=13, fontweight="bold")
        fig.tight_layout()
        fig.savefig(destino, dpi=DPI)
        plt.close(fig)


# -- consola y CLI --------------------------------------------------------------------------------------------
def imprimir(res):
    cab = f"{res.nombre}" + (f" [canal {res.canal}]" if res.n_canales > 1 else "")
    print(f"\n{cab}  -  {res.fs} Hz, {res.bits} bit, {num(res.m['dur_s'])} s")
    for g in ("audio", "enlace"):
        hs = [h for h in res.hallazgos if h.grupo == g]
        if hs:
            print(f"  {'AUDIO' if g == 'audio' else 'ENLACE Y FIRMWARE'}")
        for h in hs:
            print(ascii_(f"    [{ETIQUETA[h.nivel]:8s}] {h.area:15s} {h.texto}"))


def juntar_archivos(entradas):
    rutas = []
    for e in entradas:
        p = Path(e)
        if p.is_dir():
            rutas += sorted(p.glob("*.wav"))
        elif p.exists():
            rutas.append(p)
        else:
            sys.exit(f"No existe: {e}")
    if not rutas:
        sys.exit("No hay archivos .wav para analizar.")
    return rutas


def fila_resumen(res):
    huecos = next((h for h in res.hallazgos if h.area == "Huecos"), None)
    perdido = 0.0
    if huecos and huecos.nivel != "ok":
        mt = re.search(r"([\d,]+) s de audio", huecos.texto)
        perdido = float(mt.group(1).replace(",", ".")) if mt else 0.0
    return dict(archivo=res.nombre, dur_s=res.m["dur_s"], pico_dbfs=res.m["pico_dbfs"], senal_dbfs=res.m["senal_dbfs"],
                snr_est_db=res.m["snr_est_db"], grave60_pct=res.m["grave60_pct"], huecos_s=perdido,
                problemas=sum(h.nivel == "mal" for h in res.hallazgos))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("entradas", nargs="+", help="WAV, varios WAV o carpetas con WAV")
    ap.add_argument("--unir", action="store_true", help="pegar los WAV en orden y analizar como uno")
    ap.add_argument("--ruido", nargs=2, type=float, metavar=("T0", "T1"), help="tramo de solo ruido (s)")
    ap.add_argument("--voz", nargs=2, type=float, metavar=("T0", "T1"), help="tramo con voz (s)")
    ap.add_argument("--salida", type=Path, help="carpeta de las imagenes (por defecto: 'analisis' junto al WAV)")
    ap.add_argument("--sin-png", action="store_true", help="solo consola")
    ap.add_argument("--max-png", type=int, default=8, help="con mas archivos que esto no se dibuja uno por archivo")
    ap.add_argument("--csv", type=Path, help="guardar la tabla comparativa en CSV")
    args = ap.parse_args()

    rutas = juntar_archivos(args.entradas)
    if args.unir and len(rutas) > 1:
        partes = [cargar(r) for r in rutas]
        if len({(p[0], p[1].shape[1]) for p in partes}) != 1:
            sys.exit("Para --unir los WAV tienen que tener la misma frecuencia y canales.")
        unido = (partes[0][0], np.concatenate([p[1] for p in partes]), partes[0][2])
        trabajos = [(Path(f"{rutas[0].stem}_a_{rutas[-1].stem}.wav"), unido, rutas[0].parent)]
    else:
        trabajos = [(r, None, r.parent) for r in rutas]

    pocos = len(trabajos) <= args.max_png
    filas = []
    for ruta, datos, carpeta_origen in trabajos:
        resultados = analizar_archivo(ruta, args.ruido, args.voz, datos)
        for res in resultados:
            if pocos:
                imprimir(res)
            if pocos and not args.sin_png:
                carpeta = args.salida or carpeta_origen / "analisis"
                carpeta.mkdir(parents=True, exist_ok=True)
                png = carpeta / f"{ruta.stem}{'_ch%d' % res.canal if res.n_canales > 1 else ''}_analisis.png"
                dibujar(res, png)
                print(f"  -> {png}")
        filas.append(fila_resumen(resultados[0]))

    if len(trabajos) > 1:
        print(f"\n{'archivo':56s} {'dur':>6s} {'pico':>6s} {'señal':>6s} {'SNR':>6s} {'<60Hz%':>7s} {'perdido':>8s} {'prob':>5s}".replace("ñ", "n"))
        for r in filas:
            print(f"{r['archivo'][-56:]:56s} {r['dur_s']:6.1f} {r['pico_dbfs']:6.1f} {r['senal_dbfs']:6.1f} "
                  f"{r['snr_est_db']:6.1f} {r['grave60_pct']:7.1f} {r['huecos_s']:7.1f}s {r['problemas']:5d}")
        if not args.sin_png and not pocos:
            carpeta = args.salida or trabajos[0][2] / "analisis"
            carpeta.mkdir(parents=True, exist_ok=True)
            png = carpeta / f"resumen_{len(trabajos)}_archivos.png"
            dibujar_evolucion(png, filas)
            print(f"-> {png}")
    if args.csv:
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(filas[0]))
            w.writeheader()
            w.writerows(filas)
        print(f"-> {args.csv}")


if __name__ == "__main__":
    main()
