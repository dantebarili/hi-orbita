# Reel A1 — "54 microsegundos"

**Duración:** ~40 s · **Meta:** compartidos y guardados · **Formato:** fail → arreglo, con antes/después que se oye.
Fuente de los números: `orbita-obsidian/Beamforming/resumen_beamforming.html` (medición del 01/11).

## Hooks (probar 2-3)
1. "Dos micrófonos desfasados 54 millonésimas de segundo me arruinaron todo." ← elegido
2. "Quería que mi placa escuche solo para adelante. Escuchaba más de atrás."
3. "Este número chiquito rompía todo mi filtro."
4. "Restás dos micrófonos y el ruido desaparece. En teoría."

## Guion

| Tiempo | Dicho | Texto en pantalla | Imagen |
|---|---|---|---|
| 0-3 s | "Dos micrófonos desfasados 54 millonésimas de segundo me arruinaron todo." | 54 µs | Zoom a los dos mics de la placa; el número aparece entre ellos |
| 3-9 s | "La idea: dos micrófonos a 2 centímetros. Si restás uno del otro, lo que viene de costado se cancela." | RESTAR = ENFOCAR | Animación del patrón (gráfico del β en `teoria_hipercardioide.html`) |
| 9-15 s | "Lo probé con un parlante tirando ruido desde cuatro lados. Desde atrás entraba más que de frente." | ATRÁS > FRENTE 🤨 | Vos moviendo el parlante alrededor de la placa; tabla "sin compensar" |
| 15-22 s | "El culpable: un micrófono llegaba 0,86 muestras antes que el otro. Para una resta, eso lo arruina todo." | 0,86 MUESTRAS | Las dos ondas L y R superpuestas, corridas; después alineadas |
| 22-31 s | "Le metí un filtro de cuatro números que atrasa un canal justo eso. Escuchá: antes… y después." | ANTES / DESPUÉS | Audio: ruido de costado, mic solo vs. beamformer. Gráfico polar sin y con compensar |
| 31-35 s | "De costado ahora entra 15 decibeles menos." | −15 dB | Número grande sobre el polar compensado |
| 35-40 s | "Pero lo medí en una sola placa. Si cambia en la próxima, hay que calibrar cada aparato." | ¿CADA PLACA? | Vos a cámara con la placa, aro LED prendido |

~110 palabras habladas: leer en voz alta y recortar a ~90.

## CTA (uno solo, según qué haya para mostrar después)
- Con 2-3 placas disponibles: "En el próximo lo pruebo en tres placas más."
- Si no: "Seguinos: lo que viene es enseñarle a la placa la palabra 'Órbita'." (lleva a B1)

## Antes de filmar
- [ ] Re-grabar las 4 posiciones (frente, arriba, costado, atrás) con y sin compensación (`ORBITA_BF_CORRIMIENTO_R` en 0 para el "antes") y regenerar la tabla con `tools/analizar_beamforming.py` (rama `tests+afe`). Si los números cambian, se usan los nuevos.
- [ ] Exportar el audio A/B (canal L vs. BF, posición costado) para el tramo 22-31 s.
- [ ] "54 µs" = 0,86 muestras / 16 kHz; si la medición nueva da otro corrimiento, recalcularlo.
- [ ] No se ven claves, `cloud_config.py` ni rutas personales en pantalla.

## Caption
> Dos micrófonos, una resta y un desfase de 54 microsegundos que no dejaba enfocar. Así lo encontramos y lo corregimos en un ESP32-S3. Estudiantes de electrónica construyendo un asistente de voz para consultorios. #esp32 #dsp #beamforming #electronica #buildinpublic

**Portada:** la placa con "54 µs" grande.
