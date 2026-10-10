# TODO — Frente Audio: flujo de audio primero, AFE después

Actualizado 2026-10-08.

## Cambio de prioridad (2026-10-08)

Antes: el plan giraba en torno al experimento "¿con qué entrenamos?" (celu vs ESP crudo vs AFE).
Ahora: **primero un flujo de audio confiable captura → ring → consumidor**, después integrar y medir el AFE, y dejar todo listo para enchufar los sockets cuando el compañero los tenga. El entrenamiento de la wake word tarda de todos modos, así que corre en paralelo y no bloquea nada.

Orden: **A (flujo perfecto) → B (AFE integrado y medido) → C (listo para sockets)**. A+B = H1 (experimento), A+B+C = H2 (mergeable). El experimento de las tres fuentes pasa a ser un entregable de B, no el eje del plan.

## Dónde quedamos (2026-10-08) — retomar acá

**Estado:** VS Code listo (F12 anda). No se tocó código del firmware desde el reviewer (2026-10-06). Próximo paso: **A0 y A1**.

**Antes de pushear:** avisar al compañero. Al hacer pull se le borra su `.vscode/settings.json` (ahora es `settings.example.json`); que lo copie antes. Sin commitear: `plan_dante.md`, `main.c.old`, cambios del vault.

**A0 hecho (2026-10-10), falta probar en el chip:** al flashear, el log de arranque tiene que decir `SPI Mode : QIO`; si no arranca, volver la flash a DIO (ver `arquitectura.md`, sección 1). Hay que borrar el `sdkconfig` local y reconfigurar al hacer pull.

**Por verificar (no confirmado):** chunk del AFE = 512 muestras/canal (del reviewer, de memoria); ICS-43434 a 16 kHz (datasheet).

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

- [x] **A0. `sdkconfig.defaults` del repo — hecho 2026-10-10** (build limpio verificado; decisiones y consecuencias en `arquitectura.md`; el stack de las tareas nuevas se revisa en A2; el `sdkconfig` local hay que borrarlo para que tome los cambios) (antes: hoy el build usa defaults que no sirven y `sdkconfig` está ignorado por git, así que el compañero no los tendría):
  - Flash **16 MB** (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`; hoy dice 2 MB). Con 2 MB no entran app + modelos de `esp-sr`.
  - Tabla de particiones **custom** (`partitions.csv`) con partición `model` para los modelos del AFE; hoy está `SINGLE_APP` y el archivo custom no existe. Tamaño de la partición: verificar en la doc de `esp-sr`.
  - CPU a **240 MHz** (hoy 160) y `CONFIG_FREERTOS_HZ=1000` (hoy 100: ticks de 10 ms).
  - Stack de `app_main` y de cada tarea nueva: revisar (hoy 3584 bytes en `app_main`).
  - Cambiar esto invalida mediciones previas de CPU; hacerlo antes de medir el AFE.
- [ ] **A1. Contador de overflows del DMA.** *(código escrito y compilado 2026-10-10; falta la prueba con `vTaskDelay(200)` en el labo)* Hoy `overruns` solo cuenta ring lleno: el DMA puede perder muestras sin que nadie lo sepa, así que `overruns=0` **no garantiza** audio sin huecos.
  - Firma (`i2s_types.h:139`): `bool cb(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)`, retorna `false` si no despertó ninguna tarea.
  - Registro (`i2s_common.h:241`): `i2s_channel_register_event_callback(handle, &cbs, NULL)` con `cbs.on_recv_q_ovf = cb`. Falla con `ESP_ERR_INVALID_STATE` si el canal ya está habilitado: **registrar antes de `i2s_channel_enable`**.
  - Callback `static IRAM_ATTR`, contador `static volatile uint32_t` en `audio_capture.c`, getter público `orbita_audio_get_dma_overflows()` en `audio_capture.h`. Reiniciar a 0 en cada grabación e imprimirlo junto a `overruns` en la línea "Fin: ...".
  - `event->size` = bytes sobrescritos: opcional sumarlo para saber cuánto audio se perdió.
  - Prueba: `vTaskDelay(200)` en el loop de captura; el contador tiene que subir.
- [ ] **A2. Captura en tarea propia** *(código escrito y compilado 2026-10-10, core 1, prioridad 6; falta probar en el labo: 60 s con `overruns=0` y `dma_overflows=0`)* (hoy corre en `app_main`, prioridad 1), con prioridad mayor que las del AFE y fijada a un core.
- [x] **A3. Estructura en componente — hecho 2026-10-10** *(solo se movieron `audio_capture`, `wav_writer` y la dependencia `esp-sr`; `tarea_captura`, constantes y ring siguen en `main.c` hasta A5–A8; build idéntico, 0x3b7c0)*: mover `audio_capture`, `wav_writer` y lo que se sume a `firmware/components/orbita_audio/` (con su `CMakeLists.txt`); `main/` queda fino. Mover ahí también la dependencia `esp-sr` del `idf_component.yml`. Es barato ahora y caro cuando el compañero ya tenga código en `main.c`.
- [ ] **A4. Tamaño de bloque = chunk del AFE** (`get_feed_chunksize()`, a verificar; el reviewer estima 512 muestras/canal a 16 kHz). Hoy 1600 no es múltiplo. Un solo parámetro que B ajusta a lo que devuelva el AFE en runtime, sin rehacer el ring.
- [ ] **A5. Ring** (decisiones de producto en `orbita-obsidian/arquitectura.md`; Claude sugiere dos rings, Dante investiga cómo se suele hacer antes de cerrar): **primero definir las políticas** (reintentos de la FSM, pre-roll, qué pasa al llenarse) y recién ahí el tipo: el ring de FreeRTOS es de consumo único y no permite releer para reintentar. En PSRAM (`xRingbufferCreateWithCaps` existe en 5.5.5, pero el struct de control también cae en PSRAM; alternativa `xRingbufferCreateStatic`). Antes de armar ring 1, ver si `feed()` bloquea y qué hace `afe_ringbuf_size`. Hoy el ring es `BYTEBUF` y su memoria la decide `malloc` (con `SPIRAM_USE_MALLOC` y umbral de 16 KB probablemente ya cae en PSRAM: confirmarlo).
  - **Dimensionar para sockets:** cuántos segundos de corte de WiFi tiene que absorber (a 64 KB/s, 10 s = 640 KB; si el ring largo es mono procesado, la mitad) y qué pasa cuando se llena.
- [ ] **A6. Contrato del chunk (lo que ve cualquier consumidor).** Cada ítem del ring lleva un **encabezado chico**: número de secuencia, cantidad de canales/muestras y flag de discontinuidad. Con eso (a) el socket puede mandarlo tal cual y el backend detecta huecos, (b) el WAV de estudio sabe dónde se perdió audio, (c) no hay que rediseñar al sumar sockets. Definirlo ahora; es lo que el compañero más va a necesitar.
- [ ] **A7. Interfaz del consumidor ("sink")** con operaciones mínimas: abrir/empezar, recibir chunk, cerrar. Implementaciones: `uart_sink` (estudio, lo que hoy hace `tarea_envio`) y, para probar sin red, `throttled_sink` (consume a ritmo configurable y se puede "cortar" N segundos). El socket del compañero será una tercera implementación.
- [ ] **A8. Interfaz de control** (arrancar/parar el flujo): hoy lo dispara el comando `'g'`; mañana lo dispara la FSM (`dev_wake_word`, `dev_f_stop_send`). `main.c` solo traduce comandos a esas llamadas. Sin wake word entrenada todavía, el disparo manual (o el VAD del AFE) hace de stub.
- [ ] **A9.** `CONFIG_I2S_ISR_IRAM_SAFE` y más descriptores DMA (`dma_desc_num` 8–12).
- [ ] **A10. Manejo de errores** de `audio_capture` (deinit, guarda de `s_rx_chan == NULL`, timeout finito en el read).
- [ ] **A11.** Verificar en el datasheet del ICS-43434 que soporte 16 kHz (el reviewer duda). Barato ahora, caro si el mic de producción no lo cubre.
- [ ] **A12. Pruebas de validación:**
  - Captura larga (10 min, en el labo): línea "Fin: ..." con ambos contadores en 0 y WAV sin saltos (un tono conocido ayuda a ver huecos).
  - **Corte simulado con `throttled_sink`:** parar el consumidor 5, 10 y 20 s. Verificar que el ring absorbe lo prometido en A5 y que, al llenarse, la discontinuidad llega marcada al consumidor y los contadores lo reflejan. Valida el tamaño del ring sin necesitar sockets.
- [ ] **A13. Menores:** borrar `wav_pack_block_24bit` (muerto), comentarios desactualizados, `FRAME_SIZE` → `CHANNELS`, capacidad de `out_buf` en `wav_pack_block_16bit`.

**No tocar la ganancia/shift de captura** una vez que se graba: los picos andan en −25 a −30 dBFS (margen cómodo contra clipping) y cambiarlo invalida lo grabado con la ESP.

## Fase B — AFE integrado y medido (cierra H1)

Se integra **como medición**: el resultado confirma o descarta, no se da por decidido.

- [ ] Verificar en la doc/ejemplos de `esp-sr` cómo carga NSNet/VADNet (probablemente partición `model`, ver A0; **verificar, no asumir**) y cómo se flashean los modelos.
- [ ] **Prueba de filtros: WebRTC vs. redes neuronales (decisión pendiente).** Hoy el `sdkconfig` trae NS y VAD de WebRTC (default de `esp-sr`, clásico, liviano). El plan original asumía NSNet/VADNet (redes neuronales, más CPU, usan la partición `model`). Se prueban **los dos modos** con las mismas tomas y se decide con datos:
  - Modo 1: `SR_NSN_WEBRTC` + `SR_VADN_WEBRTC` (partición `model` vacía).
  - Modo 2: `SR_NSN_NSNET2` o `NSNET3` + `SR_VADN_VADNET1_MEDIUM` (cuál NSNet: NSNet3 pesa 113 KB, NSNet2 334 KB; probar el que entre en CPU).
  - Medir en cada uno: CPU (% de núcleo), RAM interna y PSRAM, latencia del `fetch`, `overruns`/`dma_overflows` en 0, y calidad de la salida (SNR y si se come consonantes como r, b).
  - Cambiar de modo es cambiar el `sdkconfig` y recompilar: dejar anotado en el informe qué modo se usó en cada toma.
- [ ] Levantar el AFE: 2 mics, WakeNet apagado, NS y VAD activos, AGC apagado. Solo `feed` + `fetch`, en tareas propias (fetch tiene que correr tan rápido como feed o el AFE se atrasa). Ajustar el bloque de A4 al chunk real.
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

- [ ] Mostrarle al compañero A6 (chunk) y A7 (sink) y acordar qué pide el transporte: reconexión, qué cierra el estado de envío, wake word a mitad de un envío (contrato = FSM del vault: `dev_wake_word`, `dev_f_stop_send`, etc.). Confirmar que le sirve el número de secuencia.
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
