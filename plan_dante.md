# TODO — Frente Audio: ¿con qué entrenamos?

Actualizado 2026-10-04.

## La pregunta

¿Sirve entrenar la wake word "órbita" con audio de celu (fácil de conseguir, ya tenemos limpios)? Depende de si **se parece a lo que va a recibir el modelo en la placa**. Eso se averigua grabando lo mismo con las tres fuentes y comparando. No se decide antes de medir (ni AFE sí/no, ni celu sí/no).

## El experimento

Grabar **las mismas tomas, a la vez**, con tres fuentes:

| Fuente | Qué es | Cómo se obtiene |
|---|---|---|
| **1. Celu** | Lo que ya tenemos como dataset | Celu pegado a la placa |
| **2. ESP crudo** | Lo que ve el mic de la placa sin procesar | Firmware actual (ya funciona) |
| **3. ESP + AFE** | Lo que vería el modelo si metemos el AFE | Requiere integrar el AFE (ver Fase 2) |

**Tomas** (~15 min, vos de hablante): "órbita" a 0,5 / 1 / 2 m × con y sin barbijo × ~5 tomas, aplauso al inicio de cada una para alinear. Más 1–2 min de ruido del labo para ver el piso de ruido.

**Qué mirar** (`orbita-dataset/scripts/wsp_esp_tts_comparision/compare_spectrograms.py --normalize` + espectro promedio por banda):
- Nivel y piso de ruido de cada fuente.
- Forma del espectro (¿el mic de la placa colorea la voz distinto?).
- Efecto de distancia y barbijo.
- En el AFE: ¿el NS se come consonantes de "órbita" (la r, la b) o deja artefactos? ¿Cuánto baja el ruido?

## Fases

### Fase 0 — Antes de grabar nada
- [ ] **Congelar la ganancia/shift de captura.** Cualquier cambio posterior invalida lo grabado con la ESP (el ruido de labo incluido). Hoy los picos andan en −25 a −30 dBFS.
- [ ] Verificar `overruns=0` en el log de UART0 (un hueco en el audio el modelo lo aprende como ambiente).

### Fase 1 — Celu vs ESP crudo (se puede hacer ya, sin tocar firmware)
- [ ] Grabar el set de tomas con celu + ESP crudo.
- [ ] Comparar. Si ya se ve que son muy distintas, hay pistas de qué adaptar sin esperar al AFE.

### Fase 2 — Sumar el AFE como tercera fuente
No es una decisión, es medir cómo cambia la señal.
- [ ] Compilar con `esp-sr` (dependencia ya sumada, sin uso). Verificar en la doc cómo carga los modelos NSNet/VADNet (probablemente una partición `model` en flash; **verificar, no asumir**).
- [ ] Solo `feed` + `fetch`: 2 mics, WakeNet apagado, NS y VAD activos, AGC apagado. Loguear tamaños de chunk, RAM y CPU (completa los pasos 1–2 de `orbita-obsidian/plan-presupuesto-ram-cpu.md`).
- [ ] Grabar doble: WAV de 2 canales = ch0 mic crudo + ch1 salida del AFE. Son 64 KB/s, igual que hoy. No mandar 3 canales: 96 KB/s no entran en el UART a 921600 baud.
- [ ] Repetir el set de tomas y comparar las tres fuentes.

## Cómo se decide después

| Resultado de la comparación | Qué hacer |
|---|---|
| Celu y ESP se parecen (espectro y piso de ruido cercanos) | Entrenar con celu + ruido de la placa mezclado. Listo. |
| Difieren de forma pareja (ej. curva de espectro) | Estimar el filtro celu → ESP y aplicarlo a los audios de celu. |
| Difieren en ruido o nivel | Mezclar ruido de la placa y normalizar ganancia. |
| El AFE cambia mucho la señal | Entrenar con salida del AFE (los audios de celu habría que pasarlos por el AFE) o entrenar con crudo + ruido y comparar. |
| El AFE no aporta o rompe "órbita" | Dejarlo afuera. Ahorra ~1,2 MB PSRAM y ~46% de un núcleo (solo valdría por el beamforming hacia el backend). |

Mientras no se midan, todo esto son hipótesis. Dato de la investigación: no existe un A/B publicado de ESP-AFE sobre un wake word, y la literatura sugiere que lo que decide es que entrenamiento y despliegue coincidan.

## Después: modelo v0 en Edge Impulse
- **Test fijo, nunca entra a entrenar:** "órbita" con la ESP de 3–5 personas distintas + `ruido_labo3` para falsas activaciones/hora.
- Variantes según lo que muestre la comparación (ej. celu limpio / celu + ruido / + tomas ESP reales).
- Métricas: FRR y falsas activaciones por hora, no el accuracy de Edge Impulse.
- Positivos reales con la ESP: solo si la comparación muestra que hacen falta. Más personas distintas vale más que más tomas por persona.

## Ring buffer (`firmware/main/main.c`)
El ring está bien elegido (absorbe cortes de WiFi; ping-pong no). Decisiones pendientes, después de la comparación:
- [ ] Ring en PSRAM o RAM interna (decidir al medir el AFE si se usa) y documentar en `orbita-obsidian/`.
- [ ] Política de pérdida: `xRingbufferSend(..., 0)` descarta el bloque. Aceptable para estudio, no para el backend.
- [ ] Margen del UART (921600 baud ≈ 1,44× el audio): solo importa en captura de estudio.

## Más adelante (no tocar hasta tener v0)
Common Voice + alineación forzada, 50 personas por WhatsApp, MS-SNSD, proporciones finales, pipeline de inyección, `prep_for_edge_impulse.py` actualizado. Todo en `orbita-dataset/dataset.md`.
