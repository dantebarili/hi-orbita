"""Textura de papel arrugado, gris claro y sutil (sale assets/papel.jpg, 1920x1080)."""
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

W, H = 1920, 1080
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
alt = np.zeros((H, W), np.float32)
# Pliegues: rectas al azar; la altura es una "carpa" a cada lado de la recta (crece con la distancia y se corta)
for _ in range(110):
    x0, y0 = rng.uniform(0, W), rng.uniform(0, H)
    a = rng.uniform(0, np.pi)
    d = (xx - x0) * np.sin(a) - (yy - y0) * np.cos(a)
    alt += rng.uniform(0.3, 1.0) * np.clip(np.abs(d) / 180.0, 0, 1) * rng.choice([-1, 1])
alt = gaussian_filter(alt * 40, 1.2) + gaussian_filter(rng.normal(0, 1, (H, W)).astype(np.float32), 30) * 40
gy, gx = np.gradient(alt)
luz = -(gx * 0.7 + gy * 0.7)                      # luz desde arriba a la izquierda
luz = luz / (np.percentile(np.abs(luz), 99) + 1e-6)
grano = gaussian_filter(rng.normal(0, 1, (H, W)).astype(np.float32), 0.8)
v = 239 + 5.5 * np.clip(luz, -1.5, 1.5) + 1.3 * grano
img = np.clip(np.stack([v, v, v + 0.6], -1), 0, 255).astype(np.uint8)
salida = Path(__file__).resolve().parent.parent / "assets" / "papel.jpg"
Image.fromarray(img).save(salida, quality=86)
print(salida, salida.stat().st_size // 1024, "KB")
