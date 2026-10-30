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
- **Corrimiento eléctrico de 1 muestra entera (R adelantado).** Re-medido con pendiente de fase y GCC-PHAT: 4 tomas dan −1,00 ± 0,05, plano de 200 Hz a 7 kHz (retardo digital, no acústico). Las tomas 6–8 (−0,4 a −1,3) suman una parte acústica de ±0,5. El script viejo medía en 4 frecuencias sueltas y no lo separaba.
- **No hay directividad** en frente contra costado (`toma_bf_1` −0,96, `toma_bf_2` −1,01): la parte acústica esperada (~1 muestra) no aparece en esas dos tomas. Causa sin confirmar (montaje o posición de la fuente).
- Descartado como causa principal: que cada mic muestree en su mitad de WS (daría 0,5). Detalle en `reports/Corrimiento L R en INMP441.md`.

## Errores de la prueba
- El script avisaba "Grabando" recién al terminar la toma, y varias tomas con voz quedaron vacías. Ya está corregido en `orbita_serial.py`.
- El montaje cuelga de los cables y está pegado al libro y al parlante.

## Próximo
1. Montaje rígido: plano, a 21,4 mm, lejos de superficies.
2. Desde arriba, con sonido continuo: un par de tomas con pines originales e intercambiados, sin mover los mics.
3. Toma a 32 kHz: 1 muestra = corrimiento de frame, 2 muestras = de tiempo.
4. Compensar retrasando R una muestra entera y repetir frente/costado.

## Para la PCB
- SD necesita 100 kΩ a GND (INMP441 Fig. 7, p. 11). Hoy no figura en `orbita_pinout.md`.
- Sensibilidad ±3 dB por mic (Tabla 1): calibración por unidad.
- L/R a GND = izquierdo (Fig. 9–10, p. 12).
