"""Ilustraciones isométricas (estilo de la plantilla Slidesgo, en azul marino). Sale assets/ilus/*.svg.

Proyección: X = (x - y)·cos30°, Y = (x + y)·sin30° - z. Cajas con 3 caras visibles (arriba, frente-izq., frente-der.),
cilindros verticales y texto apoyado en el plano de arriba. Se dibuja de atrás hacia adelante (orden por x + y + z).
"""
import math
from pathlib import Path

C30, S30 = math.cos(math.radians(30)), 0.5
OUT = Path(__file__).resolve().parent.parent / "assets" / "ilus"
LINEA = "#14213d"
# (arriba, izquierda, derecha) por material
MAT = {
    "pcb": ("#264a7d", "#1a355c", "#122847"),
    "pcb2": ("#33598f", "#22426d", "#183456"),
    "metal": ("#f4f6f9", "#cfd7e3", "#aab8cc"),
    "blanco": ("#ffffff", "#dfe5ee", "#c3cedc"),
    "gris": ("#d9dfe8", "#b6c1d1", "#97a6bb"),
    "negro": ("#2b3446", "#1d2433", "#151b27"),
    "oro": ("#e3c27a", "#c9a45c", "#a8853f"),
    "acento": ("#e07a5f", "#c8553d", "#a54430"),
    "acero": ("#8fb0d6", "#6a90bd", "#52769f"),
}


def p(x, y, z):
    return ((x - y) * C30, (x + y) * S30 - z)


def poly(pts, fill, sw=1.6, extra=""):
    s = " ".join(f"{a:.1f},{b:.1f}" for a, b in pts)
    return f'<polygon points="{s}" fill="{fill}" stroke="{LINEA}" stroke-width="{sw}" stroke-linejoin="round" {extra}/>'


class Escena:
    def __init__(self):
        self.items, self.xs, self.ys, self.n = [], [], [], 0

    def _b(self, pts):
        for a, b in pts:
            self.xs.append(a); self.ys.append(b)

    def caja(self, x, y, z, w, d, h, mat="blanco", sw=1.6, orden=None):
        t, l, r = MAT[mat]
        top = [p(x, y, z + h), p(x + w, y, z + h), p(x + w, y + d, z + h), p(x, y + d, z + h)]
        izq = [p(x, y + d, z), p(x + w, y + d, z), p(x + w, y + d, z + h), p(x, y + d, z + h)]
        der = [p(x + w, y, z), p(x + w, y + d, z), p(x + w, y + d, z + h), p(x + w, y, z + h)]
        svg = poly(izq, l, sw) + poly(der, r, sw) + poly(top, t, sw)
        self._b(top + izq + der)
        self.n += 1; self.items.append((orden if orden is not None else self.n, svg))

    def cil(self, x, y, z, r, h, mat="metal", sw=1.6, orden=None, tapa=None):
        t, l, _ = MAT[mat]
        X0, Y0 = p(x, y, z); X1, Y1 = p(x, y, z + h)
        rx, ry = r * math.sqrt(2) * C30, r * math.sqrt(2) * S30
        g = (f'<path d="M{X0-rx:.1f},{Y0:.1f} A{rx:.1f},{ry:.1f} 0 0 0 {X0+rx:.1f},{Y0:.1f} L{X1+rx:.1f},{Y1:.1f} '
             f'L{X1-rx:.1f},{Y1:.1f} Z" fill="{l}" stroke="{LINEA}" stroke-width="{sw}" stroke-linejoin="round"/>'
             f'<ellipse cx="{X1:.1f}" cy="{Y1:.1f}" rx="{rx:.1f}" ry="{ry:.1f}" fill="{t}" stroke="{LINEA}" stroke-width="{sw}"/>')
        if tapa:
            g += f'<ellipse cx="{X1:.1f}" cy="{Y1:.1f}" rx="{rx*0.45:.1f}" ry="{ry*0.45:.1f}" fill="{tapa}" stroke="{LINEA}" stroke-width="{sw*0.8}"/>'
        self._b([(X0 - rx, Y0 + ry), (X1 + rx, Y1 - ry)])
        self.n += 1; self.items.append((orden if orden is not None else self.n, g))

    def texto(self, x, y, z, s, tam=10, color=LINEA, peso=600, orden=None):
        X, Y = p(x, y, z)
        self.n += 1
        self.items.append((orden if orden is not None else self.n, f'<text transform="matrix({C30},{S30},{-C30},{S30},{X:.1f},{Y:.1f})" '
                                  f'font-family="Barlow, sans-serif" font-weight="{peso}" font-size="{tam}" fill="{color}">{s}</text>'))

    def raw(self, svg, orden=None, bounds=()):
        self._b(bounds)
        self.n += 1
        self.items.append((orden if orden is not None else self.n, svg))

    def guardar(self, nombre, pad=14):
        x0, x1, y0, y1 = min(self.xs) - pad, max(self.xs) + pad, min(self.ys) - pad, max(self.ys) + pad
        cuerpo = "".join(s for _, s in sorted(self.items, key=lambda t: t[0]))
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / f"{nombre}.svg").write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x0:.0f} {y0:.0f} {x1-x0:.0f} {y1-y0:.0f}">{cuerpo}</svg>',
            encoding="utf-8")


def ondas(e, x, y, z, n=3, r0=18, dr=14, desde=-60, hasta=60, color="#33598f", orden=None):
    """Arcos de sonido en el plano horizontal, saliendo hacia -x (hacia adelante-izquierda)."""
    for k in range(n):
        r = r0 + k * dr
        pts = [p(x - r * math.cos(math.radians(a)), y + r * math.sin(math.radians(a)), z)
               for a in range(desde, hasta + 1, 5)]
        d = "M" + " L".join(f"{a:.1f},{b:.1f}" for a, b in pts)
        e.raw(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{3.2 - k*0.6:.1f}" stroke-linecap="round" '
              f'opacity="{1 - k*0.22:.2f}"/>', orden, pts)


def modulo_s3(e, x, y, z, esc=1.0):
    """Módulo ESP32-S3-WROOM: base de PCB, blindaje metálico, zona de antena con meandro."""
    w, d = 72 * esc, 50 * esc
    e.caja(x, y, z, w, d, 3 * esc, "pcb2")
    e.caja(x + 18 * esc, y + 4 * esc, z + 3 * esc, w - 22 * esc, d - 8 * esc, 7 * esc, "metal")
    e.texto(x + 30 * esc, y + 10 * esc, z + 10 * esc, "ESP32-S3", 7 * esc)
    e.texto(x + 30 * esc, y + 22 * esc, z + 10 * esc, "N16R8", 5.5 * esc, "#5a6b85", 500)
    # meandro de antena
    pts, yy = [], y + 6 * esc
    for k in range(7):
        xa = x + (3 + k * 2) * esc
        pts += [p(xa, yy, z + 3 * esc), p(xa, y + d - 6 * esc, z + 3 * esc)] if k % 2 == 0 else \
               [p(xa, y + d - 6 * esc, z + 3 * esc), p(xa, yy, z + 3 * esc)]
    e.raw('<path d="M' + " L".join(f"{a:.1f},{b:.1f}" for a, b in pts) +
          f'" fill="none" stroke="#e3c27a" stroke-width="{1.6*esc:.1f}"/>', )


def tapa(mics=True, nombre="placa"):
    e = Escena()
    e.caja(0, 0, 0, 240, 150, 8, "pcb")
    for k in range(14):                                   # tira de pines de atrás
        e.caja(14 + k * 15, 6, 8, 7, 7, 10, "negro")
    modulo_s3(e, 70, 40, 8, 1.15)
    e.caja(222, 62, 8, 20, 26, 9, "metal")                 # USB-C
    e.caja(30, 30, 8, 22, 18, 4, "negro"); e.texto(34, 36, 12, "LDO", 6, "#c3cedc")
    e.caja(30, 100, 8, 16, 16, 7, "blanco"); e.caja(186, 110, 8, 14, 14, 6, "blanco")
    for k in range(14):                                   # tira de pines de adelante
        e.caja(14 + k * 15, 137, 8, 7, 7, 10, "negro")
    # dos mics INMP441 en endfire sobre el borde (ruta 21,4 mm)
    for i, yy in enumerate((36, 84) if mics else ()):
        e.caja(-46, yy, 8, 34, 30, 3, "pcb2")
        e.cil(-29, yy + 15, 11, 8, 5, "metal", tapa="#14213d")
    if mics:
        ondas(e, -60, 75, 22, n=3, r0=22, dr=18, desde=-55, hasta=55)
    e.guardar(nombre)


def placa_sola():
    tapa(mics=False, nombre="placa_sola")


def array():
    e = Escena()
    e.caja(0, 0, 0, 200, 70, 6, "pcb")
    for i, xx in enumerate((50, 150)):
        e.cil(xx, 35, 6, 14, 8, "metal", tapa="#14213d")
    # cota d
    a, b = p(50, 62, 6), p(150, 62, 6)
    e.raw(f'<path d="M{a[0]:.1f},{a[1]+14:.1f} L{b[0]:.1f},{b[1]+14:.1f}" stroke="#c8553d" stroke-width="2.2"/>'
          f'<circle cx="{a[0]:.1f}" cy="{a[1]+14:.1f}" r="3.5" fill="#c8553d"/><circle cx="{b[0]:.1f}" cy="{b[1]+14:.1f}" r="3.5" fill="#c8553d"/>',
          None, [a, b])
    # frentes de onda curvos: arcos centrados en una fuente sobre el eje, a la izquierda (endfire)
    sx, sy, sz = -125, 35, 18
    for k, r in enumerate((112, 88, 64, 40)):
        pts = [p(sx + r * math.cos(math.radians(a)), sy + r * math.sin(math.radians(a)), sz) for a in range(-42, 43, 3)]
        d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts)
        e.raw(f'<path d="{d}" fill="none" stroke="#2f4f86" stroke-width="{3.6 - k*0.6:.1f}" stroke-linecap="round" '
              f'opacity="{1 - k*0.2:.2f}"/>', None, pts)
    e.guardar("array")


def red():
    e = Escena()
    e.caja(0, 0, 0, 150, 150, 10, "negro")
    for k in range(10):                                   # patas del QFN
        for (x, y, w, d) in ((8 + k * 13.5, 150, 6, 4), (150, 8 + k * 13.5, 4, 6)):
            e.caja(x, y, 0, w, d, 3, "metal", 1.0)
    e.texto(30, 52, 10, "ESP32-S3", 16, "#dfe5ee")
    # capas de la red, flotando encima
    capas = [(150, 40, "acero"), (130, 32, "pcb2"), (112, 32, "pcb2"), (96, 32, "pcb2"), (82, 32, "pcb2")]
    z = 60
    for i, (lado, alto, mat) in enumerate(capas):
        o = (150 - lado) / 2
        e.caja(o, o, z, lado, lado, 6, mat)
        if i == 0:                                        # columnas del espectrograma sobre la primera capa
            for c in range(12):
                for f in range(8):
                    v = (math.sin(c * 0.9 + f * 0.6) + 1) / 2
                    col = f"rgb({int(30+200*v)},{int(60+160*v)},{int(110+120*v)})"
                    x0, y0 = o + 6 + c * 11.5, o + 6 + f * 17.5
                    e.raw(poly([p(x0, y0, z + 6), p(x0 + 10, y0, z + 6), p(x0 + 10, y0 + 15, z + 6), p(x0, y0 + 15, z + 6)],
                               col, 0.5))
        z += alto
    e.cil(75, 75, z + 10, 12, 10, "acento")   # la neurona de salida
    ab, ar = p(75, 75, z - 26), p(75, 75, z + 8)
    e.raw(f'<path d="M{ab[0]:.1f},{ab[1]:.1f} L{ar[0]:.1f},{ar[1]:.1f}" stroke="{LINEA}" stroke-width="1.6" stroke-dasharray="4 4"/>')
    e.guardar("red")


def chip():
    e = Escena()
    e.caja(0, 0, 0, 120, 120, 9, "negro")
    for k in range(9):
        for (x, y, w, d) in ((8 + k * 12.5, 120, 6, 4), (120, 8 + k * 12.5, 4, 6)):
            e.caja(x, y, 0, w, d, 3, "metal", 1.0)
    e.texto(22, 40, 9, "ESP32-S3", 15, "#dfe5ee"); e.texto(22, 62, 9, "Xtensa LX7 ×2", 9, "#97a6bb", 500)
    e.guardar("chip")


def deco():
    e = Escena()                                          # mic suelto, como los carretes de la plantilla
    e.caja(0, 0, 0, 40, 40, 4, "pcb2"); e.cil(20, 20, 4, 11, 6, "metal", tapa="#14213d")
    e.guardar("deco_mic")
    e = Escena()
    e.caja(0, 0, 0, 36, 36, 6, "negro")
    for k in range(4):
        e.caja(4 + k * 8.5, 36, 0, 4, 3, 2, "metal", 0.8); e.caja(36, 4 + k * 8.5, 0, 3, 4, 2, "metal", 0.8)
    e.guardar("deco_chip")
    e = Escena()
    e.cil(0, 0, 0, 16, 8, "acero"); e.cil(0, 0, 8, 10, 10, "blanco", tapa="#c8553d")
    e.guardar("deco_cap")


def dispositivo():
    """Órbita sobre el escritorio: caja con parlante y los mics al frente."""
    e = Escena()
    e.caja(0, 0, 0, 260, 170, 4, "gris")
    e.caja(60, 40, 4, 120, 90, 46, "blanco")
    for k in range(5):
        e.cil(150, 48 + k * 18, 50, 3, 0.5, "negro", 1.0)
    e.cil(110, 85, 50, 26, 2, "gris", tapa="#14213d")
    e.cil(66, 70, 50, 5, 1, "metal", 1.0); e.cil(66, 100, 50, 5, 1, "metal", 1.0)
    e.cil(170, 120, 50, 4, 1, "acento", 1.0)
    ondas(e, 40, 85, 40, n=3, r0=24, dr=18, desde=-50, hasta=50)
    e.guardar("dispositivo")


if __name__ == "__main__":
    for f in (tapa, placa_sola, array, red, chip, deco, dispositivo):
        f()
    print(sorted(x.name for x in OUT.iterdir()))
