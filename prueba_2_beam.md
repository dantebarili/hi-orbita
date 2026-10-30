# Prueba 2 — beamforming propio (30/10)

Rama `prueba-beamforming` (sin commitear). Diseño: hipercardioide diferencial endfire, mics a 21,4 mm (1 muestra), `c_F − 0,5·c_B` + integrador (fc 150 Hz). Artifact "Array diferencial de 2 mics".

## Qué hay
- `firmware/components/orbita_beamforming/`: beamformer en C puro.
- `firmware/main/test_bf.c`: modo menuconfig *Prueba de beamforming*. Graba 5,12 s y manda un WAV de 3 canales `[L, R, BF]` por UART0.
- `tools/analizar_beamforming.py`: compara el firmware contra Python y mide nivel por banda y retardo R vs L.
- `audio_capture.c`: pull-down interna en GPIO6 (DIN).
- Tomas en `tools/tools/prueba_beamforming/`.

## Resultados
- El firmware coincide con la referencia (≤ 1 LSB) y el bus anda: sin el SD del mic R, el canal R da 0.
- Son dos mics independientes (coherencia en 5–7,5 kHz de 0,05–0,12) y su ganancia es pareja (L − R ≈ 0 dB).
- **No hay directividad:** el retardo R vs L no cambia con la posición de la fuente.
- Desde arriba, con pines originales contra intercambiados (tomas 3 y 8): **corrimiento eléctrico ≈ −0,5 a −0,8 muestras** y acústico ≈ −0,4 (fuente descentrada). Es tentativo: el montaje colgante se mueve al tocar los cables.
- Hipótesis: cada INMP441 muestrea en su mitad del período de WS, lo que da ~0,5 muestras entre slots. Sin confirmar: el datasheet no lo dice.

## Errores de la prueba
- El script avisaba "Grabando" recién al terminar la toma, y varias tomas con voz quedaron vacías. Ya está corregido en `orbita_serial.py`.
- El montaje cuelga de los cables y está pegado al libro y al parlante.

## Próximo
1. Montaje rígido: plano, a 21,4 mm, lejos de superficies.
2. Desde arriba, con sonido continuo: un par de tomas con pines originales e intercambiados, sin mover los mics.
3. Si se confirma el corrimiento, compensarlo (retardo fraccionario fijo o separación física) y repetir frente/costado.

## Para la PCB
- SD necesita 100 kΩ a GND (INMP441 Fig. 7, p. 11). Hoy no figura en `orbita_pinout.md`.
- Sensibilidad ±3 dB por mic (Tabla 1): calibración por unidad.
- L/R a GND = izquierdo (Fig. 9–10, p. 12).
