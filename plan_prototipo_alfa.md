# Plan — Prototipo alfa (mono continuo por sockets + parlante)

Actualizado 2026-10-13. Deriva de `plan_dante.md` (captura/AFE/wake word). Detalle técnico del cable y del servidor: `INTEGRACION_WEBSOCKET.md`.

## Objetivo

El equipo manda **audio mono 16 kHz / 16-bit sin parar** al servidor. El **servidor** transcribe y detecta "Órbita": ahí corta el procesamiento de fondo y responde con un audio que el equipo reproduce (`audio_play`). **Sin wake word en el chip.** El **AFE entra al alfa** solo para dos cosas: **AEC** (que el equipo no se escuche a sí mismo por el parlante) y **VAD** (cortar la respuesta cuando el usuario habla encima, `barge_in`). Fuera de alcance (sigue en `plan_dante.md`): wake word on-device, NS/beamforming, VAD como compuerta de envío, FSM `dev_wake_word*`. La FSM del vault no aplica a este prototipo: es "modo prototipo", no se actualiza.

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
- **Downlink acordado el 14/10** (`orbita`, `play`, `play_end`, `play_stop`, `play_done`, ...): ver `INTEGRACION_WEBSOCKET.md`.
- Tras reconectar: sesión nueva (id nuevo); el servidor mide el hueco por `seq`. Todavía no hay reenvío desde el último `ack`.

## Paso 0 — Juntos (14/10)

Contrato en `INTEGRACION_WEBSOCKET.md` ("Downlink"). Decidido:

- [x] **`orbita` informativo:** el servidor detecta "Órbita" (STT + comparación aproximada) y avisa; el ESP lo registra y enciende un **LED de conversación** (es la idea del producto final); `conversation_end` lo apaga. El detalle de la conversación con el LLM no afecta al ESP.
- [x] **Audio del parlante:** PCM s16le 16 kHz mono, frames `PLAY` ≤ 4 KB, **streaming con prebuffer** (inicial 400 ms) para reproducir en tiempo real y bajar la latencia. El servidor puede mandar más rápido que tiempo real; el buffer en PSRAM absorbe la diferencia.
- [x] **Mic durante el parlante:** flag de S8 y el servidor ignora esos mensajes. **El ESP corta la reproducción** con `play_stop` (orden del servidor) y por barge-in propio (AEC + VAD, `barge_in`), que **entra al alfa** (S9); requiere actualizar la FSM del vault y un evento nuevo hacia el servidor (a acordar con Juani).
- [x] **Reenvío tras reconexión:** entra al alfa (A6): tercer cursor en el ring, el `ready` trae `last_index`.
- [x] **Diferencias con la FSM** (backoff sin tope, sin timeout de 5 s, sin `srv_*`): aceptadas para el alfa.
- [x] **Interfaz de unión:** `play_begin` / `play_push` / `play_end` / `play_stop` + `play_is_active()` + callback de eventos (`started`, `done`, `stopped`, `underrun`, `rejected`).
- [x] **S4 (RAM) lo hace Dante primero** (toca `orbita_ws`; avisar a Juani).
- [ ] **Pendiente (con Juani, medir):** velocidad real del downlink (hoy no está medida: los 237 KB/s fueron del uplink) y valor final del prebuffer.

## Paso 1a — Rama `dante` (firmware, hardware, audio)

Carpetas: `firmware/components/orbita_audio`, `orbita_pipeline`, `orbita_playback` (nuevo), `sdkconfig.defaults`, `firmware/main/`.

- [ ] **S4 Presupuesto de RAM** (requisito de S7 y de S9: el AFE ocupa 68 a 79 KB de RAM interna según Espressif, y hoy quedan ≈ 80; ver `arquitectura.md` §1). `orbita_ws_message_t` lleva `data[8192]` y se copia 6 veces (cola RX de 4, `s_rx`, `reply`...): ≥ 50 KB internos para recibir un `ready` de ~50 B. Bajar el mensaje de control a ≤ 512 B, mover buffers grandes a PSRAM, medir RAM interna antes/después y anotar en `orbita-obsidian/arquitectura.md`.
- [ ] **S4b Revisión de memoria** (a hablar; auditoría del 13/10 sobre el código, sin medir en la placa). Hoy hay ~80 KB de RAM interna libre con Wi-Fi + socket, sin AFE ni parlante.
  - **Mal alojado:** el módulo WebSocket pasa de 60 KB internos (cola RX 4 × 8,2 KB, `s_rx`, `s.reply`, buffer de la librería 8 KB, pila 6 KB, `packet` 4 KB). Es S4: más de la mitad de lo que queda.
  - **Bien alojado:** ring out (1,04 MB) y ring in (33 KB) en PSRAM; DMA del I2S (~15 KB) y buffers de captura/sink (~25 KB) en interna.
  - **Pulir:** (1) medir `ram_int` con parlante + AFE (si se activa) antes de elegir `ws://` o `wss://`: TLS suma decenas de KB internos; (2) pilas fijas en ~37 KB con `highwater` medido solo para el sink (queda 3,7 KB libres de 6): medir las demás; (3) `ram_int` bajó de 82 a 61 KB en 12 handshakes fallidos (~1,7 KB cada uno) y se recuperó: confirmar con un corte largo; (4) buffer de reproducción en PSRAM con tamaño que fija el Paso 0 (largo máximo de la respuesta).
- [ ] **Pruebas en la placa** (registro en `pruebas_socket.md`):
  2. Corte corto de Wi-Fi (~10 s) con la sesión en curso: reconecta, el servidor no cierra con error, hueco entre sesiones = 0.
  3. Corte largo (> 40 s): no salta el watchdog (el sink drena el atraso sin dormir, prio 5 > red 4) y el descarte del mensaje retenido aparece en el log.
- [ ] **S7 Parlante (lado hardware), sin red.** Amplificador I2S clase D (pines con `pin-map-check`: fuera de GPIO 26–37, 19/20, 43/44; puede necesitar 5 V y picos de corriente: probar con fuente decente), módulo `orbita_playback` dueño del I2S de salida (la referencia del AEC futuro sale de ahí), ring en PSRAM de una respuesta máxima (480 KB), reproducción con prebuffer y contador de `underrun`, API del Paso 0. Se prueba con un tono o un clip de la partición `clips`. AEC y barge-in: Fase 2.
- [ ] **S9 AFE con AEC y VAD (barge-in).** Va **después de S4 y S7**: necesita RAM interna libre y la referencia del parlante (`orbita_playback` es su dueño). Pasos: (1) log al arranque con el heap interno total y libre, y medir `ram_int` con el AFE encendido; (2) AEC solo (`aec_init`, sin NS ni BSS, referencia digital alineada en el `feed`; ver `plan_dante.md`), medido con el parlante sonando; (3) VAD en modo 4 (anduvo bien el 13/10; el 100 % de voz era del modo 0) como disparador de `play_stop` propio; (4) con AFE, `ws://` en el alfa: TLS no entra en la RAM interna. Criterio: sin falsos cortes con la respuesta sonando sola, y corta al hablar encima. Requiere acordar con Juani el evento `barge_in` y la FSM.
- [ ] **A6 Reenvío:** tercer cursor del ring ("confirmado por el servidor"), el sink retoma desde el `last_index` del `ready` (la parte del `hello`/`ready` es de Juani). Ver `arquitectura.md` §3.4.
- [ ] **LED de conversación:** un GPIO (con `pin-map-check`) que se enciende con `orbita` y se apaga con `conversation_end`. Es una prueba simple, pero ya fija un pin de la PCB.
- [ ] **Deuda que afecta al alfa** (detalle en `plan_dante.md`):
  - **A9:** confirmar en una toma con Wi-Fi que `CONFIG_I2S_ISR_IRAM_SAFE` y el DMA de 8 descriptores aguantan (quien ya tenga `sdkconfig` debe regenerarlo o activarlo en menuconfig).
  - **A16:** limitar el `ESP_LOGE` de fallo de lectura I2S (hasta ~100 logs/s).
  - **A5b:** `_Static_assert` de `ORBITA_MSG_SLOTS * 512 <= UINT16_MAX` y de `ORBITA_SLOT_SAMPLES == ORBITA_AFE_FETCH_FRAMES`.
  - **A10 mínimo:** guarda de `s_rx_chan == NULL`. Para la hora de prueba, `arranque_fallo()` queda como está.
- [ ] **S8 VAD como compuerta (opcional, mínimo).** Anotar los números de la prueba del 13/10 (modo 4 anduvo bien; el 100 % de voz era del modo 0) y reservar en el header un flag de "silencio salteado" (≠ pérdida).
- [ ] **En paralelo:** wake word y tomas de las tres fuentes (`plan_dante.md`); no bloquea nada.

## Paso 1b — Rama `juani` (comunicación y servidor)

Carpetas: `tools/` (servidor), `firmware/components/orbita_ws`, `orbita_audio_ws`, `orbita_wifi`.

- [ ] **S6 Servidor:** STT y TTS de prueba (Juani los prueba), detección de "Órbita" sobre la transcripción (comparación aproximada, probada con voces reales), y **simulación de la conversación** (el LLM todavía no está disponible). Alcance del alfa: ver qué pasa cuando llega un `orbita` y mandar al ESP un aviso que reproduzca algo.
- [ ] **Investigar** cómo se abre una conversación con un LLM (protocolo, historial/contexto, por turnos vs. voz en tiempo real) para no perder contexto. Bloquea el diseño final, no el alfa.
- [ ] **Medir el downlink** (timestamp de envío en el servidor vs. recepción en el ESP): velocidad real y valor de prebuffer. Si hace falta, revisar la cola RX, el buffer de 8 KB y la ventana TCP.
- [ ] **Cliente simulado de la placa** (script que habla `orbita-pcm-v2` y recibe `play`): permite probar el downlink sin hardware (base: `tools/ws_audio_session.py`).
- [ ] **Recepción en el firmware:** parsear `orbita`, `conversation_end`, `play`, `play_end`, `play_stop` en `orbita_ws` y entregar los frames `PLAY` al callback de `play_push`, en buffer propio (no por la cola RX); enviar `play_done` / `play_reject`. No toca el parlante.
- [ ] **`ready` con `last_index`** y manejo del reenvío en el servidor (A6, la mitad de Juani).

## Paso 2 — Unir (otro día)

- [ ] Merge de `dante` y `juani`; conectar `on_play_data` → `play_*` (pocas líneas).
- [ ] **S5 Prueba larga:** 1 h con Wi-Fi y cortes de 5/10/20 s: `dma_overflows` = 0, `ring_in_overruns` = 0, huecos solo los de los cortes. `ram_int` y `pila_sink` estables.
- [ ] Prueba de estrés del pipeline (sink lento, flash y Wi-Fi en paralelo) y máximos en `arquitectura.md` §3.7 (A15).

## Terminado cuando

Una hora seguida con el servidor transcribiendo, un corte de red de 20 s sin pérdida o con el hueco bien marcado, "Órbita" dicha al equipo y la respuesta sonando por el parlante, sin reiniciar.

## Producción (no bloquea el prototipo)

SSID/clave/URI en Kconfig → aprovisionamiento por NVS; `ws://` → `wss://` (CA, reloj, +RAM y +pila); `nvs_flash_init` sin el caso `NO_FREE_PAGES` (borrar la NVS destruiría las credenciales); warm-up fijo de 5 s propio del INMP441 (en la placa final conviene un pasa-altos); `arranque_fallo` en bucle (A10); sin reenvío desde el último `ack` (A6).
