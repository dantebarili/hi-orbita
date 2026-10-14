# Arquitectura — decisiones que afectan al producto final

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

PSRAM? RAM?¿

## 3. DSP y pipeline de audio

### 3.1 Flujo y datos en cada punto

```
                                                    ┌ AFE off: mono (L) en 2 slots ────────────────┐
mics ─▶ I2S/DMA ─▶ captura ─▶ RING IN ─▶ tarea feed ─┤                                              ├▶ RING OUT ─▶ sink ─▶ WebSocket ─▶ backend
                   (32→16)    (~0,5 s)              └ AFE on: AFE ─▶ tarea fetch ─┬────────────────┘   (~60 s)
                                                                                   └▶ wake word (siempre)
```

| # | Punto | Qué dato hay | Formato / tamaño / flujo | Corre en |
|---|---|---|---|---|
| 1 | Mics (INMP441 / ICS-43434, L y R) | Audio digital I2S | 24 bit útiles en slot de 32; 16 kHz; BCLK 1,024 MHz | hardware |
| 2 | I2S + DMA | Frames estéreo crudos | 8 B por frame, 128 KB/s; buffer DMA 8 × 240 frames ≈ 120 ms (RAM interna; ISR en IRAM) | hardware; la interrupción queda en el core desde donde se inicializó el driver (hoy core 0, de `app_main`; sin verificar qué función la reserva) |
| 3 | Tarea de captura | Convierte 32→16 bit (shift fijo), arma chunks | Estéreo 16 bit, 64 KB/s; chunk de 1024 frames = 4 KB (64 ms, el de `feed`) | **core 1, prioridad 6** (A2) |
| 4 | Ring in (siempre activo, con o sin AFE) | Chunks de entrada, sin header | 8 chunks de 4 KB = 32 KB (~0,5 s) | PSRAM (explícito) |
| 5 | Tarea feed | Con AFE: canales intercalados (mic, mic; con AEC, también la referencia) al AFE. Sin AFE: reduce a mono (L por ahora), parte el chunk en 2 bloques de 512 y los escribe al ring out | int16 a 16 kHz, 64 KB/s | **core 1, prioridad 5** |
| 6 | AFE (`esp-sr`), opcional | VAD (sin NS con 2 mics; AGC y WakeNet apagados): audio **mono**, `vad_state`, `raw_data`. **El beamforming (BSS) no actúa** (medido 12/10 con todas las combinaciones de canal fijo y disparo por VAD: la salida es el mic 0 idéntico) | `fetch` (`get_fetch_chunksize`) = 512 muestras mono = 32 ms (1 KB), 32 KB/s; 2 `fetch` por cada `feed`; latencia de la tubería 64 ms | tareas internas del AFE: **core 0, prioridad 5** (`afe_perferred_core`, medido); `tarea_fetch` en core 1 |
| 7 | Wake word (Edge Impulse) | Consume **todo** el audio de `fetch`, con su propia ventana | 32 KB/s | core 1 (propuesta); si no entra con el AFE, pasa al core 0 |
| 8 | Ring out | Slots (header de 16 B + 512 muestras mono) y la tabla de segmentos | 1040 B por slot; 60 s ≈ 1875 slots ≈ 1,9 MB en PSRAM | memoria |
| 9 | Sink | Saca slots del ring out, los junta en mensajes de 4 slots y los pasa al `send` registrado. Elegir segmentos: A6 (v1: en orden de llegada) | Mensaje = 4 slots ≈ 4 KB; ≈ 270 kbit/s con TLS (estimado) | **core 0, prioridad 5** (`audio_sink.c`) |
| 10 | Backend | Audio mono 16 kHz / 16 bit por segmento | — | — |

- **Reparto de cores (propuesta):** core 0 = red y control (WiFi va fijo al core 0 por defecto; `esp_timer`, `app_main`, sink, TLS); core 1 = audio en tiempo real. Motivo: las ráfagas de WiFi/TLS no deben quitarle CPU a la captura (el DMA da ~90 ms de margen). Riesgo: AFE (~46 % de un núcleo, estimado) + wake word + captura podrían no entrar en el core 1: medir en la Fase B. La tarea de lwIP no tiene core fijo.
- **Hoy:** I2S → captura (core 1) → ring in → `feed` (core 1) → [AFE → `fetch`] → ring out → tarea sink (core 0) → `send` del sink registrado (hoy `test_sink`, que solo cuenta). En paralelo, la prueba de test (`test.c`) copia a sus propios rings y `tarea_envio` (core 0) los manda por UART0 sin pasar por el ring out.
- **Test:** WAV de 2 canales (ch0 mic crudo L, ch1 salida del AFE) = 64 KB/s por UART a 921600 baud (~1,44× el audio); 3 canales no entran. El ch1 llega 64 ms después del ch0 (se corrige en el script con `--alinear`).

### 3.2 Captura (I2S → tarea de captura)

| Decisión | Estado | Nota |
|---|---|---|
| Formato 16 kHz / 16 bit en todo el sistema | Provisoria | Falta el datasheet del ICS-43434 (muestreo y rango de reloj, BCLK 1,024 MHz). Si no lo cubre, cambia el mic o el formato. |
| Captura en tarea propia, de mayor prioridad que el AFE | Cerrada | Hecho en A2 y probado (core 1, prioridad 6). Reparto de cores en 3.1; el AFE interno quedó en core 0 por defecto: se confirma con CPU medida en la Fase B. |
| La captura convierte 32→16 bit y escribe en el ring in; otra tarea hace `feed` | Cerrada | Así el DMA nunca se atrasa. |
| Chunk de captura = chunk de entrada del AFE, leído en runtime | Cerrada | No hardcodear: lo fija la librería. |
| Ganancia/shift de captura fijos en firmware | Provisoria | Calibrado con INMP441 en DevKit; con el mic, puerto acústico y carcasa de producción hay que re-medir y reentrenar. |
| Pérdida de audio siempre detectable (overflows del DMA + ring lleno) | Cerrada | Un hueco silencioso el modelo lo aprende como ambiente. |

El INMP441 entrega 32 bits por slot pero solo 24 son útiles.

### 3.3 AFE y wake word

| Decisión | Estado | Nota |
|---|---|---|
| AFE (`esp-sr`) con 2 mics: solo VAD; NS, AGC y WakeNet apagados. **Desactivable en compilación** (`CONFIG_ORBITA_PIPELINE_USA_AFE`); hoy se trabaja con AFE apagado | Provisoria | Con 2 mics el beamforming desplaza al NS, y el BSS no actúa (medido): beamforming del AFE descartado; si hace falta, `mase_process` u offline en la PC. El NS del AFE solo existe en `AFE_TYPE_VC`. "~1,2 MB PSRAM y ~46 % de un núcleo" es estimación sin fuente. |
| Wake word "Órbita" con modelo propio de Edge Impulse | Provisoria | Corre sobre la salida del AFE. Falta validar que entrenamiento y despliegue coincidan. |
| Al ring out va audio **mono**: la salida del AFE, o el canal L del crudo sin AFE (L o mezcla: a decidir con Comunicación) | Cerrada | Se reduce a mono lo más tarde posible, nunca antes del ring in. |
| Entrada del AFE con AEC | Riesgo | `feed` pide la referencia del parlante al final (mic, mic, ref). Condiciona el ruteo del parlante en la PCB. No diseñar todavía. |

Puntos abiertos:
- La wake word necesita su propio búfer de ventana (Edge Impulse): consume todo el audio siempre.
- `vad_cache`: cuando el VAD recorta el inicio del habla, `fetch` entrega ese cache; si el ring out lo ignora, se pierde el comienzo de la frase.
- Para el WAV de test: `afe_fetch_result_t` trae `raw_data`; verificar que sea el crudo y esté alineado.
- **Falsos negativos y positivos de la wake word (hay que lidiar con los dos):** *negativo*: el backend, que recibe todo el audio de la consulta, debe poder detectar un "Órbita" que el ESP32 perdió y avisar (evento `srv_` nuevo; el ring out conserva el audio hasta la confirmación). *Positivo*: el backend debe poder descartar un urgente de más y devolver al equipo al resumen (otro `srv_`). Métricas: FRR y falsas activaciones por hora.

### 3.4 Rings, header y tabla de segmentos

**Políticas (A5, 2026-10-10)**

| Tema | Decisión |
|---|---|
| Qué se guarda | Siempre, también en `server_back` y `audio_play`: sin huecos en la historia clínica. Se puede achicar a ~40 s dejando de grabar en `audio_play` (cuesta huecos). |
| Reintentos | El segmento se retiene hasta `srv_respuesta_recibida` (~11 s con 2 intentos de 5 s + backoff de 1 s). Si se agotan, la FSM lo descarta y queda anotado el hueco. |
| Pre-roll | ~1 s (≈31 slots) antes de la wake word: se detecta cuando "Órbita" ya terminó, y sin pre-roll el pedido llegaría cortado. El backend también recibe el "Órbita" y puede verificarlo. Se ajusta al medir. |
| Ring lleno | Se descarta el segmento pendiente más viejo; el hueco queda en la tabla. |

**Los dos rings**

| | Ring in | Ring out |
|---|---|---|
| Une | captura → tarea `feed` | `feed` (sin AFE) o `fetch` (con AFE) → sink. Una tarea de prioridad 5 llama siempre al mismo callback `on_audio`; nunca la captura (propuesta A5) |
| Contiene | chunks de entrada (estéreo, 16 bit, 4 KB), sin header | slots mono con header de 16 B |
| Tamaño | 8 chunks = 32 KB (~0,5 s) | ~60 s ≈ 1,9 MB en PSRAM (segmento de 2 s + reconexión de WiFi ≈ 35 s; falta sumar el largo máximo de `audio_play`) |
| Para qué | Desacopla la captura (prioridad 6) de todo lo que sigue, con o sin AFE: si `feed` tarda o bloquea, el I2S igual se lee. Queda siempre. | Retención, reintentos, pre-roll |
| Estructura | Ring de FreeRTOS (`NOSPLIT`, un chunk por ítem; se protege solo) | Ring de slots propio + tabla de segmentos (el de FreeRTOS no permite releer, ni pre-roll, ni mandar primero el urgente) |

**Header del slot (solo ring out, 16 bytes; definido en `orbita_audio/include/audio_ring_out.h`).** Guarda lo que se sabe al escribir el slot. Es **interno**: no viaja al backend; el sink arma con él el header del cable de cada mensaje.

| Campo | Tipo | Qué hace |
|---|---|---|
| `seq` | u32 | Nº de slot, sube de a 1. Comparar con `(int32_t)(a - b)` (vale al dar la vuelta, ~4,4 años a 32 ms). Se reinicia en cada arranque: el backend necesita además un id de sesión. |
| `lost_samples` | u32 | Muestras perdidas justo antes de este slot (0 = ninguna). Reconstruye el tiempo y alinea el WAV de test; no se rellena con ceros. |
| `n_samples` | u16 | Muestras de audio válidas en el slot, mono (el `vad_cache` puede traer otro tamaño). |
| `flags` | u8 | bit 0 `VAD_OK` (el VAD midió este bloque; sin AFE no hay VAD), bit 1 `VOZ` (detectó voz; solo vale con `VAD_OK`). Ambos en 0 = sin VAD: **compuerta abierta**, se manda igual. |
| `version` | u8 | Versión del formato. |
| (reservado) | u32 | Futuro. |

- `lost_samples` suma lectura fallida, overflow del DMA (A1), ring in lleno, chunk inválido y, con AFE, `fetch` inválido (la deriva del AFE no se mide): contadores que la tarea que escribe el slot (`feed` o `fetch`) lee, pone en 0 y copia (aproximado con AFE por su latencia).
- No lleva inicio/fin de segmento ni "urgente": se deciden después de guardar el slot y los pre-roll se solapan; eso vive en la tabla.
- Sin relleno: `_Static_assert(sizeof == 16)`, sin `__attribute__((packed))`. Costo en PSRAM despreciable aunque el slot baje a 10 ms (6000 slots, 2 MB).
- Hecho (13/10): formato del slot (`audio_ring_out.h`, 1040 B con `_Static_assert`), ring out v1 (`audio_ring_out.c`), productor en `pipeline.c` y tarea sink. Compila con y sin AFE; sin probar en el chip. Falta (A6): tabla de segmentos y header del cable.

**Ring out v1 (A5, 2026-10-13)**

| Tema | Decisión |
|---|---|
| Estructura | Cola FIFO circular de `ORBITA_RING_SLOTS` (1024 = 32,8 s, 1,04 MB) slots en PSRAM (`heap_caps_calloc`). Dos índices (`write_idx`, `read_idx`) y `ocupados`; el índice avanza con `(i+1) % N`. Sin confirmación ni pre-roll: leer **saca** el slot. El `max_ocupados` de las stats dice cuánto hace falta. |
| Protección | Spinlock (`portMUX`, interrupciones apagadas ~20–50 µs) sobre índices, contadores y el `memcpy`. Sin mutex (revisión 13/10): con prio 5 en core 0, WiFi/TLS podían desalojar al sink con el mutex tomado y el productor esperar o perder el slot. `write` siempre entra, sin espera ni timeout. Nunca en la ISR ni durante el `send` de red. A medir: duración real con PSRAM y flash activos. |
| Escritura | `orbita_ring_out_write(audio, n, flags, lost)`: copia el audio (el buffer del productor se reutiliza al volver), completa `seq` y `version`. Ring lleno: descarta el slot **más viejo** y suma sus muestras (`n_samples + lost_samples`) a `lost_samples` del nuevo más viejo. |
| Lectura | `orbita_ring_out_read(dst)`: copia header + `n_samples` muestras (no el slot entero: la cola es audio viejo de otra vuelta, y no debe llegar al backend) al `dst` del sink y lo libera. El sink manda `2*n_samples` bytes, nunca 1024. Vacío → `ESP_ERR_NOT_FOUND` (el sink espera y reintenta: v1 sin notificación). Que el sink tenga su copia resuelve quién es dueño del slot. |
| Capacidad | 1024 slots = **32,8 s**: cubre los ~35 s documentados (segmento de 2 s + reconexión de WiFi) casi justo. El diseño pide ~60 s (1875 slots, 1,9 MB): subir `ORBITA_RING_SLOTS` si `max_ocupados` o la red lo piden. Si el sink o la red se cuelgan más, se pisa audio (queda anotado en `lost_samples`/`descartados`). |
| Arranque | `orbita_ring_out_init` va **antes de crear las tareas** productora y sink (no es seguro contra uso en paralelo) y **antes de iniciar el AFE** (reserva un bloque contiguo de 325 KB, ~1,9 MB a 60 s: con la PSRAM fragmentada por `esp-sr` puede fallar). |
| Para el sink | Detecta pérdidas por `hdr.lost_samples` y saltos de `hdr.seq`. `orbita_ring_out_get_stats`: ocupados, máximo, escritos, descartados (A12). |
| Pendiente (A6) | Cursor de confirmación (3er puntero) para retener hasta `srv_respuesta_recibida`; tabla de segmentos; pre-roll. Esto cambia `read` a "mirar + liberar". No direccionar por `seq % N` (2³² no es múltiplo de 313: al dar la vuelta salta de slot): usar el índice físico o N potencia de 2. Con el ring lleno hay que decidir si se descarta el nuevo o el más viejo ya enviado sin confirmar.

**Tabla de segmentos (ring out).** Una entrada por segmento: `seq_inicio`, `seq_fin`, tipo (fondo / urgente), estado (pendiente / enviando / enviado / confirmado) y hueco previo (slots descartados). Es el índice de la cinta: qué mandar, en qué orden y en qué estado. Reemplaza al ring de FreeRTOS, a las banderas de la toma de test (`grabando`, `capture_done`) y a los flags de segmento en el slot. La wake word crea una entrada **urgente** con `seq_inicio = seq_detección − pre-roll`; dos segmentos pueden compartir slots.

Quién la toca (con mutex; son pocas filas):
- La tarea que escribe (`feed` o `fetch`), por slot (~31/s): antes de pisar un slot, mira si su segmento se necesita.
- Wake word: agrega la fila urgente. Corte por tiempo o VAD (`dev_fin_segmento`): cierra una fila y abre otra.
- Sink, en cada ciclo: elige el pendiente más prioritario (urgente primero) y lo marca "enviando" / "enviado". Lee por posición (`seq % cantidad_de_slots`); leer no libera.
- Confirmación del backend: "confirmado" y libera. Timeout o error: vuelve a "pendiente".

Alternativas (el patrón es habitual: TCP guarda lo enviado sin confirmar): (2) ring FIFO de tres punteros (escritura / envío / confirmación) + buffer aparte para el urgente con el pre-roll copiado; (3) un buffer por segmento. Cualquiera con confirmación por segmento necesita igual una lista de segmentos en vuelo. **Plan incremental:** v1 = ring de tres punteros para el fondo y lista de segmentos consecutivos, sin urgente ni pre-roll; v2 = tabla con prioridad.

**Reglas de implementación**
- El mutex protege solo la tabla y los índices, nunca el `send` de red (bloquearía al productor y el DMA perdería audio).
- Un segmento "enviando" no se descarta; el espacio se libera por rangos (el pre-roll solapado puede necesitar un slot de otro segmento).
- El callback del DMA solo incrementa un contador (sin mutex, `malloc` ni logs).

### 3.5 Sink, mensajes y contrato con Comunicación

| Decisión | Estado | Nota |
|---|---|---|
| Sink intercambiable (`orbita_sink_t`: `send` + `ctx`) | Provisoria | Hecho. Cada destino define su `send` (lab: `test_sink` en `main/test.c`; UART: `uart_sink`, A7; socket: compañero). `read` copia el slot al sink: no hay dueño compartido ni bloqueo. Falta `open/close` para reconectar. |
| Audio aislado en `orbita_audio/` y `orbita_pipeline/`; `main` solo integra | Cerrada | Hechos captura, pipeline, ring out y sink. Falta el control (A8). |
| Tarea sink (`audio_sink.c`, 13/10) | Provisoria | Core 0, prio 5. Saca slots del ring out, junta `ORBITA_MSG_SLOTS` (4 = 128 ms) en un `orbita_msg_t` y llama a `send(ctx, msg)`. Un slot con `lost_samples > 0` cierra el mensaje en curso: cada mensaje es audio continuo y el hueco queda en `lost_antes` del siguiente. `send` con error: mensaje descartado y contado (reintentos: A6). Ring vacío: espera 10 ms y reintenta (**polling, se deja por ahora**; con el socket, pasar a notificación una vez por mensaje). Compila y arranca en `main` con `test_sink`; sin probar en el chip. |
| Productor del ring out | Provisoria | `pipeline.c`: sin AFE, `tarea_feed` escribe los 2 bloques de 512 con `flags = 0`; con AFE, `tarea_fetch` escribe con `VAD_OK` (+`VOZ`). `lost_samples` = errores de I2S y chunks que no entraron al ring in (1024 c/u) + overflows del DMA (240 c/u), desde el slot anterior. `orbita_ring_out_init` se llama dentro de `orbita_pipeline_start`, antes del AFE. |
| Velocidad mínima del sink | Cálculo | Entrada: 16 000 muestras/s × 2 B = **32 000 B/s** de audio (256 kbit/s) = **31,25 slots/s** = **7,8 mensajes/s** de 4 slots (4096 B de audio c/u, uno cada 128 ms). Para "cumplir justo" cada `send` debe tardar **< 128 ms en promedio**, y el cable tiene que mover ≥ 32 KB/s de audio **más** header y TLS (estimado ~270 kbit/s con TLS). Para vaciar un atraso hay que ir más rápido que el tiempo real: con 10 s acumulados (313 slots, 320 KB) a 2× (64 KB/s) tarda 10 s en alcanzar al vivo. UART0 a 921 600 baud (8N1) ≈ 92 KB/s = 2,9× el audio mono. La espera del sink entre lecturas (hoy `vTaskDelay` de 10 ms) no pesa: es 1/3 de un slot. |
| Mensaje de WebSocket | Abierta | ~100–130 ms (4 slots, ~4 KB), binario (base64 suma 33 %), múltiplo de un slot. Header del cable por mensaje, de Comunicación (sugerido: id de segmento, `seq` del primer slot, cantidad de slots, tipo, `lost_samples` total, hueco previo). Overhead de cabeceras (estimado): ~12 % a 20 ms, ~5 % a 100 ms, ~0,5 % a 1 s; el costo real es CPU y radio. Medir ventana TCP (el buffer de lwIP de ~5,7 KB limita a ~28 KB/s con 200 ms de latencia) y RAM de TLS. |
| Mensajes del backend | Propuesta | (1) confirmación de recepción, rápida; (2) el LLM procesa mensaje a mensaje mientras se envía; (3) la respuesta sale al recibir el fin del pedido (`dev_fin_segmento` del urgente, por silencio con `VOZ`, por ahora). |
| Pipelining: **C, híbrido** | Propuesta | El **fondo** se manda sin esperar respuesta, solo confirmación de recepción ("enviado" → "confirmado", libera memoria). El **urgente** espera su respuesta (FSM secuencial). Máximo 1–2 segmentos de fondo en vuelo, para que el urgente no quede detrás en el buffer del socket (~180 ms). Sin confirmar con Comunicación: que el backend confirme recepción por separado y en <1 s. |
| Recibir mientras se envía (WebSocket full-duplex) | Abierta | Necesario para C. Los `srv_` deberían poder llegar durante `server_send`. Decide Comunicación; impacta la FSM. |
| Mono vs. estéreo hacia el backend | Abierta | Hoy mono (ver 3.3); falta decidir L o mezcla. |
| Política de captura: continua vs. solo tras la wake word | Abierta | Dentro de la consulta ya es continua; falta decidir fuera de sesión. |

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

## 5. Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA (ya hay dos slots).
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con los rings).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.

