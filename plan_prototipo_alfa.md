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
- [x] **Interfaz de unión:** `play_begin` / `play_push` / `play_end` / `play_stop` + `play_is_active()` + callback de eventos con el id de cada reproducción (`started`, `done`, `stopped`, `underrun`; los rechazos van solo por el valor de retorno, 14/10).
- [x] **S4 (RAM) lo hace Dante primero** (toca `orbita_ws`; avisar a Juani).
- [ ] **Pendiente (con Juani, medir):** velocidad real del downlink (hoy no está medida: los 237 KB/s fueron del uplink) y valor final del prebuffer.

## Paso 1a — Rama `dante` (firmware, hardware, audio)

Carpetas: `firmware/components/orbita_audio`, `orbita_pipeline`, `orbita_playback` (nuevo), `sdkconfig.defaults`, `firmware/main/`.

- [ ] **S4 Presupuesto de RAM** (requisito de S7 y de S9: el AFE ocupa 68 a 79 KB de RAM interna según Espressif, y hoy quedan ≈ 80; ver `arquitectura.md` §1). `orbita_ws_message_t` lleva `data[8192]` y se copia 6 veces (cola RX de 4, `s_rx`, `reply`...): ≥ 50 KB internos para recibir un `ready` de ~50 B. Bajar el mensaje de control a ≤ 512 B, mover buffers grandes a PSRAM, medir RAM interna antes/después y anotar en `orbita-obsidian/arquitectura.md`.
  - **Estado 14/10:** hecho el cambio (constantes `ORBITA_WS_MAX_MESSAGE_BYTES` 8192 para envío y `ORBITA_WS_MAX_CONTROL_BYTES` 512 para recibir; ahorro estimado ≈ 46 KB) y funciona en la placa. **Falta:** medir `ram_int` con el cambio (debería pasar de ≈ 80 a ≈ 125 KB) y cargar el valor medido en la tabla de `arquitectura.md` §1. También falta sumar a esa tabla el DMA de salida (≈ 4 KB) y la pila de `playback` (4 KB).
- [ ] **S4b Revisión de memoria** (a hablar; auditoría del 13/10 sobre el código, sin medir en la placa). Hoy hay ~80 KB de RAM interna libre con Wi-Fi + socket, sin AFE ni parlante.
  - **Mal alojado:** el módulo WebSocket pasa de 60 KB internos (cola RX 4 × 8,2 KB, `s_rx`, `s.reply`, buffer de la librería 8 KB, pila 6 KB, `packet` 4 KB). Es S4: más de la mitad de lo que queda.
  - **Bien alojado:** ring out (1,04 MB) y ring in (33 KB) en PSRAM; DMA del I2S (~15 KB) y buffers de captura/sink (~25 KB) en interna.
  - **Pulir:** (1) medir `ram_int` con parlante + AFE (si se activa) antes de elegir `ws://` o `wss://`: TLS suma decenas de KB internos; (2) pilas fijas en ~37 KB con `highwater` medido solo para el sink (queda 3,7 KB libres de 6): medir las demás; (3) `ram_int` bajó de 82 a 61 KB en 12 handshakes fallidos (~1,7 KB cada uno) y se recuperó: confirmar con un corte largo; (4) buffer de reproducción en PSRAM con tamaño que fija el Paso 0 (largo máximo de la respuesta).
- [ ] **Pruebas en la placa** (registro en `pruebas_socket.md`):
  2. Corte corto de Wi-Fi (~10 s) con la sesión en curso: reconecta, el servidor no cierra con error, hueco entre sesiones = 0.
  3. Corte largo (> 40 s): no salta el watchdog (el sink drena el atraso sin dormir, prio 5 > red 4) y el descarte del mensaje retenido aparece en el log.
- [ ] **S7 Parlante (lado hardware), sin red.** Amplificador I2S clase D (pines en `orbita-obsidian/orbita_pinout.md`; puede necesitar 5 V y picos de corriente: probar con fuente decente), módulo `orbita_playback` dueño del I2S de salida (la referencia del AEC futuro sale de ahí), ring en PSRAM de una respuesta máxima (480 KB), reproducción con prebuffer y contador de `underrun`, API del Paso 0. Se prueba con un tono o un clip de la partición `clips`. AEC y barge-in: Fase 2.
  - **Estado 14/10 (código hecho, compila, sin probar en la placa):** módulo `orbita_playback` completo: ring de bytes en PSRAM (480 KB), estados `LIBRE/LLENANDO/SONANDO/VACIANDO/DETENIENDO`, tarea de reproducción (core 0, prio 6, pila 4 KB, polling de 10 ms; la mejora con `xTaskNotifyGive` está anotada en `arquitectura.md` §2 y §3.7), I2S de salida en `I2S_NUM_1` (16 kHz, 16 bit, mono, DMA de 8 × 240 = 120 ms, `auto_clear`), `play_id` opaco y eventos solo desde la tarea, revisado con `embedded-reviewer`. Pines 7/15/16 (`orbita-obsidian/orbita_pinout.md`). **Actualizacion 15/10:** tono local validado; en `juani`, `main.c` llama una vez a `audio_ws_playback_init` antes del pipeline, que inicializa playback. Ganancia de Dante integrada; prueba por red pendiente.
  - **Para probar con el hardware (15/10), en orden:** (1) verificar el MAX98357A contra su datasheet antes de cablear: alimentación (3,3 V o 5 V), 16 kHz / 16 bit, función de `SD` y `GAIN`; (2) prueba corta en `main.c`, sin red: `orbita_playback_init` con los pines de `orbita_playback.h`, y un tono por `begin` / `push` / `end` (también respuesta corta, < 400 ms, y `stop` a mitad); (3) mirar el log de fin de respuesta (`underruns` y pila libre) y anotar la pila en la tabla de `arquitectura.md` §2; (4) confirmar que 15/16 (XTAL_32K) no molestan; (5) afinar el DMA (ver S9, "Para decidir").
  - **Límites conocidos:** tras un `stop` suena hasta 120 ms más (lo que ya está en el DMA; `disable`/`enable` no lo limpia, verificado en el driver); `DONE` espera 120 ms para coincidir con lo que se oye; si el ring se llena, `push` devuelve `ESP_ERR_NO_MEM` y la respuesta queda con un hueco (quien recibe el audio debe mandar `play_reject` y llamar a `stop()`).
- [ ] **S9 AFE con AEC y VAD (barge-in).** Va **después de S4 y S7** (RAM interna libre y referencia del parlante). Acordado 14/10: **AFE con VAD en modo 4, sin beamforming** (`se_init = false`, no actuaba); el AEC se agrega con el parlante andando. Pasos: (1) log al arranque con heap interno total y libre; medir `ram_int` con el AFE encendido y comprobar que acepta la config (log de `afe_config_check`); (2) AEC con referencia del parlante, probado con el parlante sonando; (3) VAD como disparador del `play_stop` propio; (4) con AFE, `ws://` en el alfa (TLS no entra). Requiere acordar con Juani el evento `barge_in` y la FSM.
  - **Problema de fondo:** sin AEC los micrófonos oyen al parlante y el VAD marca voz con la propia respuesta; el equipo se cortaría solo. El corte por voz depende del AEC.
  - **Qué existe hoy:** el AFE da un `voz` por bloque de 32 ms; llega a `on_afe(audio, n, voz)` en la tarea `fetch` (core 1) y al header del ring out (`ORBITA_FLAG_VOZ`/`VAD_OK`). Ahí se engancha el corte: `orbita_playback_stop()` es rápido y no bloquea.
  - **Criterio de corte (a diseñar y ajustar con datos, nunca un solo bloque):** contador con fuga (+1 por bloque con voz, −2 sin voz) y corte al pasar un umbral (≈ 0,5 a 1 s de voz sostenida), ignorando los primeros ~300 ms tras `STARTED`; evita cortar por fondo, un error del VAD o una reacción corta ("ajá"). Ideas a probar: bajar el volumen al primer indicio y cortar solo si se sostiene; sumar un umbral de energía contra el piso de ruido. Los umbrales van como parámetros (Kconfig).
  - **Latencia estimada del detector (sin medir):** 128 ms de voz mínima del VAD + 32 ms del `fetch` + 64 ms de la tubería ≈ 220 ms, más hasta 120 ms de DMA que siguen sonando tras el `stop`.
  - **AEC, para hablar (investigado 14/10 en headers de `esp-sr`; la doc web no dice nada de sincronización):** la referencia es el audio mandado al parlante, en el mismo formato y frame que el del micrófono (16 kHz, entrelazado, `"MMR"`); no hay parámetro de retardo: lo absorbe el filtro (`filter_length`, 4 recomendado para S3; más largo = más CPU); NLP agresivo por defecto. `afe_aec_*` solo usa 1 mic y 1 referencia. Dos caminos: (A) AEC adentro del AFE (`aec_init`, `"MMR"`; ~70 KB internos según Espressif), o (B) AEC suelto sobre un mic más VAD suelto (`esp_vad`), sin el AFE completo (menos RAM, a medir). **Abierto y sin verificar:** cuánto retardo tolera el filtro (el DMA de salida aporta 120 ms antes de que suene) y de dónde sale la referencia (`orbita_playback` no la expone todavía; el reloj I2S del parlante es distinto al de los mics).
  - **Filtro y alineación del AEC:** el AEC estima el camino parlante→mic con un filtro adaptativo y resta el eco calculado desde la referencia; solo cancela un eco cuyo retardo caiga **dentro de lo que cubre el filtro** (`filter_length`, 4 recomendado en S3; **la unidad no está verificada**: hipótesis, 4 frames de 32 ms ≈ 128 ms). Retardo referencia→mic en nuestro equipo: cola del DMA de salida (hasta 120 ms, domina) + amplificador/parlante (pocos ms, sin verificar) + acústica (≈ 1 ms a 30 cm). Arreglos, combinables: (1) achicar el DMA; (2) línea de retardo de la referencia del tamaño del DMA, de modo que el filtro cubra solo lo acústico. Alinear **contando muestras** (escritas al I2S vs. índice de captura), no con el reloj. Deriva entre los dos I2S: se supone de ppm si comparten cristal (sin verificar). Además: el filtro necesita converger (mantener su estado entre respuestas; idea: sonido de calibración al arrancar) y el NLP agresivo por defecto puede dañar la voz propia en doble habla.
  - **DMA y sincronización, en una línea:** el DMA de salida es un colchón anti-cortes (hoy 120 ms) que a la vez es **retardo** (lo que se escribe suena ≈ 120 ms después); como el AEC solo cancela ecos dentro de lo que cubre su filtro, se achica el DMA y/o se retrasa la referencia, midiendo `underruns` con Wi-Fi activo. El DMA solo cubre los tirones de la tarea (la red la absorben el ring y el prebuffer). Candidato a probar: 4 × 240 = 60 ms con trozos de escritura de 256 a 512 B (hoy 1024 B = 32 ms, que deja poco margen).
  - **Para decidir (15/10, con el parlante andando):** elegir (A) o (B) midiendo RAM interna y retardo; achicar el DMA de salida (hoy 120 ms: es retardo de la referencia y cola tras `stop`) hasta el mínimo con `underruns = 0` con Wi-Fi activo; alinear la referencia con una línea de retardo del tamaño del DMA contando muestras (no tiempo de reloj).
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
- [ ] **Recepción en el firmware:** integrada en `juani` con `orbita_playback`: controles JSON, buffer PLAY de 4 KB en PSRAM, mapeo de IDs y reportes asincronos `play_done` / `play_reject`. Pendiente validar en placa con `--replay-window 2 12` y acordar el contrato con el servidor final. API/pruebas: `firmware/components/orbita_ws/README.md`.
  - **Interfaz actualizada 14/10 (`orbita_playback.h`):** `begin(uint32_t play_id)` y callback `cb(ev, play_id, ctx)`; eventos `STARTED`, `DONE`, `STOPPED`, `UNDERRUN` ordenados desde la tarea de reproduccion. Cada begin OK termina en DONE o STOPPED. El puente `audio_ws_playback` asocia IDs uint64 del servidor con tokens uint32, detiene al desconectar o rechazar PCM y evita reportes duplicados. Pendiente medir prioridades (ws 4, sink 5, playback 6) y RAM; respuesta limitada a 15 s.
- [ ] **`ready` con `last_index`** y manejo del reenvío en el servidor (A6, la mitad de Juani).

## Paso 2 — Unir (otro día)

- [x] Integracion de `origin/dante` en `juani` (15/10): callback downlink conectado a `play_*`, ganancia Q15 de Dante y prueba de devolucion 2..12 s. Falta validacion en placa; `dante` conserva su version de banco.
- [ ] **S5 Prueba larga:** 1 h con Wi-Fi y cortes de 5/10/20 s: `dma_overflows` = 0, `ring_in_overruns` = 0, huecos solo los de los cortes. `ram_int` y `pila_sink` estables.
- [ ] Prueba de estrés del pipeline (sink lento, flash y Wi-Fi en paralelo) y máximos en `arquitectura.md` §3.7 (A15).

## Terminado cuando

Una hora seguida con el servidor transcribiendo, un corte de red de 20 s sin pérdida o con el hueco bien marcado, "Órbita" dicha al equipo y la respuesta sonando por el parlante, sin reiniciar.

## Producción (no bloquea el prototipo)

SSID/clave/URI en Kconfig → aprovisionamiento por NVS; `ws://` → `wss://` (CA, reloj, +RAM y +pila); `nvs_flash_init` sin el caso `NO_FREE_PAGES` (borrar la NVS destruiría las credenciales); warm-up fijo de 5 s propio del INMP441 (en la placa final conviene un pasa-altos); `arranque_fallo` en bucle (A10); sin reenvío desde el último `ack` (A6).
