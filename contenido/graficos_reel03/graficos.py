"""Gráficos del reel 03 (wake word) desde entrenamiento.log del v0.

Uso: python graficos.py <ruta a entrenamiento.log>
Sale: 1_error_entrenamiento.png, 2_nota_validacion.png, 3_certeza_ruido.png (1080x1350).
Estética: la de hiorbita.com (ver plan_general.md, "Estética").
El gráfico 3 usa los máximos en MUSAN noise de resultados_v0.md ("Robustez", punto 4), no el log.
"""
import logging
import re
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib import font_manager
from matplotlib.ticker import FuncFormatter

# Paleta de hiorbita.com: fondo suave, texto "espacio", azul de acento
FONDO, TEXTO, TEXTO_2, GRILLA, CRUDO = "#EEF3FA", "#0C1526", "#5C6B82", "#E2E8F0", "#B9C0CC"
C_11K, C_45K = "#2F6BFF", "#94A3B8"  # el modelo elegido en azul de acento, el otro en gris pizarra (gráficos 2 y 3)
SALIDA = Path(__file__).parent

# Fuentes de la web (contenido/fuentes/, Regular y SemiBold): títulos en Geist, el resto en Instrument Sans
for ttf in (SALIDA.parent / "fuentes").glob("*.ttf"):
    font_manager.fontManager.addfont(str(ttf))
logging.getLogger("matplotlib.font_manager").setLevel(logging.ERROR)  # si faltan, usa Segoe UI sin avisar

plt.rcParams.update({
    "figure.facecolor": FONDO, "axes.facecolor": FONDO, "savefig.facecolor": FONDO,
    "text.color": TEXTO, "axes.labelcolor": TEXTO_2, "xtick.color": TEXTO_2, "ytick.color": TEXTO_2,
    "axes.edgecolor": GRILLA, "font.size": 26,
    "font.family": ["Instrument Sans", "Segoe UI", "DejaVu Sans"],
})


def coma(x, decimales):
    return f"{x:.{decimales}f}".replace(".", ",")


def miles(x, _=None):
    return f"{int(x):,}".replace(",", ".")


def figura(titulo, subtitulo):
    fig, ax = plt.subplots(figsize=(10.8, 13.5), dpi=100)
    fig.subplots_adjust(left=0.17, right=0.93, top=0.80, bottom=0.12)
    fig.text(0.06, 0.92, titulo, fontsize=58, color=TEXTO,  # la web usa títulos grandes de peso normal
             fontfamily=["Geist", "Segoe UI", "DejaVu Sans"])
    fig.text(0.06, 0.875, subtitulo, fontsize=28, color=TEXTO_2)
    for lado in ("top", "right"):
        ax.spines[lado].set_visible(False)
    ax.grid(axis="y", color=GRILLA, linewidth=1)
    ax.set_axisbelow(True)
    return fig, ax


def leer_log(ruta):
    texto = Path(ruta).read_text(encoding="utf-8", errors="ignore")
    # Una línea por paso de entrenamiento (la librería la rotula "Validation Batch", pero es el lote de entrenamiento)
    error = np.array([float(m) for m in re.findall(r"Loss = ([0-9.]+); Mini-Batch", texto)])
    evals = re.findall(r"Step (\d+) \(nonstreaming\).*?average viable recall = ([0-9.]+)", texto)
    pasos = np.array([int(p) for p, _ in evals])
    avr = np.array([float(a) for _, a in evals])
    return error, pasos, avr


def grafico_error(error):
    ventana = 250  # media móvil: el error por lote es muy ruidoso
    suave = np.convolve(error, np.ones(ventana) / ventana, mode="valid")
    pasos = np.arange(len(suave)) + ventana
    fig, ax = figura("Error en entrenamiento", "Con sus propios ejemplos, en escala logarítmica")
    ax.plot(pasos, suave, color=TEXTO, linewidth=3)
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: coma(y, max(0, -int(np.floor(np.log10(y)))))))
    ax.xaxis.set_major_formatter(FuncFormatter(miles))
    ax.set_xlabel("Paso de entrenamiento", labelpad=16)
    ax.set_ylim(0.001, 0.2)
    for paso, dx, dy in ((1000, 60, 40), (len(suave) + ventana - 1, -60, 120)):
        y = suave[paso - ventana]
        ax.annotate(coma(y, 4 if y < 0.01 else 3), (paso, y), xytext=(dx, dy), textcoords="offset points",
                    ha="left" if dx > 0 else "right", fontsize=34, weight="bold", color=TEXTO,
                    bbox=dict(facecolor=FONDO, edgecolor="none", pad=4),
                    arrowprops=dict(arrowstyle="-", color=TEXTO_2, linewidth=1.5))
    fig.savefig(SALIDA / "1_error_entrenamiento.png")
    plt.close(fig)


def grafico_avr(pasos, avr):
    fig, ax = figura("Nota con audio nuevo", "Validación: audio que no usó para entrenar")
    # Cada evaluación es muy ruidosa: la tendencia es la media móvil de 9 evaluaciones (4.500 pasos)
    ax.plot(pasos, avr, color=CRUDO, linewidth=1.5)
    tendencia = np.convolve(avr, np.ones(9) / 9, mode="valid")
    ax.plot(pasos[4:-4], tendencia, color=TEXTO, linewidth=5)
    ax.text(40000, 0.80, "Tendencia", ha="center", fontsize=28, color=TEXTO)
    # Etiquetas en zonas vacías: la del 11.000 arriba de los picos, la del 45.000 abajo de la curva
    for paso, color, y_texto, ha in ((11000, C_11K, 0.975, "center"), (45000, C_45K, 0.56, "right")):
        y = avr[pasos == paso][0]
        ax.plot(paso, y, "o", markersize=18, color=color, markeredgecolor=FONDO, markeredgewidth=3, zorder=3)
        ax.annotate(f"Paso {miles(paso)}: {coma(y, 2)}", (paso, y), xytext=(paso, y_texto), ha=ha, va="center",
                    fontsize=32, weight="bold", color=TEXTO,
                    arrowprops=dict(arrowstyle="-", color=color, linewidth=2))
    ax.set_ylim(0.4, 1.0)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: coma(y, 1)))
    ax.xaxis.set_major_formatter(FuncFormatter(miles))
    ax.set_xlabel("Paso de entrenamiento", labelpad=16)
    fig.savefig(SALIDA / "2_nota_validacion.png")
    plt.close(fig)


def grafico_certeza():
    fig, ax = figura("Certeza con puro ruido", "Máximo de \"es Órbita\" en 6 h de ruido")
    modelos, valores, colores = ["Paso 11.000", "Paso 45.000"], [0.41, 0.995], [C_11K, C_45K]
    barras = ax.bar(modelos, valores, width=0.55, color=colores)
    for b, v in zip(barras, valores):
        ax.text(b.get_x() + b.get_width() / 2, v + 0.03, f"{coma(v * 100, 1 if v > 0.9 else 0)} %",
                ha="center", fontsize=46, weight="bold", color=TEXTO)
    ax.set_ylim(0, 1.12)
    ax.set_yticks([0, 0.25, 0.5, 0.75, 1.0])
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{int(y * 100)} %"))
    ax.tick_params(axis="x", labelsize=32, colors=TEXTO, length=0, pad=14)
    fig.savefig(SALIDA / "3_certeza_ruido.png")
    plt.close(fig)


if __name__ == "__main__":
    error, pasos, avr = leer_log(sys.argv[1])
    print(f"{len(error)} pasos, {len(pasos)} evaluaciones")
    grafico_error(error)
    grafico_avr(pasos, avr)
    grafico_certeza()
