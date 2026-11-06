# Prueba 3 — división en bandas (06/11)

Rama `medicion-bandas` (sale de `tests+afe`, con el `beamforming.c` de `main`). Hipercardioide debajo de 4 kHz, mic de adelante arriba (crossover Linkwitz-Riley 4.º orden). Diseño y simulación: `beamforming.h` en `main`.

**Qué se quiere ver:** que la voz de frente quede plana hasta 7 kHz y que la directividad debajo de 3 kHz no cambie. Se repiten las 4 posiciones de la prueba 2 (01/11).

## Preparación
- Igual que el 01/11: parlante a 0,5 m, ruido continuo a volumen fijo, mic de adelante en el slot L. Esta vez el frente **sobre el eje** (el 01/11 quedó a 76°).
- El parlante tiene que dar agudos hasta 7 kHz: uno de notebook o de celular puede no darlos.

## Tomas (2 por posición)
| Prefijo | Posición |
|---|---|
| `bandas_frente_` | 0°, parlante en la línea de los mics, del lado del mic L |
| `bandas_arriba_` | encima de la placa |
| `bandas_costado_` | 90°, perpendicular a la línea de los mics |
| `bandas_atras_` | 180°, en la línea, del lado del mic R |

## Referencia: tomas del 01/11 recalculadas con bandas en la PC
`BF − mic L` en dB, sin bandas → con bandas:

| Toma | 0,3–1k | 1–2k | 2–3k | 3–4k | 4–5k | 5–7k |
|---|---|---|---|---|---|---|
| frente 76° | −0,7 → −0,7 | −2,6 → −2,6 | −4,5 → −4,6 | −5,1 → −4,2 | −7,2 → −2,5 | −9,7 → −0,6 |
| arriba 97° | −7,0 → −7,0 | −8,4 → −8,4 | −8,0 → −7,7 | −9,8 → −7,5 | −8,7 → −2,5 | −11,9 → −0,5 |
| costado 119° | −15,2 → −15,2 | −8,5 → −8,6 | −7,9 → −7,7 | −9,5 → −6,1 | −13,2 → −2,6 | −13,6 → −0,8 |
| atrás 154° | −2,9 → −2,9 | −5,9 → −6,0 | −5,5 → −6,0 | −7,2 → −5,7 | −9,6 → −2,2 | −13,2 → −0,4 |

## Qué tiene que dar
- **Firmware vs referencia:** ≤ 2 LSB.
- **Frente sobre el eje, "con bandas":** todas las bandas entre 0 y −2,5 dB (simulación: −0,2 / −0,2 / −0,9 / −1,8 / −1,6 / −0,4).
- **Todas las posiciones, ≤ 3 kHz:** "con bandas" a menos de ~1 dB de "sin bandas" (la directividad no se tocó). Arriba de 5 kHz ~0 dB: esperado, ahí no hay directividad.
- **Frente con un pozo de más de 3 dB en 3–5 kHz:** el mic de adelante no es el que se cree (`BF_ADELANTE_ES_L`) o cambió el corrimiento (tabla de retardo del script). Anotar y no tocar.

## Resultados
(completar)
