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

## 1. Memoria

Estado: **medido** (en la placa) · **calculado** (del código, sin medir) · **estimado** (de otra fuente o supuesto).

### Flash

| Partición | Para qué | Offset | Tamaño |
|---|---|---|---|
| bootloader, tabla, `nvs`, `otadata`, `phy_init` | Fijos del chip; datos que sobreviven al reinicio; slot activo; calibración de radio | 0x0 | 36 + 24 + 8 + 4 KB |
| *(hueco)* | La app empieza en múltiplo de 64 KB | 0x12000 | 56 KB |
| `ota_0` / `ota_1` | App / siguiente actualización | 0x20000 / 0x320000 | 3 MB c/u |
| `model` | Modelos de `esp-sr`: **solo NS y VAD** (la wake word es un modelo propio `.tflite` y va en la app) | 0x620000 | 2 MB |
| `clips` | Audios locales de error para `audio_play` (1 MB ≈ 32 s a 16 kHz/16 bit mono) | 0x820000 | 1 MB |
| **TOTAL usado** | | 0x0 – 0x920000 | **9,125 MB** |
| **LIBRE** | Reserva (SD offline, logs) | 0x920000 – 0x1000000 | **6,875 MB** |

Modelos de `esp-sr` (medidos): NSNet1 813 KB, NSNet2 334 KB, NSNet3 113 KB, VADNet1 289 KB; lo máximo que entra son ~1,1 MB, así que 2 MB sobra.

Puntos abiertos:
- **NS y VAD hoy son WebRTC** (default de `esp-sr`, no una elección): algoritmo clásico, liviano, sin red neuronal; `model` queda vacía. NSNet/VADNet son redes neuronales de Espressif. **Pendiente:** en la Fase B se prueban los dos con las mismas tomas (CPU, RAM, latencia, calidad).
- Cambiar `sdkconfig.defaults` no actualiza un `sdkconfig` existente: borrarlo y reconfigurar. Flashear otra tabla borra el `nvs`.

### RAM interna (la escasa)

| Concepto | KB | Origen |
|---|---|---|
| **Libre hoy** (Wi-Fi + socket corriendo, sin AFE ni parlante) | **≈ 80** (74 a 84) | medido (`ram_int`, `pruebas_socket.md`, en el historial) |
| Heap total disponible al arrancar | sin medir | falta un log al arranque (ver abajo) |
| **Usado por Orbita** | | |
| `orbita_ws`: cola RX (4 × 8 KB) | 33 | calculado |
| `orbita_ws`: copias de un mensaje (`s_rx`, `reply`) | 16 | calculado (auditoría 13/10, sin releer) |
| `orbita_ws`: buffer de la librería + pila | 8 + 6 | calculado |
| Captura: `s_crudo_buf` (32 bit) + `s_chunk_buf` (16 bit) | 12 | calculado |
| DMA del I2S de entrada (8 × 240 frames × 8 B) | 15 | calculado |
| Sink: audio del mensaje + un slot | 5 | calculado |
| Adaptador del socket: `packet` + `control` | 4,4 | calculado |
| Pilas: `captura` 4, `feed` 4, `sink` 6, `orbita_wifi` 4, `app_main` 8 | 26 | asignado (§2) |
| **Total usado por Orbita** | **≈ 125** | calculado |

El resto (IDF, Wi-Fi, lwIP, TLS futuro) no está itemizado: es la diferencia entre el heap total y lo que hay libre más lo de Orbita. **Falta medir:** un log al arranque con `heap_caps_get_total_size(MALLOC_CAP_INTERNAL)` y el libre antes de iniciar los módulos. Con eso se cierra la tabla y se sabe cuánto come el sistema.

### PSRAM

| Concepto | MB | Origen |
|---|---|---|
| **Disponible** (N16R8) | **8** | `CLAUDE.md` |
| Ring out (1024 slots × 1040 B) | 1,04 | calculado (`diseño_ring_orbita.md`) |
| Ring in (8 chunks de 4 KB) | 0,03 | calculado |
| **Total usado hoy** | **≈ 1,07** | calculado |
| *Previsto:* ring out a 60 s (1875 slots) | +0,9 | estimado (diseño) |
| *Previsto:* buffer del parlante (una respuesta) | 0,48 | plan alfa (S7) |
| *Previsto:* AFE | 1,15 a 1,2 | estimado (benchmark de Espressif, ver abajo) |
| *Previsto:* wake word (modelo propio, microWakeWord) | sin medir | |
| **Total con todo lo previsto** | **≈ 3,6** (45 % de 8) | estimado |

La PSRAM no es el problema. Sí importa el orden de arranque: el ring out se reserva en un bloque contiguo **antes** de iniciar el AFE, porque `esp-sr` fragmenta la PSRAM.

### Qué pasa si se activa el AFE

Referencia: benchmark oficial de `esp-sr` para ESP32-S3 (2 mics, filas MMNR; leído de la web, no de memoria). Esa configuración incluye AEC, BSS, VAD vadnet y WakeNet; la nuestra es más liviana (solo VAD WebRTC, BSS que no actúa, WakeNet apagado), así que es un techo probable, **no una medición propia**.

| | RAM interna | PSRAM | CPU de un core |
|---|---|---|---|
| AFE 2 mics (MMNR, `SR`) | 68 a 79 KB | 1,15 a 1,2 MB | feed ≈ 24 %, fetch ≈ 23 % |
| Nuestro `feed` con AFE (medido) | — | — | 20 ms de cada 64 ms ≈ 31 % |

| Escenario | Libre en RAM interna |
|---|---|
| Hoy | ≈ 80 KB |
| Hoy + AFE (+ 4 KB de pila de `fetch`) | ≈ 0 a 8 KB: **no hay margen** |
| Con S4 (cola RX y copias de mensaje a ≤ 512 B, ahorro estimado ≈ 45 KB) | ≈ 125 KB |
| S4 + AFE | ≈ 40 a 50 KB |
| S4 + AFE + parlante + `wss://` | sin medir; TLS suma decenas de KB internos |

**Lectura:** el AFE consume casi toda la RAM interna que queda hoy; S4 es requisito para activarlo, y con AFE conviene `ws://` en el alfa. La PSRAM y la CPU alcanzan.

## 2. Tareas, cores y prioridades

Relevado el 14/10 sobre el código (`xTaskCreate*` en `components/` y `main/`). Prioridad mayor = más urgente; en un mismo core, igual prioridad se reparte el tiempo (tick de 1 ms). Las del sistema son los **defaults de ESP-IDF, sin verificar** (no hay IDF en esta PC; confirmar en menuconfig). Qué hace cada una con los datos: §3.1.

| Tarea | Qué hace | Core | Prio | Pila asignada | Pila en uso (máx.) |
|---|---|---|---|---|---|
| Wi-Fi (sistema) | Radio y eventos de red | 0 | 23 | — | — |
| `esp_timer`, event loop (sistema) | Timers y eventos | 0 | 22 / 20 | — | — |
| lwIP `tcpip_thread` (sistema) | Pila TCP/IP | 0 (fijado en `sdkconfig.defaults`) | 18 | — | — |
| `captura` | I2S → ring in | 1 | 6 | 4 KB | sin medir |
| `feed` | Ring in → AFE (o mono → ring out) | 1 | 5 | 4 KB | sin medir |
| `fetch` (solo con AFE) | Salida del AFE → ring out | 1 | 5 | 4 KB | sin medir |
| AFE interno (solo con AFE) | Procesa el audio | 0 | 5 | — | — |
| `sink` | Ring out → mensajes → `send` | 0 | 5 | 6 KB | ~2,7 KB (libre ≥ 3,4 KB; `pruebas_socket.md`, en el historial) |
| `orbita_ws` | Cliente WebSocket (librería) | 0 | 4 | 6 KB | sin medir |
| `ws_play_reports` | Reportes PLAY por WebSocket | 0 | 3 | 6 KB | sin medir |
| `orbita_wifi` | Conexión y reintentos | **sin fijar** | 3 | 4 KB | sin medir |
| `app_main` | Arranque | 0 | 1 | 8 KB | sin medir |
| `envio` (`test.c`, laboratorio) | WAV por UART0 | 0 | 5 | 4 KB | sin medir |
| `orbita_playback` (S7) | Ring de salida → I2S del parlante (validado con red el 15/10) | 0 | 6 | 4 KB | sin medir |

**REPARTO DE CORES:**
- core 0: main, wifi, envio (sink) y recepcion (parlante, sink)
- core 1: pipeline de audio

- **lwIP fijado al core 0** (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`, nombre sin verificar contra el IDF): con prio 18 > captura 6, si corría en el core 1 le robaba CPU a la captura en cada ráfaga TCP. Confirmar con Wi-Fi activo (A9) que el DMA no se desborda.
- **Prioridad de `orbita_ws` (4) vs. `sink` (5): a ver con Juani.** `orbita_ws` recibe los `PLAY` del parlante; hay que decidir si debe quedar por encima de quien envía. No se cambia sin él.
- **Parlante (propuesta):** core 0 con prio 6 (> sink, para que el drenaje no la deje sin datos). Consume poco CPU y el colchón del DMA de salida absorbe las ráfagas de Wi-Fi; el core 1 queda para el audio de entrada. Si luego se agrega AEC, la referencia debe quedar alineada con `feed`: se revisa entonces.
- **Volumen del parlante limitado por software (15/10):** `orbita_playback` multiplica todo el audio por `ORBITA_PLAY_GAIN_Q15` (hoy 8192 = 0,25 = −12 dB) antes del I2S, en `aplicar_ganancia()` ([orbita_playback.c](../firmware/components/orbita_playback/orbita_playback.c)); la constante está en `orbita_playback.h`. Motivo: proteger el parlante (4 Ω) y la fuente. El amplificador va a 5 V externos (desde el 3V3 del DevKit no sonaba, 15/10). Subir de a poco. Protección de hardware pendiente: pin `GAIN` y riel de 5 V en la PCB.
- **Mejorable (latencia de arranque del parlante):** en reposo `orbita_playback` mira el estado cada 10 ms (polling, 100 despertares por segundo): hasta 10 ms de demora al empezar a sonar. Alternativa: `xTaskNotifyGive` desde `push`/`end` y `ulTaskNotifyTake` con timeout (arranque inmediato, sin polling). Se dejó el polling por tener menos formas de fallar en silencio; con el prebuffer de 400 ms no se nota. Tiempos en §3.7.

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
| 5 | AFE (`esp-sr`), opcional | VAD (modo 4); audio mono. Sin beamforming: no actuaba (medido 12/10) y se sacó (14/10) | `fetch` = 512 muestras mono = 32 ms; 2 `fetch` por `feed`; latencia 64 ms |
| 6 | Wake word (modelo propio) | Consume todo el audio de `fetch`, con su propia ventana | 32 KB/s |
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
| Parlante: espera en reposo por polling (arranque de la reproducción) | ≤ 10 ms | prebuffer de 400 ms | diseño; **mejorable** (§2) |
| Parlante: prebuffer antes de sonar | 400 ms (12,8 KB) | — | config (Paso 0) |
| Parlante: trozo que saca la tarea del ring por vuelta | 32 ms (1 KB) | — | diseño |
| Parlante: respuesta máxima en el ring | 15 s (480 KB) | — | config (provisoria) |
| Sink: juntar un mensaje (4 slots) | 128 ms (reducible) | — | diseño |
| `send` de un mensaje | UART: 4 KB a ~92 KB/s ≈ 45 ms | < 128 ms promedio | cálculo |

**Mic → fin del `send`** (audio más viejo del mensaje): ≈ 128 + 10 + 45 ≈ **180 ms** con UART; con AFE, +64 ms o 84?. La latencia del urgente (§3.6) es otra cuenta.

### 3.2 Captura

La tarea `captura` lee el I2S (slots de 32 bit, 24 útiles en el INMP441), convierte a 16 bit con un shift fijo y escribe chunks en el ring in; `feed` los consume aparte, así el DMA nunca se atrasa. El tamaño del chunk lo fija el AFE y se lee en runtime.

- **Provisorio:** formato 16 kHz / 16 bit (falta el datasheet del ICS-43434) y shift fijo calibrado con INMP441 en DevKit (re-medir y reentrenar con el mic de producción).
- Toda pérdida (overflow del DMA, ring lleno) tiene que ser detectable: un hueco silencioso el modelo lo aprende como ambiente.

### 3.3 AFE y wake word

- **AFE (`esp-sr`):** solo VAD (NS, AGC y WakeNet apagados). Desactivable en compilación (`CONFIG_ORBITA_PIPELINE_USA_AFE`); **apagado por defecto; en el alfa se enciende para AEC y VAD** (`plan_orbita.md`, Línea 2). NS y beamforming siguen en pausa (decisión 11/10; `plan_dante.md` se borró el 19/10, queda en el historial de git: `git show 3da73c6:plan_dante.md`). El beamforming (BSS) no actuaba con 2 mics (medido 12/10) y se sacó el 14/10 (`se_init = false`). Sin BSS el AFE pide `feed` de 512 frames: el pipeline alimenta cada chunk de 1024 en 2 llamadas (15/10, compila; sin probar en placa con el AFE encendido); el VAD queda en modo 4 por defecto. El NS del AFE solo existe en `AFE_TYPE_VC`. La cifra "~1,2 MB de PSRAM y ~46 % de un núcleo" es estimación sin fuente.
- **Wake word "Órbita":** modelo propio entrenado con el pipeline de microWakeWord, con su propio búfer de ventana; consume todo el audio siempre. Si corre sobre audio crudo o sobre la salida del AFE se decide con datos (plan en `orbita-dataset/plan_wakeword.md`).
- **Mono:** al ring out va audio mono (salida del AFE, o canal L del crudo sin AFE; L o mezcla: a decidir con Comunicación). Se reduce lo más tarde posible, nunca antes del ring in.
- **AEC (diseño, 15/10; pasos y pendientes en `plan_orbita.md` Línea 2):**
  - **Cómo funciona:** el parlante reproduce `x(n)`; el mic oye la voz más el eco, `d(n) = voz + h * x(n)` (`h` = camino parlante → aire → mic). Un filtro FIR de `L` coeficientes `w` estima el eco `ŷ(n) = w * x(n)` a partir de la referencia, se resta (`e(n) = d(n) − ŷ(n)`) y `e(n)` ajusta `w` (tipo LMS/NLMS) para minimizar el residuo. Sigue un NLP (supresor del eco que queda) y el VAD. Todo ocurre **dentro** del AFE: `feed` solo entrega mic, mic, ref (`"MMR"`, referencia al final).
  - **`D` = retardo entre que `x(n)` se entrega como referencia y su eco llega al mic.** El filtro solo cancela ecos con retardo dentro de lo que cubre (`filter_length`; unidad **sin verificar**, hipótesis 4 × 32 ms). Si `D` cae fuera, el AEC no cancela nada. Se compensa con una línea de retardo `τ ≈ D − margen` en la referencia, contando **muestras**, no tiempo de reloj.
  - **De qué depende `D` (no es una constante de diseño):**

    | Componente | Valor hoy | Qué lo cambia |
    |---|---|---|
    | Cola del DMA de salida | hasta 120 ms (domina) | `PLAY_DMA_DESC_NUM × PLAY_DMA_FRAME_NUM`; tamaño de los trozos de escritura; **cuán llena esté la cola** (si la referencia sale al escribir, `D` varía entre ~0 y 120 ms) |
    | Ampli + parlante | pocos ms, sin verificar | modelo de ampli, filtros internos |
    | Acústica | ≈ 1 ms a 30 cm | distancia parlante → mic en la PCB |
    | Entrada del mic | no cuenta si se alinea por índice de muestra | solo si cambia el modo de alineación |

  - **Medición de `D` (pendiente):** reproducir un chirp con `orbita_playback`, capturar el mic crudo y anotar el índice de captura en que se escribió el primer trozo; la correlación cruzada da el retardo. **Repetir siempre que cambie** el DMA, el tamaño de trozo, el ruteo de la PCB o la versión de `esp-sr`/IDF. Valor medido: *sin medir*.
  - **A decidir:** AEC dentro del AFE (A) o suelto (B) midiendo RAM; `τ` fijo en Kconfig o calculado en ejecución; DMA mínimo con `underruns = 0`; quién expone la referencia (`orbita_playback` no lo hace aún); convergencia entre respuestas y doble habla (el NLP agresivo puede dañar la voz propia). Condiciona el ruteo del parlante en la PCB.
  - **Plan B si el AEC por software falla** (porque `D` no se puede fijar; decidir antes de la PCB): pasar a códec, con referencia por loopback de hardware (mismo reloj que los mics, `D` fijo). Combo: mics analógicos + ADC multicanal (ES7210) + DAC/ampli (ES8311 + NS4150B). Cambia la regla de mics digitales de `CLAUDE.md`/`AGENTS.md`. Referencia a mirar: ESP-Mosaico (ES8311 + NS4150B + mic analógico LMA2718B331-OE1; [guía](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html)). Hoy se sigue con mics digitales + MAX98357A (decisión 21/10).
- Pendientes (`vad_cache`, falsos negativos y positivos de la wake word): en el historial de git (`git show 3da73c6:plan_dante.md`).

### 3.4 Rings, header y tabla de segmentos

Movido a [[diseño_ring_orbita]].

### 3.5 Sink, mensajes y contrato con Comunicación

Movido a [[protocolo_comunicacion]].

## 4. Puertos serie y cableado de los mics

- **Dos puertos serie, uso separado:** UART0 (chip puente USB-UART del DevKit) solo para logs (`idf.py monitor`); USB-Serial-JTAG nativo libre para comandos y datos (`tools/orbita_serial.py`). Compartirlos hace que el driver se instale dos veces y falle (`ESP_ERR_INVALID_STATE`). Se logra con `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, `CONFIG_ESP_CONSOLE_UART_NUM=0` y `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` en `sdkconfig.defaults`. En Windows, cuál es cuál se ve en el Administrador de dispositivos desconectando un cable. En la PCB: chip puente o conector de programación, o USB nativo sin puente (decisión de BOM).
- **Cableado de los dos mics:** BCLK, WS y DIN (pines en `orbita_pinout.md`) van en paralelo a ambos; cada mic elige su slot con el pin `L/R` (a GND = canal L, a VDD = canal R, según el datasheet del INMP441/ICS-43434: verificar). Alimentación 3,3 V estricta, nunca 5 V.

## 5. Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA (ya hay dos slots).
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con los rings).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.

## 6. Backend (servidor de laboratorio, `tools/ws_audio_cloud.py`)

Hoy el servidor decide solo cuándo termina cada frase y la manda entera al STT (no streaming). Medido el 21/10 en tomas por WebSocket con el mic crudo (AFE apagado).

### 6.1 Detectar inicio y fin de la frase
- **Problema medido:** el 50 a 99 % de la energía del mic está bajo 100 Hz (rumble: mesa, tecleo, aire). Medido en banda ancha, la voz (RMS ~1000 a 1700) apenas supera el fondo (~500 a 800): SNR de 3 a 6 dB, y las sílabas suaves se tomaban por silencio ("Orbi" en vez de "Órbita").
- **Solución:** el nivel se mide en la **banda de voz (150 a 3400 Hz)**, con un Butterworth pasa-banda de 4.º orden por flanco (`scipy`, `sosfilt` con estado entre paquetes). Ahí el fondo queda en ~30 a 120 y la voz en 500 a 1900 (SNR > 20 dB). **El filtro solo mide: al STT va el audio crudo.**
- **Cómo decide** (bloques de 100 ms, RMS por bloque sobre la señal filtrada):
  - *Inicio:* 3 bloques seguidos sobre `max(2 × piso, 25)`; el piso es el percentil 20 de los últimos 20 s.
  - *Fin:* 1,2 s seguidos bajo un umbral de cierre (`max(2,5 × piso, 35)`), distinto del de inicio (histéresis) para que el ruido de fondo no mantenga la frase abierta y las sílabas suaves no la corten.
  - *Margen:* se agregan 0,5 s antes del inicio y 0,5 s después del último bloque con voz (el detector tarda 0,3 s en confirmar y las últimas sílabas son suaves). Son muestras que ya estaban en el buffer.
- **Después de la respuesta:** no escucha hasta que (a) el ESP manda `play_done` (el firmware lo manda cuando termina de sonar) y (b) el audio **recibido** tiene 0,5 s seguidos de silencio en banda, con tope de 8 s. Se mide el silencio recibido y no un tiempo fijo porque el audio del mic llega atrasado respecto del parlante (red y rings): la cola de su propia voz sigue llegando después de `play_done`.
- **Al conectar:** la ESP acumula hasta 32 s mientras no hay servidor (S1) y los manda de golpe. El servidor descarta lo que llega más rápido que su duración, y como mínimo 5 s de reloj.

### 6.2 Pasa-altos en la captura (hecho 29/10, rama `prototipo-alfa-wakeword`)
Butterworth de **2.º orden a 70 Hz**, un biquad por canal, en `tarea_captura` sobre el int32 del I2S y antes de bajar a 16 bit, con redondeo y saturación (antes era un corte `>>16`). Motivo: los mics traen continua de −121 a +147 LSB según la toma. Forma directa II traspuesta en `float`; coeficientes y datos en el cartel de `pipeline.c`.
- Respuesta: −22 dB a 20 Hz, −3 dB a 70 Hz, −0,9 dB a 100 Hz, 0 dB a 1 kHz. Se descartó la propuesta anterior (4.º orden a 150 Hz): le sacaba −14 dB a 100 Hz, la fundamental de una voz grave.
- Simulado en PC con la misma aritmética: continua de 147 LSB → 0,01; diferencia con `scipy` ≤ 1 LSB. Sin medir en placa.
- Lo ve todo lo que sale de la captura: AFE, wake word (su frontend arranca en 125 Hz: efecto mínimo, confirmar), socket y tomas UART del AGC/AEC.

### 6.3 Latencia de un turno (de que dejas de hablar a que suena la respuesta)
Medida el 21/10 con ElevenLabs (STT Scribe, TTS `eleven_flash_v2_5`) y Gemini en la copia de laboratorio: **mediana de 15 turnos con `gemini-3.5-flash-lite`**; el servidor la registra por etapa (`stt_s`, `llm_s`, `tts_s`, `send_s`, `total_s`) en `tools/prueba_websocket/cloud_*.jsonl`. `total_s` arranca cuando el detector cierra la frase, o sea que **no incluye** la espera de silencio ni lo que pasa en el ESP.

| # | Etapa | Dónde | Tiempo | Notas |
|---|---|---|---|---|
| 1 | Silencio para cerrar la frase | servidor | **1,2 s** fijos (+ hasta 0,1 s del bloque) | `END_QUIET_BLOCKS`. Es la etapa más fácil de recortar. |
| 2 | Audio del mic hasta el servidor | ESP + Wi-Fi | sin medir | Paquetes de hasta 128 ms + ring + red. Hace falta timestamp de envío (pendiente de Juani). |
| 3 | STT (la frase entera, no streaming) | ElevenLabs | **0,95 s** (0,6 a 1,5) | Crece con la duración de la frase. Deepgram Nova-3 midió parecido (~0,9 s, prueba anterior). |
| 4 | LLM | Gemini | **0,94 s** (0,8 a 2,4) | Con `gemini-3.5-flash` fue **9 s de mediana** (6 a 30 s, modelo saturado): no usarlo. `thinkingLevel: "minimal"`, sin razonamiento. |
| 5 | TTS (genera todo el audio antes de mandarlo) | ElevenLabs | **0,79 s** (0,6 a 0,9) | Sin streaming de audio. |
| 6 | Envío del PLAY | servidor a ESP | **0,16 s** (0 a 2,9) | Sube con la duración de la respuesta (16 kHz, 16 bit = 32 KB/s) y con el estado del Wi-Fi. |
| 7 | Prebuffer del parlante | ESP | **0,4 s** | `prebuffer_ms` acordado (S7). Más el DMA de salida de 120 ms. |
| | **Subtotal medido (3 a 6)** | | **≈ 2,9 s** (`total_s`) | |
| | **Total estimado (1 a 7)** | | **≈ 4,5 s + etapa 2** | 1,2 + 2,9 + 0,4. Sin medir de punta a punta con el ESP. |

Después de la respuesta, el servidor vuelve a escuchar a los ≈ 0,5 s de silencio recibido tras `play_done` (§6.1), más los 120 ms del DMA.

**Dónde se puede recortar (nada de esto está hecho):**
- *Silencio de cierre* de 1,2 s a ~0,8 s: ahorra ~0,4 s, a costa de más cortes entre palabras.
- *STT en streaming* con fin de frase del proveedor: elimina la etapa 1 y gran parte de la 3 (~1,5 s o más), y es lo que más pesa. Cuesta conexión abierta, reconexión y manejar texto parcial.
- *LLM y TTS en streaming por oración:* empezar a hablar con la primera frase mientras se genera el resto (~1 s).
- *Prebuffer* de 0,4 s a 0,2 s: ahorra 0,2 s, con más riesgo de `underrun`.
- *Modelo del LLM:* la elección pesa más que cualquier ajuste: de 0,9 s a 9 s según el modelo y su carga. Medir siempre antes de cambiar.

