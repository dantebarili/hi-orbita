# TODO — Frente Audio: flujo de audio primero, AFE después

Actualizado 2026-10-10.

## Prioridad (2026-10-08)

Primero un flujo de audio confiable captura → ring → consumidor, después integrar y medir el AFE, y dejar todo listo para enchufar los sockets. El entrenamiento de la wake word tarda de todos modos, así que corre en paralelo y no bloquea nada.

Orden: **A (flujo perfecto) → B (AFE integrado y medido) → C (listo para sockets)**. A+B = H1 (experimento), A+B+C = H2 (mergeable). El experimento de las tres fuentes es un entregable de B.

**Orden de ejecución (2026-10-10):** A0–A4 y pruebas del labo → Fase B (AFE + grabador de estudio, con el ring simple actual) → recién ahí A5–A8 (ring de slots, header, sink, control), con el chunk del AFE ya medido. H1 desbloquea el entrenamiento de la wake word; A5–A8 solo hacen falta para los sockets (H2).

## Hecho (2026-10-10)

Todo compila; nada se probó todavía en el chip.

- **A0** `sdkconfig.defaults` + `partitions.csv`: flash 16 MB en QIO a 80 MHz, CPU 240 MHz, tick 1 ms, stack de `app_main` 8 KB, particiones con 2 slots OTA, `model` y `clips` (`arquitectura.md` §1).
- **A1** Contador de overflows del DMA (`orbita_audio_get_dma_overflows()`), impreso en la línea "Fin:".
- **A2** Captura en tarea propia (core 1, prioridad 6), controlada con `grabando` y `frames_objetivo`.
- **A3** `audio_capture` y `wav_writer` movidos a `firmware/components/orbita_audio/` (la tarea de captura y el ring siguen en `main.c`).
- Constantes de `main.c` agrupadas, con `_Static_assert` de las relaciones entre tamaños.
- **A5/A6 (solo diseño):** políticas del ring, dos rings, header de 16 B, tabla de segmentos, pipelining híbrido (`arquitectura.md` §3.4–3.6).
- Vocabulario fijo (chunk / mensaje / segmento) y FSM renombrada en todos los archivos: `dev_fin_segmento`, `srv_fin_conversacion`, `srv_sigue_conversacion`.

## Dónde quedamos — retomar acá

**Probar en el labo:**
- Log de arranque con `SPI Mode : QIO` (si no arranca, volver la flash a DIO).
- Captura de 60 s con `overruns=0` y `dma_overflows=0`.
- Forzar la falla (`vTaskDelay(200)` en `tarea_captura`): el contador tiene que subir.

**Antes de pushear:** avisarle al compañero. Tiene que copiar su `.vscode/settings.json` (ahora es `settings.example.json`), borrar su `sdkconfig` y reconfigurar, y saber que la tabla de particiones nueva borra el `nvs` del DevKit y que cambiaron los nombres de eventos de la FSM.

**Siguiente:** pruebas del labo, A4 y Fase B. A5 v1 queda para después (con tiempo acotado). Las decisiones de `arquitectura.md` §3.6 se hablan con el compañero.

**Por verificar (no confirmado):** chunk del AFE = 512 muestras/canal (del reviewer, de memoria); ICS-43434 a 16 kHz (A11).

## Dos hitos

| Hito | Qué significa | Fases |
|---|---|---|
| **H1 — Listo para el experimento** | Se puede grabar la sesión de las tres fuentes: crudo + AFE en un mismo WAV, sin huecos, con duración variable. | A + B |
| **H2 — Mergeable y listo para sockets** | El audio vive aislado en su propio componente, entrega chunks numerados con discontinuidades marcadas, y el transporte se enchufa sin tocar captura ni ring. | A + B + C |

**"Mergeable" =** (todo se cumple)
- Compila limpio desde cero con el `sdkconfig.defaults` del repo (no depende de tu `sdkconfig` local, que está en `.gitignore`).
- El audio vive en `firmware/components/orbita_audio/`; `main.c` queda como integrador fino (inicia módulos, atiende comandos). El compañero agrega su componente de comunicación sin tocar `main.c` más que para enchufar el sink.
- Ningún parámetro del estudio (10 min, `'g'`, baud, markers) vive dentro de captura/ring/AFE.
- Pasaron `embedded-reviewer` y `fsm-consistency-reviewer`; `arquitectura.md` actualizado.

## Fase A — Flujo de audio perfecto (PRIORIDAD)

**Criterio de "terminado":**
- `overruns=0` **y** `dma_overflows=0` en toda captura, sin importar la duración. Los 10 min de A12 son solo la prueba de validación (la que hacemos en el labo), no el requisito.
- El contador de overflows se probó forzando la falla (si no sube cuando debe, no sirve).
- Ante un hueco, el consumidor se entera (discontinuidad marcada), no se rellena en silencio.
- El consumidor es un módulo intercambiable (hoy UART, mañana socket) sin tocar captura ni ring.

Pasos (fixes del `embedded-reviewer` del 2026-10-06 más lo que salió de revisar el código el 2026-10-08):

- [ ] **A4. Chunk de captura = chunk del AFE** (`get_feed_chunksize()`, a verificar; el reviewer estima 512 muestras/canal a 16 kHz). Hoy 1600 no es múltiplo. Un solo parámetro que B ajusta a lo que devuelva el AFE en runtime, sin rehacer el ring.
- [ ] **A5. Ring de slots** (diseño en `arquitectura.md` §3.4). v1: slots en PSRAM + 3 cursores (escritura / envío / confirmación) + cola de segmentos consecutivos, sin urgente ni pre-roll (no hay wake word aún); probar con `uart_sink` y `throttled_sink`. v2: urgente y pre-roll. Antes de armar el ring 1, verificar si `feed()` bloquea.
- [ ] **A6. Header de 16 B + tabla de segmentos** (diseño en §3.4): codearlo junto con A5 y revisarlo con el compañero.
- [ ] **A7. Interfaz del consumidor ("sink")** con operaciones mínimas: abrir/empezar, recibir chunk, cerrar. Implementaciones: `uart_sink` (estudio, lo que hoy hace `tarea_envio`) y, para probar sin red, `throttled_sink` (consume a ritmo configurable y se puede "cortar" N segundos). El socket del compañero será una tercera implementación.
- [ ] **A8. Interfaz de control** (arrancar/parar el flujo): hoy lo dispara el comando `'g'`; mañana lo dispara la FSM (`dev_wake_word`, `dev_fin_segmento`). `main.c` solo traduce comandos a esas llamadas. Sin wake word entrenada todavía, el disparo manual (o el VAD del AFE) hace de stub.
- [ ] **A9.** `CONFIG_I2S_ISR_IRAM_SAFE` y más descriptores DMA (`dma_desc_num` 8–12).
- [ ] **A10. Manejo de errores** de `audio_capture` (deinit, guarda de `s_rx_chan == NULL`, timeout finito en el read).
- [ ] **A11.** Verificar en el datasheet del ICS-43434 que soporte 16 kHz (el reviewer duda). Barato ahora, caro si el mic de producción no lo cubre.
- [ ] **A12. Pruebas de validación:**
  - Captura larga (10 min, en el labo): línea "Fin: ..." con ambos contadores en 0 y WAV sin saltos (un tono conocido ayuda a ver huecos).
  - **Corte simulado con `throttled_sink`:** parar el consumidor 5, 10 y 20 s. Verificar que el ring absorbe lo prometido en A5 y que, al llenarse, la discontinuidad llega marcada al consumidor y los contadores lo reflejan. Valida el tamaño del ring sin necesitar sockets.
- [ ] **A13. Menores:** borrar `wav_pack_chunk_24bit` (muerto), comentarios desactualizados, capacidad de `out_buf` en `wav_pack_chunk_16bit`.

**No tocar la ganancia/shift de captura** una vez que se graba: los picos andan en −25 a −30 dBFS (margen cómodo contra clipping) y cambiarlo invalida lo grabado con la ESP.

## Fase B — AFE integrado y medido (cierra H1)

Se integra **como medición**: el resultado confirma o descarta, no se da por decidido.

- [ ] Verificar en la doc/ejemplos de `esp-sr` cómo carga NSNet/VADNet (probablemente partición `model`, ver A0; **verificar, no asumir**) y cómo se flashean los modelos.
- [ ] **Prueba de filtros: WebRTC vs. redes neuronales (decisión pendiente).** Hoy el `sdkconfig` trae NS y VAD de WebRTC (default de `esp-sr`, clásico, liviano). El plan original asumía NSNet/VADNet (redes neuronales, más CPU, usan la partición `model`). Se prueban **los dos modos** con las mismas tomas y se decide con datos:
  - Modo 1: `SR_NSN_WEBRTC` + `SR_VADN_WEBRTC` (partición `model` vacía).
  - Modo 2: `SR_NSN_NSNET2` o `NSNET3` + `SR_VADN_VADNET1_MEDIUM` (cuál NSNet: NSNet3 pesa 113 KB, NSNet2 334 KB; probar el que entre en CPU).
  - Medir en cada uno: CPU (% de núcleo), RAM interna y PSRAM, latencia del `fetch`, `overruns`/`dma_overflows` en 0, y calidad de la salida (SNR y si se come consonantes como r, b).
  - Cambiar de modo es cambiar el `sdkconfig` y recompilar: dejar anotado en el informe qué modo se usó en cada toma.
- [ ] Levantar el AFE: 2 mics, WakeNet apagado, NS y VAD activos, AGC apagado. Solo `feed` + `fetch`, en tareas propias (fetch tiene que correr tan rápido como feed o el AFE se atrasa). Ajustar el chunk de A4 al valor real.
- [ ] **Crear el AFE al arrancar, antes del `'g'`.** `esp-sr` imprime config al crearse y, si algo escribe por `printf` durante el streaming por UART0, corrompe el audio (apagar `esp_log` no frena `printf`). Verificar en una captura que el WAV no tiene texto adentro.
- [ ] Loguear tamaños de chunk, RAM y CPU (completa los pasos 1–2 de `orbita-obsidian/plan-presupuesto-ram-cpu.md`). Documentar en `orbita-obsidian/`.
- [ ] Confirmar que con el AFE andando siguen `overruns=0` y `dma_overflows=0` (el AFE compite por CPU; es la prueba real de A2).
- [ ] **Grabador de estudio** (sirve para la sesión de tomas):
  - **Duración variable**: hoy `STREAM_SECONDS` es fija (600 s) y la sesión son decenas de tomas cortas. Opción: arrancar/parar por comando; el header del WAV se arregla en la PC al terminar.
  - WAV de 2 canales = ch0 mic crudo + ch1 salida del AFE (64 KB/s, igual que hoy). No mandar 3 canales: 96 KB/s no entran en el UART a 921600 baud.
  - **Alineación:** la salida del AFE llega con latencia respecto al crudo. Primero probar `raw_data` de `afe_fetch_result_t` (viene junto a la salida procesada; verificar que esté alineado). Si no, emparejar por número de secuencia (A6) y medir la latencia con el aplauso; si no, ch0 y ch1 quedan desfasados dentro del mismo WAV.
  - Anotar qué mic va en ch0 (L o R): el crudo es uno solo.

## Fase C — Listo para sockets (cierra H2)

Objetivo: cuando el compañero tenga el transporte, sumarlo sea implementar un sink, no rediseñar.

- [ ] Mostrarle al compañero A6 (chunk) y A7 (sink) y acordar qué pide el transporte: reconexión, qué cierra el estado de envío, wake word a mitad de un envío (contrato = FSM del vault: `dev_wake_word`, `dev_fin_segmento`, etc.). Confirmar que le sirve el número de secuencia.
- [ ] Revisar con `fsm-check` / `fsm-consistency-reviewer` que el flujo no contradice `orbita-obsidian/órbita..md`; actualizar el vault y `arquitectura.md` si cambia algo (política de pérdida, tamaño de ring).
- [ ] Decidir la política de captura abierta (continuo vs. solo tras wake word) y mono vs. estéreo hacia el backend, porque condicionan qué se encola en el ring.
- [ ] Pasar el checklist de "mergeable" (arriba) y correr `embedded-reviewer` sobre todo `orbita_audio`.
- [ ] Margen del UART (921600 baud ≈ 1,44× el audio): solo importa en captura de estudio; no es requisito del socket.

## En paralelo (no bloquea A/B/C): wake word "órbita"

El entrenamiento tarda, así que se avanza con lo que ya hay (dataset de celu) sin esperar el firmware.

**La pregunta que sigue abierta:** ¿sirve entrenar con audio de celu? Depende de si se parece a lo que recibe el modelo en la placa. Se averigua grabando **las mismas tomas, a la vez**, con tres fuentes (el hablante, el ruido y el montaje no se repiten idénticos entre sesiones):

| Fuente | Qué es | Cómo se obtiene |
|---|---|---|
| **1. Celu** | Lo que ya tenemos como dataset | Celu pegado a la placa |
| **2. ESP crudo** | Lo que ve el mic de la placa sin procesar | ch0 del WAV de la ESP |
| **3. ESP + AFE** | Lo que vería el modelo si metemos el AFE | ch1 del WAV de la ESP (salida del AFE) |

La sesión necesita Fase A completa (audio sin huecos) y Fase B (para la fuente 3). Hasta entonces, no se decide ni AFE sí/no ni celu sí/no.

**Tomas** (~15 min): "órbita" a 0,5 / 1 / 2 m × con y sin barbijo × ~5 tomas, aplauso al inicio de cada una para alinear (el AFE tiene latencia), más 1–2 min de ruido de labo. Anotar el montaje.

**Qué mirar** (`orbita-dataset/scripts/wsp_esp_tts_comparision/compare_spectrograms.py --normalize` + espectro promedio por banda): nivel y piso de ruido (clave el SNR, no el nivel absoluto), forma del espectro, efecto de distancia y barbijo, y en el AFE si el NS se come consonantes (r, b) o deja artefactos.

| Resultado de la comparación | Qué hacer |
|---|---|
| Celu y ESP se parecen | Entrenar con celu + ruido de la placa mezclado. |
| Difieren de forma pareja (curva de espectro) | Estimar el filtro celu → ESP y aplicarlo a los audios de celu. |
| Difieren en ruido o nivel | Mezclar ruido de la placa y normalizar ganancia. |
| El AFE cambia mucho la señal | Entrenar con salida del AFE (pasar el celu por el AFE) o con crudo + ruido, y comparar. |
| El AFE no aporta o rompe "órbita" | Dejarlo afuera. Ahorra ~1,2 MB PSRAM y ~46% de un núcleo (solo valdría por el beamforming hacia el backend). |

Dato de la investigación: no hay un A/B publicado de ESP-AFE sobre un wake word; lo que decide es que entrenamiento y despliegue coincidan.

**Modelo v0 en Edge Impulse**
- **Test fijo, nunca entra a entrenar:** "órbita" con la ESP de 3–5 personas distintas + `ruido_labo3` para falsas activaciones/hora.
- Variantes según la comparación (celu limpio / celu + ruido / + tomas ESP reales).
- Métricas: FRR y falsas activaciones por hora, no el accuracy de Edge Impulse.
- Positivos reales con la ESP solo si la comparación muestra que hacen falta. Más personas distintas vale más que más tomas por persona.

**Más adelante (no tocar hasta tener v0):** Common Voice + alineación forzada, 50 personas por WhatsApp, MS-SNSD, proporciones finales, pipeline de inyección, `prep_for_edge_impulse.py` actualizado. Todo en `orbita-dataset/dataset.md`.
