# Prueba 2 — beamforming propio (30/10 y 01/11)

Rama `prueba-beamforming`. Hipercardioide diferencial endfire, mics a 21,4 mm (1 muestra), `c_F − 0,5·c_B` + integrador (fc 150 Hz). Teoría: `orbita-obsidian/Beamforming/teoria_hipercardioide.html`; resumen con las mediciones: `resumen_beamforming.html` (misma carpeta).

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

## Resultados en placa (BF compensado en el firmware, parlante a 0,5 m, 1 toma por posición)
`BF − mic L` en 300–3000 Hz: 0 dB = igual que un mic solo. "Sin compensar" = mismas tomas, beamformer sin alinear calculado en la PC.

| Toma | Posición | Ángulo medido | Sin compensar | **Compensado (placa)** |
|---|---|---|---|---|
| `toma_bf_9` | frente | 76° | −3,0 dB | **−0,7 dB** |
| `toma_bf_11` | arriba | 97° | −3,8 dB | **−7,1 dB** |
| `toma_bf_8` | costado | 119° | −3,8 dB | **−15,1 dB** |
| `toma_bf_10` | atrás | 154° | +1,7 dB | **−3,0 dB** |

- **Directividad real:** frente/costado 14 dB, con el nulo cerca de 120° como predice el hipercardioide. Atrás −3 dB: lóbulo trasero (teórico −6). Para más rechazo atrás: `BF_BETA` ~0,27–0,3 (supercardioide), a costa del costado.
- Firmware vs Python: 1 LSB en 3 tomas; `toma_bf_11` difiere en 223 muestras (0,3 %), probable error de transmisión de esa toma. `toma_bf_7` era de prueba (descartada).
- El frente quedó a 76°, no a 0°: con el parlante sobre el eje los rechazos serían algo mayores.
- Las tomas del 30/10 y la tarde del 01/11 (`toma_bf_3`–`6`) dieron lo mismo compensadas en la PC.

## Próximo
1. Repetir 2–3 tomas por posición con el parlante **sobre el eje** (0° y 180°) y lejos de superficies; con eso decidir `beta`.
2. Probar con voz y STT: crudo vs AFE vs beamformer. Es el número que decide.
3. Decidir 21,4 mm vs 50 mm: a 50 mm el corrimiento pesa 2,3 veces menos.
4. Producto: en un consultorio hablan médico y paciente desde lados distintos; un haz fijo atenúa a uno. Definir si sirve antes de seguir.

## Para la PCB
- SD del INMP441 necesita 100 kΩ a GND (Fig. 7, p. 11). Hoy no figura en `pines.md`.
- Sensibilidad ±3 dB por mic (Tabla 1) y corrimiento L/R del bus: calibración por unidad o constante medida en la línea de producción.
- L/R a GND = izquierdo (Fig. 9–10, p. 12).
