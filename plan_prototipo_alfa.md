# Plan — Prototipo alfa (mono continuo por sockets + parlante)

Actualizado 2026-10-13. Deriva de `plan_dante.md` (captura/AFE/wake word). Detalle técnico del cable y del servidor: `INTEGRACION_WEBSOCKET.md`.

## Objetivo

El equipo manda **audio mono 16 kHz / 16-bit sin parar** al servidor. El **servidor** transcribe y detecta "Órbita": ahí corta el procesamiento de fondo y responde con un audio que el equipo reproduce (`audio_play`). **Sin wake word en el chip, sin AFE, sin VAD** (el VAD entra después como compuerta opcional). Fuera de alcance (sigue en `plan_dante.md`): wake word on-device, NS/beamforming, FSM `dev_wake_word*`. La FSM del vault no aplica a este prototipo: es "modo prototipo", no se actualiza.

## Cómo se trabaja: 3 pasos

1. **Juntos:** cerrar el contrato (Paso 0).
2. **Por separado:** cada uno en su rama y en sus carpetas (Dante → `dante`, Juani → `juani`), sin tocar las del otro.
3. **Unir:** otro día, conectando las dos mitades y probando todo junto (Paso 2).

## Hecho

- [x] **S1 Retención.** Sin red el sink espera con el mensaje en la mano y el ring (32,8 s) acumula; si se llena descarta lo más viejo y lo marca. Si el ring pisó ≥ 1 s mientras se retenía, el mensaje retenido se descarta.
- [x] **S2 Reconexión.** Backoff 1-2-4-8 s, heartbeat 10 s / pong 3 s, tarea de red en core 0, un solo `hello` por conexión, captura antes que Wi-Fi. Warm-up de 5 s (arranque del mic y 21 overflows del DMA, informados como `dma_boot`).
- [x] **S3 Servidor continuo + firmware v2.** Segmentos de 30 s con su JSON de huecos, `ack`, hueco entre sesiones. Probado en la placa (sesión de 48 s, hueco de 27 s bien marcado). 22 pruebas automáticas.
- [x] Revisión `embedded-reviewer` (13/10) aplicada, salvo lo de S4 y "Producción".
- [x] Prueba 1 del registro (`pruebas_socket.md`) y toma de 10 min: ESP estable.

## Protocolo `orbita-pcm-v2` (implementado salvo lo marcado)

Una conexión WebSocket larga, una sesión por conexión, sin `end`:

- ESP → servidor: `hello` (JSON) una vez; después binarios `ORBA` (header 40 B + hasta 4 slots = 128 ms de PCM), `index` creciente sin cortes. Los huecos se anotan (`lost`, `seq`), no se rellenan.
- Servidor → ESP: `ready` (JSON) y `ack` (último `index` recibido) cada ~1 s.
- **Falta definir (Paso 0):** `orbita` (se detectó la palabra), `play` (id, bytes), audio binario del parlante y `play_end`.
- Tras reconectar: sesión nueva (id nuevo); el servidor mide el hueco por `seq`. Todavía no hay reenvío desde el último `ack`.

## Paso 0 — Juntos (antes de separarse, ~1 h)

Se escribe en `INTEGRACION_WEBSOCKET.md` y se commitea a **las dos ramas** antes de empezar el Paso 1. Sin esto, S6 y S7 se escriben en el aire.

- [ ] **Mensajes nuevos** con sus campos: `orbita`, `play` (id, bytes), `play_end`.
- [ ] **Audio del parlante:** binario con header propio (distinto de `ORBA`); formato (PCM 16 kHz / 16-bit mono) y largo máximo de la respuesta (fija el buffer en PSRAM).
- [ ] **Mic durante el parlante:** sin AEC, lo más simple para el alfa es pausar el envío mientras suena (si no, el servidor transcribe el propio parlante). Decidir y anotar.
- [ ] **Reenvío tras reconexión:** ¿alcanza marcar el hueco o hace falta reenviar desde el último `ack` (A6)?
- [ ] **Diferencias con la FSM** (backoff sin tope de intentos, sin timeout de 5 s de respuesta, sin eventos `srv_*`): confirmar que se aceptan para el alfa.
- [ ] **Interfaz de unión** acordada: firmware de Juani entrega el audio de respuesta a un callback `on_play_data(data, len)` (más `play_begin(id, bytes)` / `play_end()`); firmware de Dante implementa la reproducción detrás de esa API. Es lo único que se toca de los dos lados.
- [ ] **S4 (RAM) lo hace Dante, primero:** toca `orbita_ws` (de Juani), así que se avisa y se commitea a ambas ramas antes de separarse.

## Paso 1a — Rama `dante` (firmware, hardware, audio)

Carpetas: `firmware/components/orbita_audio`, `orbita_pipeline`, `orbita_playback` (nuevo), `sdkconfig.defaults`, `firmware/main/`.

- [ ] **S4 Presupuesto de RAM** (requisito de S7). `orbita_ws_message_t` lleva `data[8192]` y se copia 6 veces (cola RX de 4, `s_rx`, `reply`...): ≥ 50 KB internos para recibir un `ready` de ~50 B. Bajar el mensaje de control a ≤ 512 B, mover buffers grandes a PSRAM, medir RAM interna antes/después y anotar en `orbita-obsidian/arquitectura.md`.
- [ ] **S4b Revisión de memoria** (a hablar; auditoría del 13/10 sobre el código, sin medir en la placa). Hoy hay ~80 KB de RAM interna libre con Wi-Fi + socket, sin AFE ni parlante.
  - **Mal alojado:** el módulo WebSocket pasa de 60 KB internos (cola RX 4 × 8,2 KB, `s_rx`, `s.reply`, buffer de la librería 8 KB, pila 6 KB, `packet` 4 KB). Es S4: más de la mitad de lo que queda.
  - **Bien alojado:** ring out (1,04 MB) y ring in (33 KB) en PSRAM; DMA del I2S (~15 KB) y buffers de captura/sink (~25 KB) en interna.
  - **Pulir:** (1) medir `ram_int` con parlante + AFE (si se activa) antes de elegir `ws://` o `wss://`: TLS suma decenas de KB internos; (2) pilas fijas en ~37 KB con `highwater` medido solo para el sink (queda 3,7 KB libres de 6): medir las demás; (3) `ram_int` bajó de 82 a 61 KB en 12 handshakes fallidos (~1,7 KB cada uno) y se recuperó: confirmar con un corte largo; (4) buffer de reproducción en PSRAM con tamaño que fija el Paso 0 (largo máximo de la respuesta).
- [ ] **Pruebas en la placa** (registro en `pruebas_socket.md`):
  2. Corte corto de Wi-Fi (~10 s) con la sesión en curso: reconecta, el servidor no cierra con error, hueco entre sesiones = 0.
  3. Corte largo (> 40 s): no salta el watchdog (el sink drena el atraso sin dormir, prio 5 > red 4) y el descarte del mensaje retenido aparece en el log.
- [ ] **S7 Parlante (lado hardware), sin red.** Amplificador I2S clase D (pines con `pin-map-check`: fuera de GPIO 26–37, 19/20, 43/44; puede necesitar 5 V y picos de corriente: probar con fuente decente), salida I2S, buffer de reproducción en PSRAM, API `play_begin` / `play_push` / `play_end`. Se prueba con un tono o un clip de la partición `clips`. AEC queda para después.
- [ ] **Deuda que afecta al alfa** (detalle en `plan_dante.md`):
  - **A9:** confirmar en una toma con Wi-Fi que `CONFIG_I2S_ISR_IRAM_SAFE` y el DMA de 8 descriptores aguantan (quien ya tenga `sdkconfig` debe regenerarlo o activarlo en menuconfig).
  - **A16:** limitar el `ESP_LOGE` de fallo de lectura I2S (hasta ~100 logs/s).
  - **A5b:** `_Static_assert` de `ORBITA_MSG_SLOTS * 512 <= UINT16_MAX` y de `ORBITA_SLOT_SAMPLES == ORBITA_AFE_FETCH_FRAMES`.
  - **A10 mínimo:** guarda de `s_rx_chan == NULL`. Para la hora de prueba, `arranque_fallo()` queda como está.
- [ ] **S8 VAD como compuerta (opcional, mínimo).** Anotar los números de la prueba del 13/10 (modo 4 anduvo bien; el 100 % de voz era del modo 0) y reservar en el header un flag de "silencio salteado" (≠ pérdida).
- [ ] **En paralelo:** wake word y tomas de las tres fuentes (`plan_dante.md`); no bloquea nada.

## Paso 1b — Rama `juani` (comunicación y servidor)

Carpetas: `tools/` (servidor), `firmware/components/orbita_ws`, `orbita_audio_ws`, `orbita_wifi`.

- [ ] **S6 Servidor:** transcripción, detección de "Órbita", envío de `orbita`, `play` y el audio binario.
- [ ] **Cliente simulado de la placa** (script que habla `orbita-pcm-v2` y recibe `play`): permite probar todo el downlink sin hardware (base: `tools/ws_audio_session.py`).
- [ ] **Recepción en el firmware:** parsear `orbita`, `play`, `play_end` en `orbita_ws` y entregar los binarios al callback `on_play_data`, en buffer propio (no por la cola RX). No toca el parlante.
- [ ] **Reenvío desde el último `ack` (A6)**, solo si el Paso 0 dice que hace falta.

## Paso 2 — Unir (otro día)

- [ ] Merge de `dante` y `juani`; conectar `on_play_data` → `play_*` (pocas líneas).
- [ ] **S5 Prueba larga:** 1 h con Wi-Fi y cortes de 5/10/20 s: `dma_overflows` = 0, `ring_in_overruns` = 0, huecos solo los de los cortes. `ram_int` y `pila_sink` estables.
- [ ] Prueba de estrés del pipeline (sink lento, flash y Wi-Fi en paralelo) y máximos en `arquitectura.md` §3.7 (A15).

## Terminado cuando

Una hora seguida con el servidor transcribiendo, un corte de red de 20 s sin pérdida o con el hueco bien marcado, "Órbita" dicha al equipo y la respuesta sonando por el parlante, sin reiniciar.

## Producción (no bloquea el prototipo)

SSID/clave/URI en Kconfig → aprovisionamiento por NVS; `ws://` → `wss://` (CA, reloj, +RAM y +pila); `nvs_flash_init` sin el caso `NO_FREE_PAGES` (borrar la NVS destruiría las credenciales); warm-up fijo de 5 s propio del INMP441 (en la placa final conviene un pasa-altos); `arranque_fallo` en bucle (A10); sin reenvío desde el último `ack` (A6).
