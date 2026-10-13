# TODO — Frente Audio

Actualizado 2026-10-13.

Orden: **A (flujo de audio confiable) → B (AFE integrado y medido) → C (listo para sockets)**. A+B = H1 (experimento de las tres fuentes), A+B+C = H2 (mergeable, transporte enchufable). El entrenamiento de la wake word corre en paralelo y no bloquea. A5–A8 solo hacen falta para los sockets (H2).

## Objetivo: prototipo 1 = audio mono continuo por sockets (en pocos días)

Sin wake word ni AFE: el equipo manda **todo el tiempo**. Hecho (compila; probado sin AFE en el chip el 13/10): pipeline en componente (A14), ring out con su productor y tarea sink con `orbita_sink_t` (A5, A7 en parte). Falta, en orden: (1) medir tiempos y probar una toma con ring out + sink de lab (A15); (2) `uart_sink` y `throttled_sink` (A7) para comparar contra el WAV viejo; (3) robustez (A9/A10) y subir el ring out a 35–60 s; (4) prueba larga con WiFi (A12); (5) el sink de socket del compañero. Decidir con él: mono L o mezcla, tamaño de mensaje (hoy 4 slots = 128 ms) y header del cable. Estimación gruesa original: 4–6 días de trabajo concentrado más 2–4 de integración; el transporte del compañero no está estimado.

## Hecho y validado en el chip

- **A0** flash QIO 80 MHz, CPU 240 MHz, tick 1 ms, particiones con 2 slots OTA + `model` + `clips` (`arquitectura.md` §1). **A1** contador de overflows del DMA (probado forzando la falla). **A2** captura en tarea propia (core 1, prio 6). **A3** `audio_capture`/`wav_writer` en `components/orbita_audio/`. **A4** chunk = 1024 frames = chunk de `feed`, con chequeo contra el AFE al arrancar.
- **Pipeline:** captura → ring in (8 chunks, NOSPLIT) → `tarea_feed` → AFE → `tarea_fetch` (core 1; el AFE interno corre en core 0). Toma de 60 s: bytes completos, todos los `overruns` y `dma_overflows` en 0, `feed`=940, `fetch`=1880 (2 por feed), stacks libres captura 3188 / feed 3144 / fetch 1904 B.
- **Ring out y sink (13/10, compilan con y sin AFE, sin probar en el chip):** `audio_ring_out.c` (cola FIFO de 1024 slots en PSRAM, spinlock, descarta el más viejo si se llena), productor en `pipeline.c`, tarea sink (core 0) con `orbita_sink_t` y `test_sink` de lab, `VAD_OK` solo si el VAD quedó activo, baseline de pérdidas al arrancar y ring in en PSRAM. Dos reviews del `embedded-reviewer`. Diseño en `arquitectura.md` §3.4–3.5, tiempos en §3.7.
- **Test de audio** (borrable, en `main/test.c`): WAV de 2 canales (ch0 mic crudo L, ch1 salida del AFE), 64 bloques = 2,048 s. Scripts: `tools/orbita_serial.py` (guarda en `tools/prueba_espectrogramas/`, pide `Fin:` con `'s'` si no la ve) y `tools/espectrograma.py` (espectrogramas crudo / AFE / celu, `--alinear`, SNR por banda).
- Diseño cerrado de A5/A6 (rings, header de 16 B, tabla de segmentos, pipelining) en `arquitectura.md` §3.4–3.6. Vocabulario fijo: chunk / mensaje / segmento.

## Medido del AFE (2 mics, `AFE_TYPE_SR`, LOW_COST)

- `feed` = 1024 muestras/canal × 2 = 64 ms (~20 ms de cómputo); `fetch` = 512 muestras mono = 32 ms; `data_size` en bytes. Latencia de la tubería: **64 ms** exactos.
- Buffer interno 50 frames = 1,6 s: si no se hace `fetch`, **sobrescribe sin frenar** y solo avisa por log. Memoria mayormente en PSRAM. Pipeline SE(BSS) + VAD WebRTC, **sin NS** (con 2 mics el beamforming desplaza a NS).
- **Hallazgo:** con la config por defecto la salida del AFE era **el mic 0 sin procesar** (idéntico bit a bit al crudo, solo 64 ms después; SNR igual). Con micrófono cerca y cuarto silencioso, celu y ESP dieron espectrogramas casi iguales (SNR 41,6 vs 40,2 dB, una toma).
- Probado: `fixed_first_channel=false` + VAD como disparador → **sigue idéntico** (31170 de 31744 muestras iguales; solo difieren los últimos 36 ms, artefacto fijo del cierre de toma). VAD WebRTC modo 0 marca voz en el 100 % de los bloques (en un tramo de 0,5 s había mucho grave bajo 50 Hz; el offset de continua de tomas de 20 s resultó despreciable: la hipótesis del filtro de continua no se sostiene). `AFE_TYPE_VC` trabaja con frames de 10 ms (feed de 160). NS en `AFE_TYPE_SR` no se activa (pipeline vacío; `feed` baja a 512): el NS del AFE solo existe en VC.
- **Cerrado (12/10): el BSS del AFE no actúa.** Con SE(BSS)+VAD y cada combinación de `fixed_first_channel`, `fixed_output_channel` y `vad_enable_channel_trigger` (tomas `tools/prueba_afe/prueba_fixed1–3.wav`), la salida es el mic 0 idéntico (99,97–100 %, con 64 ms de retardo en una toma). Beamforming del AFE descartado; si hace falta, `mase_process` u offline en la PC. VAD WebRTC modo 0 en silencio: ~15 % de bloques con `VOZ` (arranque ~1,2 s + arrastre de 1 s por `vad_min_noise_ms`); `vad_energy_threshold` solo aplica con modelo VADNet. **El AEC del AFE queda como candidato:** se usa solo (`aec_init`, `se_init=false`, formato `"MR"`) con la salida I2S como referencia digital alineada en el `feed`; probarlo cuando haya parlante.
- El código del AFE sigue en `audio_afe.c` (config del experimento NS-solo); se activa con `idf.py menuconfig` → Orbita pipeline → Usar el AFE (`CONFIG_ORBITA_PIPELINE_USA_AFE`).

## Retomar acá

**Decisión (2026-10-11): el AFE y el NS quedan en pausa.** El AFE entregó el mic 0 sin procesar en todas las configs probadas; `esp-sr` desaconsulta el NS para reconocimiento de voz; y no hay evidencia de que el ruido dañe la detección. Se decide con datos de la wake word, no en el chip.

1. **Grabar con el firmware con el AFE apagado en menuconfig** (WAV de 2 canales: mic L y mic R crudos, 2,048 s) junto con el celu: distancias, con y sin ruido continuo, varias voces. Es lo que necesita la comparación de las tres fuentes (hoy: celu, ESP-L, ESP-R).
2. **Entrenar y evaluar la wake word con audio crudo** (ruido mezclado como aumento): falsas activaciones por hora y rechazos.
3. **Solo si el ruido resulta un problema:** probar NS y beamforming **offline en la PC** sobre las tomas L+R, y portar a la placa lo que ayude. Alternativas ya relevadas: NS WebRTC directo (`esp_ns.h`, 10 ms, sin AFE), `mase_process` (BSS directo, mics a 50 mm), NSNet; plan B: crudo a Edge Impulse.
4. Medir CPU por core y RAM interna/PSRAM con el pipeline que quede (hoy sin AFE sobra margen), y que el WAV no tenga texto adentro.

## VAD como compuerta de envío (propuesta 12/10, pendiente)

**Idea:** no mandar todo el tiempo; el VAD decide qué se manda. **Orden decidido (13/10):** (1) merge con audio continuo y prueba con el servidor; (2) VAD suelto, **sin AFE** (`esp_vad.h`, WebRTC; procesa frames de 10/20/30 ms y el slot es de 32 ms: decidir cómo encajarlo), medido contra silencio y voz en los modos 0–3; (3) compuerta **en el sink, no en el productor**: el ring out guarda todo con `VAD_OK`/`VOZ` y el sink decide qué enviar con pre-roll y hangover, como interruptor (continuo / por `VOZ`) para comparar ahorro vs. audio perdido; (4) recién ahí decidir si el `uart_sink` y `test.c` se achican o se borran. Segmento mínimo para no mandar mensajes de una palabra.

**Veredicto:** el VAD sirve como compuerta del fondo, **no reemplaza la wake word** (el urgente, "Órbita, ¿qué hora es?", se distingue por la palabra; en un consultorio hay voz todo el día). Y no se puede diseñar sobre un VAD que hoy marca 100 % de voz.

1. **Debuggear el VAD primero (sin AFE)** (hoy WebRTC modo 0 = el más permisivo): probar modo 3, `vad_min_speech_ms` / `vad_min_noise_ms` / `vad_delay_ms` de `afe_config` (verificar nombres), VADNet, y el VAD WebRTC suelto (`esp_vad.h`, verificar) sobre mono sin el AFE. Medir con silencio puro y con voz: % de bloques con `VOZ`. Criterio: ~0 % en silencio.
2. **Compuerta = pre-roll corto + hangover:** el VAD tarda en detectar el inicio y marca fin con retraso. Sin ~300–500 ms previos se pierde el comienzo de la frase. El ring out ya guarda todos los slots (también los de silencio); el pre-roll sale de ahí. El `vad_cache` del AFE hoy no se lee y probablemente duplica audio ya guardado: primero medir si `vad_cache_size > 0` alguna vez (A5b).
3. **Privacidad:** mandar todo lo que suene a voz es una política de captura (Fase C). Con la wake word, solo se envía lo pedido. Decidirlo con el compañero y el cliente, no por ancho de banda (32 KB/s mono es poco para el WiFi; el costo real es el procesamiento del backend).

## Qué pasa con `test.c` (decidido 13/10)

- **No se borra todavía.** Es el único camino que graba el **WAV de 2 canales** (L y R crudos, o crudo + AFE) que hacen falta para la comparación de las tres fuentes y para `comparar_canales.py`; el ring out es mono y no lo cubre. Queda como módulo de laboratorio (`#if`) hasta tener esas tomas.
- El `uart_sink` (A7) reemplazaría solo el envío de audio **mono** por UART; el comando `g` deja de hacer falta si se manda de forma continua. Con streaming continuo por UART0 no puede salir ningún log por esa UART: logs por USB-Serial-JTAG o apagados.
- El destino del prototipo es el socket del compañero; la UART es solo herramienta de laboratorio, por eso no bloquea el merge.
- `app_main` hoy termina en `test_loop()` (no vuelve): al integrar el socket, su init va antes o `test_loop` va bajo `#if`.

## Decisiones por hablar con el compañero (contexto en `arquitectura.md` §3.4–3.6)

1. ¿Quién cierra el pedido urgente? Provisorio: el equipo, tras ≥ X ms sin `VOZ` (`dev_fin_segmento`); el backend puede responder antes si procesa en streaming.
2. Recibir mientras se envía (necesario para el pipelining híbrido). Impacta la FSM.
3. Tamaño y criterio del segmento de fondo (pausa de voz con máximo; el número lo define el backend).
4. Qué resumen de `VOZ` lleva el header del cable.
5. Eventos `srv_` nuevos para falsos negativos y positivos de la wake word.
6. Id de segmento y de sesión; `srv_respuesta_recibida` con el id; retomar vs. reenviar el segmento entero.
7. FSM: sacar "buffer lleno" de los motivos de `dev_fin_segmento` y definir el arco de la wake word en `server_back`. Actualizar `órbita..md`.
8. Largo máximo de una respuesta de `audio_play` (afina el ring out).
9. Política del ring in lleno: hoy descarta el chunk nuevo (hueco de 64 ms que afecta la ventana de la wake word); evaluar descartar el más viejo.
10. Capacidad del ring out: hoy 32,8 s (1024 slots); el diseño pide ~60 s. Ajustar con el `max_ocupados` real y la red de Juani.
11. Tamaño del mensaje (`ORBITA_MSG_SLOTS`, hoy 4 = 128 ms) y qué resumen de `VOZ` va al cable: el sink entrega `n_voz` y `n_vad_ok` crudos.
12. Si `send` falla: hoy el mensaje se descarta; reintentos y reconexión (`open/close`) dependen del socket.

## Fase A — pendiente

**Terminado =** `overruns=0` y `dma_overflows=0` en toda captura; el consumidor se entera de un hueco (discontinuidad marcada, nunca se rellena en silencio); el consumidor es intercambiable (UART hoy, socket mañana) sin tocar captura ni ring. No tocar la ganancia/shift de captura (picos en −25 a −30 dBFS): invalida lo grabado.

- [x] **A5. Ring out v1 (13/10, compila con y sin AFE; sin probar en el chip).** `audio_ring_out.c` (init, write, read, stats), productor en `pipeline.c` (sin AFE desde `tarea_feed`, con AFE desde `tarea_fetch`; llamada directa, sin el callback `on_audio` del plan original) y flags `VAD_OK`/`VOZ`. Diseño en `arquitectura.md` §3.4. El 3er cursor y la tabla de segmentos pasan a A6.
- [ ] **A5b. Pendientes de la revisión del 13/10:** hecho: sumar `chunks_invalidos` y `fetch_invalidos` a `lost_samples`; exportar el 240 del DMA; subir `ORBITA_RING_SLOTS` (1024). Falta: (2) contador de cuántas veces `vad_cache_size > 0` en `audio_afe.c`, antes de decidir qué hacer con el cache; (3) la pérdida se anota en el primer slot que escribe `feed`/`fetch`, hasta ~0,5 s después del hueco real: si hace falta precisión, que la captura marque el hueco en el propio chunk (cambia el formato del ring in); (4) `_Static_assert` de `ORBITA_MSG_SLOTS * 512 <= UINT16_MAX` y de `ORBITA_SLOT_SAMPLES == ORBITA_AFE_FETCH_FRAMES`; (5) `orbita_ring_out_get_stats` debería avisar si falla.
- [ ] **A7. Sink:** hecho: interfaz `orbita_sink_t` (`send` + `ctx`), tarea sink (core 0, prio 5, polling de 10 ms: se deja), `test_sink` de lab. Falta: `uart_sink` (lo que hace hoy `tarea_envio`), `throttled_sink` (consumo a ritmo configurable, se puede "cortar"), `open/close` cuando lo pida el socket, y el socket (compañero). En `fetch` la copia al ring es siempre sin esperar.
- [ ] **A8. Control:** `main.c` traduce comandos (`'g'`, luego la FSM) a llamadas; la captura hace solo I2S → ring in.
- [x] **A9 (13/10, compila; falta una toma en el chip que lo confirme).** `CONFIG_I2S_ISR_IRAM_SAFE=y` en `sdkconfig.defaults` y DMA de 8 descriptores (120 ms; `ORBITA_DMA_DESC_NUM`/`FRAME_NUM` en `audio_capture.h`). Quien ya tenga `sdkconfig` debe activarlo en menuconfig o regenerarlo. Antes:  Riesgo real solo con **escrituras a flash durante la captura** (NVS, OTA): sin IRAM-safe, la ISR del I2S espera a la flash y el DMA tiene 90 ms de margen. Hacerlo antes de sumar WiFi/NVS/OTA. Verificar en qué core queda la ISR del I2S cuando entre WiFi.
- [ ] **A10. Errores:** si falla un init/`malloc`/`create`, loguear y reiniciar (hoy queda mudo; **provisorio de laboratorio:** `arranque_fallo()` en `main.c` deja el equipo parado repitiendo el error cada 2 s para poder leerlo; en producto cambiarlo a reinicio con contador en RTC para no entrar en bucle, o modo de fallo con aviso por LED); timeouts finitos en USB-JTAG y en la espera de `sender_done`; guarda de `s_rx_chan == NULL`; `uart_write` con resultado; framing con secuencia en el UART (sirve también para sockets).
- [ ] **A11.** ICS-43434 soporta 16 kHz (verificar en datasheet; el reviewer duda).
- [ ] **A12. Validación:** captura de 10 min con contadores en 0; corte simulado con `throttled_sink` de 5/10/20 s (valida el tamaño del ring out).
- [ ] **A13. Menores:** borrar `wav_pack_chunk_24bit`, capacidad de `out_buf` en `wav_pack_chunk_16bit`, contadores `volatile` → `stdatomic`.
- [x] **A14 (2026-10-12; probado en el chip con AFE apagado: toma de 20 s completa, contadores en 0).** Captura (+ AFE opcional por Kconfig) en `components/orbita_pipeline/`; la prueba de test quedó aislada en `main/test.c/.h` (se borra sacando esos dos archivos, la línea de `CMakeLists.txt` y sus llamadas en `app_main`). Falta probar con AFE.

- [ ] **A15. Tiempos del pipeline.** Agregado (13/10, compila): `orbita_tiempo.h` y máximos en `Fin:` ("us max", por toma) de la vuelta de captura sin la lectura (límite 26 ms), `xRingbufferSend`, `on_crudo`, chunks esperando en el ring in, vuelta de `feed` (límite 64 ms), `ring_out_write`, vuelta de `fetch` y `on_afe` (solo AFE), `ring_out_read` y `send` del sink. **Medido sin AFE (13/10, toma de 20 s, en reposo): todo en 0 y muy por debajo de los límites; números en `arquitectura.md` §3.7.** **Falta:** toma con AFE; la prueba de estrés (sink lento, escritura a flash en paralelo, WiFi) se hace cuando esté el socket de Juani, no antes; anotar sus máximos en §3.7.

- [ ] **A16. Pendientes de la revisión del 12/10.** Hecho: buffers de captura fijos, guarda de doble `start`, AFE por Kconfig, `hueco` en `on_crudo`, ring in en PSRAM explícito. Falta: `tarea_fetch` con `vTaskDelay(1)` si el AFE devuelve error rápido (al reactivar el AFE); `orbita_audio_reset_dma_overflows()` o encender el I2S después de crear el AFE; `ESP_LOGE` en cada fallo de lectura I2S (hasta ~100 logs/s); `#define` nombrados para stack/prioridad/core de las tareas; liberar `afe_config` en `audio_afe.c`; `REQUIRES` de `main` al borrar `test.c`.

## Fase B — AFE medido (cierra H1)

- [ ] Verificar cómo carga `esp-sr` NSNet/VADNet (partición `model`) y cómo se flashean los modelos.
- [ ] **WebRTC vs. NSNet/VADNet:** modo 1 `SR_NSN_WEBRTC` + `SR_VADN_WEBRTC`; modo 2 `NSNET2`/`NSNET3` + `VADNET1_MEDIUM` (NSNet3 113 KB, NSNet2 334 KB). Medir CPU por core, RAM, `overruns`/`dma_overflows` en 0 y calidad (SNR; que no se coma r, b). Anotar el modo de cada toma.
- [ ] Que el AFE procese de verdad (ver "Retomar acá").
- [ ] Grabador de test con duración variable (hoy fija en 64 bloques): arrancar/parar por comando.
- [ ] Documentar RAM/CPU en `orbita-obsidian/plan-presupuesto-ram-cpu.md`.

## Fase C — listo para sockets (cierra H2)

- [ ] Mostrar A6 y A7 al compañero y acordar qué pide el transporte (reconexión, qué cierra el estado de envío, wake word a mitad de un envío; contrato = FSM del vault).
- [ ] Revisar con `fsm-check` / `fsm-consistency-reviewer`; actualizar el vault y `arquitectura.md` si cambia algo.
- [ ] Decidir política de captura (continuo vs. tras wake word) y mono vs. estéreo hacia el backend.
- [ ] Checklist de "mergeable" y `embedded-reviewer` sobre todo `orbita_audio`.

## En paralelo: wake word "órbita"

**Pregunta abierta:** ¿sirve entrenar con audio de celu? Se averigua grabando **las mismas tomas a la vez** con tres fuentes: celu, ESP crudo (ch0) y ESP + AFE (ch1, válida solo si el AFE procesa). Primera comparación (una toma, cerca, silencio): celu y ESP se parecen; falta ruido, distancia y más voces.

**Tomas:** "órbita" a 0,5 / 1 / 2 m × con y sin barbijo × ~5, más 1–2 min de ruido de labo; ruido continuo para ver el AFE. Se alinea con `--alinear` (retardo medido), no hace falta aplauso.

| Resultado | Qué hacer |
|---|---|
| Celu y ESP se parecen | Entrenar con celu + ruido de la placa mezclado |
| Difieren de forma pareja | Estimar el filtro celu → ESP y aplicarlo a los audios de celu |
| Difieren en ruido o nivel | Mezclar ruido de la placa y normalizar ganancia |
| El AFE cambia mucho la señal | Entrenar con salida del AFE o con crudo + ruido, y comparar |
| El AFE no aporta o rompe "órbita" | Dejarlo afuera (ahorra PSRAM y CPU) |

**Modelo v0 en Edge Impulse:** test fijo con "órbita" de 3–5 personas en la ESP + `ruido_labo3` para falsas activaciones/hora; variantes (celu limpio / + ruido / + tomas ESP). Métricas: FRR y falsas activaciones por hora. Más personas vale más que más tomas por persona. Lo posterior (Common Voice, WhatsApp, MS-SNSD, inyección de ruido) está en `orbita-dataset/dataset.md`.
