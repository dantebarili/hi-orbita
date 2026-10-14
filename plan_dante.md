# TODO — Frente Audio (después del alfa)

Actualizado 2026-10-13. El camino inmediato está en `plan_prototipo_alfa.md`; acá queda lo que sigue después. Diseño de rings, header y tiempos: `orbita-obsidian/arquitectura.md` §3.4–3.7.

## Medido del AFE (2 mics, `AFE_TYPE_SR`, LOW_COST)

- `feed` = 1024 muestras/canal × 2 = 64 ms (~20 ms de cómputo); `fetch` = 512 muestras mono = 32 ms. Latencia de la tubería: **64 ms**. Buffer interno 50 frames = 1,6 s: si no se hace `fetch`, sobrescribe sin frenar.
- **Con la config por defecto la salida del AFE es el mic 0 sin procesar** (idéntico bit a bit, 64 ms después). Probadas todas las combinaciones de `fixed_first_channel`, `fixed_output_channel` y `vad_enable_channel_trigger`: **el BSS del AFE no actúa** (cerrado 12/10). Beamforming del AFE descartado; si hace falta, `mase_process` u offline en la PC.
- NS en `AFE_TYPE_SR` no se activa (pipeline vacío); el NS del AFE solo existe en `AFE_TYPE_VC` (frames de 10 ms, feed de 160). VAD WebRTC modo 0 marca voz en el 100 % de los bloques (en silencio ~15 %); `vad_energy_threshold` solo aplica con VADNet.
- **AEC del AFE, candidato:** solo (`aec_init`, `se_init=false`, formato `"MR"`) con la salida I2S como referencia digital alineada en el `feed`; probarlo cuando haya parlante.
- El código del AFE sigue en `audio_afe.c`; se activa en menuconfig → Orbita pipeline → Usar el AFE.

## Retomar: AFE y NS en pausa (decisión 11/10)

El AFE entregó el mic 0 sin procesar en todas las configs; `esp-sr` desaconseja el NS para reconocimiento de voz; no hay evidencia de que el ruido dañe la detección. Se decide con datos de la wake word, no en el chip.

1. Grabar con el AFE apagado (WAV de 2 canales, L y R crudos, 2,048 s) junto con el celu: distancias, con y sin ruido, varias voces.
2. Entrenar y evaluar la wake word con audio crudo (ruido mezclado como aumento): falsas activaciones por hora y rechazos.
3. Solo si el ruido es un problema: NS y beamforming **offline en la PC** sobre las tomas L+R y portar lo que ayude. Alternativas: NS WebRTC directo (`esp_ns.h`), `mase_process`, NSNet; plan B: crudo a Edge Impulse.
4. Medir CPU por core y RAM interna/PSRAM con el pipeline que quede.

## VAD como compuerta de envío

El VAD sirve como compuerta del fondo, **no reemplaza la wake word** (en un consultorio hay voz todo el día). No se diseña sobre un VAD que hoy marca 100 % de voz. Orden decidido (13/10):

1. Merge con audio continuo y prueba con el servidor (alfa).
2. VAD suelto, **sin AFE** (`esp_vad.h`, WebRTC; frames de 10/20/30 ms vs. slot de 32 ms: decidir cómo encajarlo), medido contra silencio y voz en los modos 0–3. Criterio: ~0 % en silencio.
3. Compuerta **en el sink, no en el productor**: el ring out guarda todo con `VAD_OK`/`VOZ` y el sink decide con pre-roll (~300–500 ms) y hangover, como interruptor continuo / por `VOZ`. Segmento mínimo para no mandar una palabra suelta. El `vad_cache` del AFE hoy no se lee: medir si `vad_cache_size > 0` alguna vez (A5b).
4. Privacidad: mandar todo lo que suene a voz es política de captura; decidirla con el compañero y el cliente, no por ancho de banda.

## `test.c`

No se borra todavía: es el único camino que graba el **WAV de 2 canales** (L y R crudos, o crudo + AFE) para la comparación de las tres fuentes y `comparar_canales.py`. Queda como módulo de laboratorio (`#if`). `app_main` termina en `test_loop()`: al integrar el socket, su init va antes o `test_loop` va bajo `#if`.

## Decisiones abiertas con el compañero (las del alfa están en su Paso 0)

1. ¿Quién cierra el pedido urgente? Provisorio: el equipo, tras ≥ X ms sin `VOZ` (`dev_fin_segmento`).
2. Tamaño y criterio del segmento de fondo (pausa con máximo; lo define el backend).
3. Qué resumen de `VOZ` lleva el header del cable y tamaño del mensaje (`ORBITA_MSG_SLOTS`, hoy 4 = 128 ms).
4. Eventos `srv_` nuevos para falsos negativos y positivos de la wake word; id de segmento y de sesión.
5. FSM: sacar "buffer lleno" de los motivos de `dev_fin_segmento`, definir el arco de la wake word en `server_back`, actualizar `órbita..md`.
6. Ring in lleno: hoy descarta el chunk nuevo (hueco de 64 ms que afecta la wake word); evaluar descartar el más viejo.
7. Capacidad del ring out: hoy 32,8 s (1024 slots); el diseño pide ~60 s. Ajustar con el `max_ocupados` real.
8. Si `send` falla: hoy el mensaje se descarta; reintentos y `open/close` dependen del socket.

## Deuda técnica pendiente (lo que no entra en el alfa)

**Terminado =** `overruns=0` y `dma_overflows=0` en toda captura; los huecos se marcan, nunca se rellenan; el consumidor es intercambiable sin tocar captura ni ring. No tocar la ganancia/shift de captura (picos en −25 a −30 dBFS): invalida lo grabado.

- **A5b:** contador de `vad_cache_size > 0`; la pérdida se anota hasta ~0,5 s después del hueco real (si hace falta precisión, que la captura marque el hueco en el chunk); `orbita_ring_out_get_stats` debería avisar si falla.
- **A7:** `uart_sink`, `throttled_sink` (consumo a ritmo configurable), `open/close` cuando lo pida el socket.
- **A8:** `main.c` traduce comandos a llamadas; la captura hace solo I2S → ring in.
- **A10:** resto de errores: `arranque_fallo()` pasa a reinicio con contador en RTC o aviso por LED; timeouts finitos en USB-JTAG y en `sender_done`; `uart_write` con resultado; framing con secuencia en el UART.
- **A11:** verificar en datasheet que el ICS-43434 soporta 16 kHz.
- **A13:** borrar `wav_pack_chunk_24bit`, capacidad de `out_buf` en `wav_pack_chunk_16bit`, contadores `volatile` → `stdatomic`.
- **A15:** toma con AFE y anotar máximos en §3.7.
- **A16:** `tarea_fetch` con `vTaskDelay(1)` si el AFE devuelve error rápido; `orbita_audio_reset_dma_overflows()` o encender el I2S después de crear el AFE; `#define` nombrados para stack/prioridad/core; liberar `afe_config`; `REQUIRES` de `main` al borrar `test.c`.

## Fase B — AFE medido

- [ ] Verificar cómo carga `esp-sr` NSNet/VADNet (partición `model`) y cómo se flashean.
- [ ] WebRTC vs. NSNet/VADNet: modo 1 `SR_NSN_WEBRTC` + `SR_VADN_WEBRTC`; modo 2 `NSNET2`/`NSNET3` + `VADNET1_MEDIUM`. Medir CPU por core, RAM, contadores en 0 y calidad (SNR; que no se coma r, b).
- [ ] Que el AFE procese de verdad (ver "Retomar").
- [ ] Grabador de test con duración variable (hoy fija en 64 bloques).
- [ ] Documentar RAM/CPU en `orbita-obsidian/arquitectura.md`.

## En paralelo: wake word "órbita"

**Pregunta abierta:** ¿sirve entrenar con audio de celu? Se averigua grabando **las mismas tomas a la vez** con tres fuentes: celu, ESP crudo (ch0) y ESP + AFE (ch1, válida solo si el AFE procesa). Primera comparación (una toma, cerca, silencio): celu y ESP se parecen; falta ruido, distancia y más voces.

**Tomas:** "órbita" a 0,5 / 1 / 2 m × con y sin barbijo × ~5, más 1–2 min de ruido de labo; ruido continuo para ver el AFE. Se alinea con `--alinear`.

| Resultado | Qué hacer |
|---|---|
| Celu y ESP se parecen | Entrenar con celu + ruido de la placa mezclado |
| Difieren de forma pareja | Estimar el filtro celu → ESP y aplicarlo a los audios de celu |
| Difieren en ruido o nivel | Mezclar ruido de la placa y normalizar ganancia |
| El AFE cambia mucho la señal | Entrenar con salida del AFE o con crudo + ruido, y comparar |
| El AFE no aporta o rompe "órbita" | Dejarlo afuera (ahorra PSRAM y CPU) |

**Modelo v0 en Edge Impulse:** test fijo con "órbita" de 3–5 personas en la ESP + `ruido_labo3` para falsas activaciones/hora; variantes (celu limpio / + ruido / + tomas ESP). Métricas: FRR y falsas activaciones por hora. Más personas vale más que más tomas por persona. Lo posterior (Common Voice, WhatsApp, MS-SNSD, inyección de ruido) está en `orbita-dataset/dataset.md`.
