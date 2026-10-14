# Arquitectura — decisiones que afectan al producto final

> **A futuro (diseño de la PCB):** probar los MCP de KiCad (`mixelpixx/KiCAD-MCP-Server`, el más maduro, pide KiCad 9+) y de partes JLCPCB/LCSC (`jlc-mcp`) para revisar esquemáticos y elegir componentes. Son comunitarios e inmaduros: usarlos como segunda opinión, nunca como verificación; los pines y tensiones se confirman a mano contra el datasheet. Detalle en `reports/Herramientas Claude para firmware embebido.md`.

FSM: `órbita..md`. Hardware y pines: `CLAUDE.md`.

Estado: **Cerrada** · **Provisoria** (vale hasta medir/verificar) · **Propuesta** (sin validar) · **Abierta**.

### Vocabulario (de menor a mayor)

| Nombre | Qué es | Tamaño |
|---|---|---|
| Muestra | Un número de un mic | 2 bytes (16 bit) |
| Frame de I2S | Un par L+R | 4 bytes (8 B en crudo de 32 bit) |
| **Chunk** | Lo que lee la captura y consume `feed` (ring in); es la entrada del AFE | 1024 frames estéreo = 64 ms (4 KB). Lo fija el AFE (`feed` pide 1024 muestras/canal; medido en el chip) |
| **Slot** | Unidad del ring out: header de 16 B + 512 muestras mono | 1040 B, 32 ms. Mide lo mismo que un `fetch` del AFE, pero **no depende del AFE**: con AFE lo llena `fetch`; sin AFE, `feed` reduce el chunk a mono y lo parte en 2 slots |
| **Ring in** / **Ring out** | Los dos rings del pipeline. *In*: captura → `feed`, chunks estéreo crudos. *Out*: escritor → sink, slots mono listos para enviar (con o sin AFE) | Ver 3.4 |
| **Mensaje** | Lo que el sink escribe al socket de una vez; un segmento viaja en varios | ~100 ms ≈ 4 slots (a definir con Comunicación) |
| **Segmento** | Lo que se manda al backend entre un `dev_fin_segmento` y el siguiente (unidad de la FSM) | 1–2 s, provisorio |

---

## 1. Memoria, chip: `sdkconfig.defaults` y `partitions.csv`

ACTUALIZAR

Hecho el 2026-10-10 (A0). `sdkconfig` está en `.gitignore`: lo que vale para el equipo es `sdkconfig.defaults`.

| Decisión | Estado | Consecuencia |
|---|---|---|
| Flash 16 MB | Cerrada | Es lo del N16R8; la flash de la PCB debe ser de 16 MB. Declarar más que la real: no arranca. |
| Flash en QIO a 80 MHz | Provisoria | Antes DIO (default de IDF); se apagó `FLASH_MODE_AUTO_DETECT`. El header de la imagen sigue diciendo `dio` (normal en S3). **Confirmar en el chip:** `SPI Mode : QIO` en el log; si no arranca, volver a DIO. |
| Tabla de particiones propia | Cerrada | Lo más caro de cambiar: un equipo instalado no mueve sus particiones por OTA. Por eso se reservó de más. |
| Dos slots de app de 3 MB (`ota_0`, `ota_1`) | Propuesta | Si el OTA falla vuelve al slot anterior; con uno solo, un corte de luz puede dejar el equipo muerto. La app pesa 240 KB; con `esp-sr`, WiFi, TLS y Edge Impulse podría llegar a 1,5–2 MB (estimado). |
| CPU a 240 MHz | Cerrada | Más corriente y calor que a 160; lo medido de CPU vale solo a esta frecuencia. |
| Tick de FreeRTOS a 1 ms | Cerrada | Antes 10 ms. Un `vTaskDelay(10)` en ticks crudos espera 10 veces menos; `pdMS_TO_TICKS` se adapta. |
| Stack de `app_main` 8192 B | Provisoria | RAM interna (la escasa); medir cada tarea con `uxTaskGetStackHighWaterMark`. |

### Mapa de la flash

| Partición | Para qué | Offset | Tamaño |
|---|---|---|---|
| bootloader, tabla, `nvs`, `otadata`, `phy_init` | Fijos del chip; datos que sobreviven al reinicio; slot activo; calibración de radio | 0x0 | 36 + 24 + 8 + 4 KB |
| *(hueco)* | La app empieza en múltiplo de 64 KB | 0x12000 | 56 KB |
| `ota_0` / `ota_1` | App / siguiente actualización | 0x20000 / 0x320000 | 3 MB c/u |
| `model` | Modelos de `esp-sr`: **solo NS y VAD** (la wake word es de Edge Impulse y va en la app) | 0x620000 | 2 MB |
| `clips` | Audios locales de error para `audio_play` (1 MB ≈ 32 s a 16 kHz/16 bit mono) | 0x820000 | 1 MB |
| **TOTAL usado** | | 0x0 – 0x920000 | **9,125 MB** |
| **LIBRE** | Reserva (SD offline, logs) | 0x920000 – 0x1000000 | **6,875 MB** |

Modelos de `esp-sr` (medidos): NSNet1 813 KB, NSNet2 334 KB, NSNet3 113 KB, VADNet1 289 KB; lo máximo que entra son ~1,1 MB, así que 2 MB sobra.

Puntos abiertos:
- **NS y VAD hoy son WebRTC** (default de `esp-sr`, no una elección): algoritmo clásico, liviano, sin red neuronal; `model` queda vacía. NSNet/VADNet son redes neuronales de Espressif. **Pendiente:** en la Fase B se prueban los dos con las mismas tomas (CPU, RAM, latencia, calidad).
- Cambiar `sdkconfig.defaults` no actualiza un `sdkconfig` existente: borrarlo y reconfigurar. Flashear otra tabla borra el `nvs`.

## 2. Tareas, cores y prioridades

Relevado el 14/10 sobre el código (`xTaskCreate*` en `components/` y `main/`). Prioridad mayor = más urgente; en un mismo core, igual prioridad se reparte el tiempo (tick de 1 ms). Las del sistema son los **defaults de ESP-IDF, sin verificar** (no hay IDF en esta PC; confirmar en menuconfig). Qué hace cada una con los datos: §3.1.

| Tarea | Qué hace | Core | Prio | Pila |
|---|---|---|---|---|
| Wi-Fi (sistema) | Radio y eventos de red | 0 | 23 | — |
| `esp_timer`, event loop (sistema) | Timers y eventos | 0 | 22 / 20 | — |
| lwIP `tcpip_thread` (sistema) | Pila TCP/IP | 0 (fijado en `sdkconfig.defaults`) | 18 | — |
| `captura` | I2S → ring in | 1 | 6 | 4 KB |
| `feed` | Ring in → AFE (o mono → ring out) | 1 | 5 | 4 KB |
| `fetch` (solo con AFE) | Salida del AFE → ring out | 1 | 5 | 4 KB |
| AFE interno (solo con AFE) | Procesa el audio | 0 | 5 | — |
| `sink` | Ring out → mensajes → `send` | 0 | 5 | 6 KB |
| `orbita_ws` | Cliente WebSocket (librería) | **sin fijar** | 4 | 6 KB |
| `orbita_wifi` | Conexión y reintentos | **sin fijar** | 3 | 4 KB |
| `app_main` | Arranque | 0 | 1 | 8 KB |
| `envio` (`test.c`, laboratorio) | WAV por UART0 | 0 | 5 | 4 KB |
| `orbita_playback` (S7) | Ring de salida → I2S del parlante | 0 | 6 | — |

**REPARTO DE CORES:**
- core 0: main, wifi, envio (sink) y recepcion (parlante, sink)
- core 1: pipeline de audio

- **lwIP fijado al core 0** (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`, nombre sin verificar contra el IDF): con prio 18 > captura 6, si corría en el core 1 le robaba CPU a la captura en cada ráfaga TCP. Confirmar con Wi-Fi activo (A9) que el DMA no se desborda.
- **`sink` (5) > `orbita_ws` (4):** al drenar atraso el sink no duerme y puede demorar el heartbeat/pong del cliente WebSocket si este queda en su mismo core. Verificar con el corte largo; el remedio es un `vTaskDelay(1)` cada N mensajes o subir `orbita_ws` a 5.
- **Parlante (propuesta):** core 0 con prio 6 (> sink, para que el drenaje no la deje sin datos). Consume poco CPU y el colchón del DMA de salida absorbe las ráfagas de Wi-Fi; el core 1 queda para el audio de entrada. Si luego se agrega AEC, la referencia debe quedar alineada con `feed`: se revisa entonces.

## 3. DSP y pipeline de audio

### 3.1 Flujo y datos en cada punto

```
                                                    ┌ AFE off: mono (L) en 2 slots ────────────────┐
mics ─▶ I2S/DMA ─▶ captura ─▶ RING IN ─▶ tarea feed ─┤                                              ├▶ RING OUT ─▶ sink ─▶ WebSocket ─▶ backend
                   (32→16)    (~0,5 s)              └ AFE on: AFE ─▶ tarea fetch ─┬────────────────┘   (~60 s)
                                                                                   └▶ wake word (siempre)
```

| # | Punto | Qué hace | Formato |
|---|---|---|---|
| 1 | Mics + I2S/DMA | Audio digital estéreo (INMP441 / ICS-43434, L y R) | 24 bit útiles en slot de 32; 16 kHz; BCLK 1,024 MHz; DMA 8 × 240 frames ≈ 120 ms (RAM interna) |
| 2 | `captura` | Convierte 32→16 bit (shift fijo) y arma chunks | Estéreo 16 bit; chunk de 1024 frames = 4 KB (64 ms) |
| 3 | Ring in | Cola siempre activa, con o sin AFE | 8 chunks = 32 KB (~0,5 s), PSRAM |
| 4 | `feed` | Con AFE: intercala los canales (mic, mic; con AEC, también la referencia) y alimenta al AFE. Sin AFE: reduce a mono (L por ahora) y parte el chunk en 2 bloques de 512 | int16 a 16 kHz |
| 5 | AFE (`esp-sr`), opcional | VAD; audio mono. El beamforming (BSS) no actúa (medido 12/10) | `fetch` = 512 muestras mono = 32 ms; 2 `fetch` por `feed`; latencia 64 ms |
| 6 | Wake word (Edge Impulse) | Consume todo el audio de `fetch`, con su propia ventana | 32 KB/s |
| 7 | Ring out | Slots (header de 16 B + 512 muestras mono) y tabla de segmentos | 1040 B por slot; 60 s ≈ 1,9 MB en PSRAM |
| 8 | `sink` | Saca slots, los junta en mensajes de 4 slots y los pasa al `send` registrado | ≈ 4 KB por mensaje; ≈ 270 kbit/s con TLS (estimado) |

- **Test:** WAV de 2 canales (ch0 mic crudo L, ch1 salida del AFE) = 64 KB/s por UART a 921600 baud (~1,44× el audio); 3 canales no entran. El ch1 llega 64 ms después del ch0 (se corrige en el script con `--alinear`).

### 3.7 Tiempos y latencia del pipeline

Sin AFE salvo que se diga. *Medido* = máximo en una toma de 20 s en el chip (13/10), en reposo: sin WiFi, sin escrituras a flash. *Estimado* = sin medir. A15 imprime los máximos en la línea `Fin:`.

| Tramo | Tiempo | Límite | Origen |
|---|---|---|---|
| Descriptor del DMA (240 frames) | 15 ms | colchón total del DMA: 8 × 15 = 120 ms (antes 90) | config |
| Captura: esperar un chunk (1024 frames) | 64 ms | = ritmo del sistema | config |
| Captura: trabajo por chunk (pack + ring in + callback) | **444 µs** (`on_crudo` 209, envío al ring in 124) | 56 ms (120 − 64) | medido |
| Ring in → `feed` | máx. 1 chunk esperando | colchón de 8 chunks = 512 ms | medido |
| `feed` sin AFE (mono + 2 slots) | **189 µs** | 64 ms | medido |
| `feed` con AFE | ~20 ms | 64 ms | medido |
| Latencia de la tubería del AFE | 64 ms | — | medido |
| `ring_out_write` / `read` (interrupciones apagadas en ese core) | **62 µs** / **5 µs** | ≪ 15 ms | medido |
| Sink: espera por polling | ≤ 10 ms | — | diseño |
| Sink: juntar un mensaje (4 slots) | 128 ms (reducible) | — | diseño |
| `send` de un mensaje | UART: 4 KB a ~92 KB/s ≈ 45 ms | < 128 ms promedio | cálculo |

**Mic → fin del `send`** (audio más viejo del mensaje): ≈ 128 + 10 + 45 ≈ **180 ms** con UART; con AFE, +64 ms o 84?. La latencia del urgente (§3.6) es otra cuenta.

### 3.2 Captura

La tarea `captura` lee el I2S (slots de 32 bit, 24 útiles en el INMP441), convierte a 16 bit con un shift fijo y escribe chunks en el ring in; `feed` los consume aparte, así el DMA nunca se atrasa. El tamaño del chunk lo fija el AFE y se lee en runtime.

- **Provisorio:** formato 16 kHz / 16 bit (falta el datasheet del ICS-43434) y shift fijo calibrado con INMP441 en DevKit (re-medir y reentrenar con el mic de producción).
- Toda pérdida (overflow del DMA, ring lleno) tiene que ser detectable: un hueco silencioso el modelo lo aprende como ambiente.

### 3.3 AFE y wake word

- **AFE (`esp-sr`):** solo VAD (NS, AGC y WakeNet apagados). Desactivable en compilación (`CONFIG_ORBITA_PIPELINE_USA_AFE`); **hoy apagado y en pausa** (decisión 11/10, `plan_dante.md`). Con 2 mics el BSS no actúa (medido 12/10) y el NS del AFE solo existe en `AFE_TYPE_VC`. La cifra "~1,2 MB de PSRAM y ~46 % de un núcleo" es estimación sin fuente.
- **Wake word "Órbita":** modelo propio de Edge Impulse, con su propio búfer de ventana; consume todo el audio siempre. Si corre sobre audio crudo o sobre la salida del AFE se decide con datos (`plan_dante.md`).
- **Mono:** al ring out va audio mono (salida del AFE, o canal L del crudo sin AFE; L o mezcla: a decidir con Comunicación). Se reduce lo más tarde posible, nunca antes del ring in.
- **AEC (riesgo):** `feed` pide la referencia del parlante al final (mic, mic, ref). Condiciona el ruteo del parlante en la PCB. No diseñar todavía.
- Pendientes (`vad_cache`, falsos negativos y positivos de la wake word): en `plan_dante.md`.

### 3.4 Rings, header y tabla de segmentos

Movido a [[diseño_ring_orbita]].

### 3.5 Sink, mensajes y contrato con Comunicación

Movido a [[protocolo_comunicacion]].

## 4. Puertos serie y cableado de los mics

- **Dos puertos serie, uso separado:** UART0 (chip puente USB-UART del DevKit) solo para logs (`idf.py monitor`); USB-Serial-JTAG nativo (GPIO 19/20) libre para comandos y datos (`tools/orbita_serial.py`). Compartirlos hace que el driver se instale dos veces y falle (`ESP_ERR_INVALID_STATE`). Se logra con `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, `CONFIG_ESP_CONSOLE_UART_NUM=0` y `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` en `sdkconfig.defaults`. En Windows, cuál es cuál se ve en el Administrador de dispositivos desconectando un cable. En la PCB: chip puente o conector de programación, o USB nativo sin puente (decisión de BOM).
- **Cableado de los dos mics:** BCLK (GPIO 4), WS (GPIO 5) y DIN (GPIO 6) van en paralelo a ambos; cada mic elige su slot con el pin `L/R` (a GND = canal L, a VDD = canal R, según el datasheet del INMP441/ICS-43434: verificar). Alimentación 3,3 V estricta, nunca 5 V.

## 5. Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA (ya hay dos slots).
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con los rings).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.

