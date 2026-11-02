# Prueba 2 — beamforming propio (30/10 y 01/11)

Rama `prueba-beamforming`. Hipercardioide diferencial endfire, mics a 21,4 mm (1 muestra), `c_F − 0,5·c_B` + integrador (fc 150 Hz). Teoría: `orbita-obsidian/hipercardioide_con_cardioides.html`.

## Qué hay
- `firmware/components/orbita_beamforming/`: beamformer en C puro, con la compensación L/R.
- `firmware/main/test_bf.c`: modo menuconfig *Prueba de beamforming*. Graba 5,12 s y manda un WAV `[L, R, BF]` por UART0.
- `tools/orbita_serial.py COM_nativo COM_uart --prefix nombre_`: graba en `tools/prueba_beamforming/`.
- `tools/analizar_beamforming.py toma.wav`: firmware vs Python, niveles y retardo R vs L por banda. Tomas anteriores al 01/11: `--firmware-viejo`.

## Corrimiento L/R: −0,86 muestras, eléctrico (01/11)
El canal R llega **0,86 muestras antes** que L (~54 µs). Sigue al canal, no al mic.
- **Cómo se midió:** misma posición, toma normal (D1) y con los pines L/R de los mics intercambiados (D2). Eléctrico = (D1+D2)/2 = **−0,86 ± 0,02** en las 5 bandas (200 Hz–7 kHz); acústico = (D1−D2)/2. Confirmado aparte: con la fuente arriba (acústico ≈ 0) mide −0,86.
- **No es entero:** el "−1,00" del 30/10 era eléctrico + acústico mezclados. Causa exacta sin confirmar (toma a 32 kHz pendiente); para compensar no hace falta.
- **Ganancia:** un mic es ~1,3 dB más sensible (la diferencia sigue al mic al intercambiar). Normal para ±3 dB del datasheet.

## Compensación (en el firmware, 01/11)
- R pasa por un **FIR de Lagrange de 4 coeficientes** (retardo 1 + 0,86) y L se retrasa 1 muestra pura → alineados. `BF_CORRIMIENTO_R` en `test_bf.c`.
- Validado en Python contra un retardo ideal (FFT): mismos dB en las 4 tomas; error < 0,2 dB y < 0,03 muestras hasta 3 kHz. El C simulado coincide con la referencia (1 LSB). Latencia extra: 62,5 µs.

## Resultados con compensación (parlante a 0,5 m, 1 toma por posición)
| Toma | Posición | Ángulo medido | BF − mic L sin compensar | **con compensación** |
|---|---|---|---|---|
| `toma_bf_3` | frente | 52° | −0,3 dB | **−0,6 dB** |
| `toma_bf_4` | atrás | 143° | −0,7 dB | **−5,7 dB** |
| `toma_bf_5` | arriba | 88° | −1,5 dB | **−2,1 dB** |
| `toma_bf_6` | costado | 105° | −4,4 dB | **−10,5 dB** |

- **Primera directividad real:** frente/atrás 5 dB, costado 10 dB (el nulo del hipercardioide está a ~110°). Sin compensar, todo daba igual.
- Lejos de la teoría (~15 dB atrás) porque frente/atrás no quedaron sobre el eje (52° y 143°, no 0° y 180°).
- **Arriba no cierra:** debería rechazar como el costado (−2 vs −10 dB). Repetir; sospecha: reflejo de la mesa o los agujeros de los mics hacia arriba.
- Las tomas del 30/10 no sirven para directividad: todas quedaron entre 60° y 116°.

## Próximo
1. Flashear el modo beamforming con la compensación y repetir 2 tomas por posición, con el parlante **sobre el eje** (0° y 180°) y lejos de superficies.
2. Probar con voz y STT: crudo vs AFE vs beamformer. Es el número que decide.
3. Decidir 21,4 mm vs 50 mm: a 50 mm el corrimiento pesa 2,3 veces menos.
4. Producto: en un consultorio hablan médico y paciente desde lados distintos; un haz fijo atenúa a uno. Definir si sirve antes de seguir.

## Para la PCB
- SD del INMP441 necesita 100 kΩ a GND (Fig. 7, p. 11). Hoy no figura en `orbita_pinout.md`.
- Sensibilidad ±3 dB por mic (Tabla 1) y corrimiento L/R del bus: calibración por unidad o constante medida en la línea de producción.
- L/R a GND = izquierdo (Fig. 9–10, p. 12).
