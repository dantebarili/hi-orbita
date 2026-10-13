# Plan — Prototipo alfa (mono continuo por sockets + parlante)

Actualizado 2026-10-13. Deriva de `plan_dante.md` (captura/AFE/wake word). Detalle técnico del cable y del servidor: `INTEGRACION_WEBSOCKET.md`.

## Objetivo

El equipo manda **audio mono 16 kHz / 16-bit sin parar** al servidor. El **servidor** transcribe y detecta "Órbita": ahí corta el procesamiento de fondo y responde con un audio que el equipo reproduce (`audio_play`). **Sin wake word en el chip, sin AFE, sin VAD** (el VAD entra después como compuerta opcional). Fuera de alcance (sigue en `plan_dante.md`): wake word on-device, NS/beamforming, FSM `dev_wake_word*`. La FSM del vault no aplica a este prototipo: es "modo prototipo", no se actualiza.

## Protocolo `orbita-pcm-v2` (implementado; a validar con Juani)

Una conexión WebSocket larga, una sesión por conexión, sin `end`:

- ESP → servidor: `hello` (JSON) una vez; después binarios `ORBA` (header 40 B + hasta 4 slots = 128 ms de PCM), `index` creciente sin cortes. Los huecos se anotan (`lost`, `seq`), no se rellenan.
- Servidor → ESP: `ready` (JSON) y `ack` (último `index` recibido) cada ~1 s.
- **Falta definir (S6/S7):** `orbita` (se detectó la palabra), `play` (id, bytes), audio binario del parlante y `play_end`.
- Tras reconectar: sesión nueva (id nuevo); el servidor mide el hueco entre sesiones por `seq`. Todavía no hay reenvío desde el último `ack`.

## Estado

- [x] **S1 Retención.** Sin red el sink espera con el mensaje en la mano y el ring (32,8 s) acumula; si se llena descarta lo más viejo y lo marca. Si el ring pisó ≥ 1 s mientras se retenía, el mensaje retenido se descarta (no se manda un fragmento viejo aislado).
- [x] **S2 Reconexión.** Backoff 1-2-4-8 s, heartbeat 10 s / pong 3 s (valores de la FSM), tarea de red en core 0, un solo `hello` por conexión, captura antes que Wi-Fi (no espera IP). **Warm-up de 5 s** (arranque del mic y 21 overflows del DMA de arranque, informados como `dma_boot`).
- [x] **S3 Servidor continuo + firmware v2.** Segmentos de 30 s con su JSON de huecos, `ack`, hueco entre sesiones. Probado en la placa con una sesión de 48 s y un hueco de 27 s correctamente marcado. 22 pruebas automáticas.
- [x] Revisión `embedded-reviewer` (13/10) aplicada, salvo lo que está en S4 y en "Producción".
- [ ] **Pruebas en la placa** (registro y resultados en `pruebas_socket.md`; la 1 ya salió OK el 13/10):
  1. Corte corto de Wi-Fi (~10 s) con la sesión en curso: reconecta, el servidor no cierra con error, hueco entre sesiones = 0.
  2. Toma de 10 min: `ram_int` y `pila_sink` estables, `sin_ack` chico, `dma` = 0 después del warm-up.
  3. Corte largo (> 40 s): no salta el watchdog (el sink drena el atraso sin dormir, prio 5 > red 4) y el descarte del mensaje retenido aparece en el log.
- [ ] **S4 Presupuesto de RAM** (requisito de S7). `orbita_ws_message_t` lleva `data[8192]` y se copia 6 veces (cola RX de 4, `s_rx`, `reply`...): ≥ 50 KB internos para recibir un `ready` de ~50 B. Bajar el mensaje de control a ≤ 512 B, mover buffers grandes a PSRAM, medir RAM interna antes/después, anotar en `orbita-obsidian/plan-presupuesto-ram-cpu.md`.
- [ ] **S5 Prueba larga.** 1 h con Wi-Fi y cortes de 5/10/20 s: `dma_overflows` = 0, `ring_in_overruns` = 0, huecos solo los de los cortes.
- [ ] **S6 Transcripción + detección de "Órbita" en el servidor** (Juani). El equipo solo recibe el evento.
- [ ] **S7 Downlink y parlante.** Amplificador I2S clase D (pines con `pin-map-check`: fuera de GPIO 26–37, 19/20, 43/44; puede necesitar 5 V y picos de corriente), buffer de reproducción en PSRAM, recepción de binarios del servidor en buffer propio (no por la cola RX). AEC queda para después.
- [ ] **S8 VAD como compuerta (opcional).** El VAD del AFE en modo 4 anduvo bien (13/10; el 100 % de voz era del modo 0). Antes: anotar los números de esa prueba y reservar en el header un flag de "silencio salteado" (≠ pérdida).

## Producción (no bloquea el prototipo)

SSID/clave/URI en Kconfig → aprovisionamiento por NVS; `ws://` → `wss://` (CA, reloj, +RAM y +pila); `nvs_flash_init` sin el caso `NO_FREE_PAGES` (borrar la NVS destruiría las credenciales); warm-up fijo de 5 s propio del INMP441 (en la placa final conviene un pasa-altos); `arranque_fallo` en bucle (A10); sin reenvío desde el último `ack` (A6).

## Terminado cuando

Una hora seguida con el servidor transcribiendo, un corte de red de 20 s sin pérdida o con el hueco bien marcado, "Órbita" dicha al equipo y la respuesta sonando por el parlante, sin reiniciar.

## Por acordar con Juani

1. Mensajes del v2 (`hello`, `ready`, `ack` ya implementados; `orbita`, `play`, `play_end` por definir).
2. Reenvío tras reconexión: ¿alcanza marcar el hueco o hace falta reenviar desde el último `ack`?
3. Formato y largo máximo del audio de respuesta.
4. Mientras suena el parlante: ¿seguimos mandando el mic?
5. Diferencias con la FSM: backoff sin tope de intentos, sin timeout de 5 s de respuesta, sin eventos `srv_*`.
