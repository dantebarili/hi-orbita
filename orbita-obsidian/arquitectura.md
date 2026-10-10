# Arquitectura — decisiones que afectan al producto final

Solo decisiones que sobreviven al prototipo. Pasos y pruebas: `plan_dante.md`. FSM (estados, `dev_`/`srv_`, reintentos): `órbita..md`. Hardware y pines: `CLAUDE.md`. Presupuesto RAM/CPU: `plan-presupuesto-ram-cpu.md`.

Estado: **Cerrada** · **Provisoria** (vale hasta medir/verificar) · **Propuesta** (sin validar) · **Abierta**.

Orden (de la base hacia arriba): 1 chip → 2 memoria → 3 DSP y pipeline de audio → 4 depuración → 5 pendientes.

### Vocabulario (de menor a mayor)

| Nombre | Qué es | Tamaño |
|---|---|---|
| Muestra | Un número de un mic | 2 bytes (16 bit) |
| Frame de I2S | Un par L+R | 4 bytes (8 B en crudo de 32 bit) |
| **Chunk** | Lo que consume y entrega el AFE (`feed`/`fetch`); cada slot del ring 2 guarda uno (`FRAMES_PER_CHUNK` en el código; hoy 1600 frames = 100 ms, objetivo el del AFE) | ~512 muestras por canal = 32 ms (a verificar) |
| **Mensaje** | Lo que el sink escribe al socket de una vez; un segmento viaja en varios y empieza a salir antes de cerrarse | ~100 ms (a definir con Comunicación) |
| **Segmento** | Lo que se manda al backend entre un `dev_fin_segmento` y el siguiente (unidad de la FSM) | 1–2 s, provisorio |

---

## 1. Chip: `sdkconfig.defaults` y `partitions.csv`

Hecho el 2026-10-10 (A0). Compila limpio desde cero. `sdkconfig` está en `.gitignore`: lo que vale para el equipo es `sdkconfig.defaults`.

| Decisión | Estado | Consecuencia |
|---|---|---|
| Flash 16 MB | Cerrada | Es lo que trae el N16R8; la flash de la PCB debe ser de 16 MB (si cambia el módulo, se rehace la tabla). Si el valor declarado supera la flash real, no arranca. |
| Flash en QIO a 80 MHz | Provisoria | Antes DIO (default de IDF, no una elección). Más rápido en flash quad; se apagó `ESPTOOLPY_FLASH_MODE_AUTO_DETECT`. El header de la imagen sigue diciendo `dio` (normal en S3: el ROM arranca en DIO y el bootloader pasa a QIO). **Falta confirmar en el chip:** el log de arranque debe decir `SPI Mode : QIO`; si no arranca, volver a DIO. |
| Tabla de particiones propia | Cerrada | Lo más caro de cambiar: un equipo instalado no puede mover sus particiones por OTA. Por eso se reservó de más. |
| Dos slots de app (`ota_0`, `ota_1`) | Propuesta | Cuestan 3 MB de flash (sobra). El OTA escribe en el slot libre y, si falla, vuelve al anterior; con un solo slot, un corte de luz puede dejar el equipo muerto. |
| 3 MB por slot | Provisoria | La app hoy pesa 240 KB; con `esp-sr`, WiFi, TLS y Edge Impulse podría llegar a 1,5–2 MB (estimación). Confirmar con `idf.py size`. |
| CPU a 240 MHz | Cerrada | Más corriente y calor que a 160. Lo medido de CPU vale solo a esta frecuencia. |
| Tick de FreeRTOS a 1 ms (`FREERTOS_HZ=1000`) | Cerrada | `vTaskDelay` y timeouts con granularidad de 1 ms (antes 10). Los `pdMS_TO_TICKS(x)` se adaptan; un `vTaskDelay(10)` en ticks crudos espera 10 veces menos. |
| Stack de `app_main` 8192 B | Provisoria | RAM interna (la escasa). Cada tarea nueva tiene su stack, medido con `uxTaskGetStackHighWaterMark`. |

**Riesgos de subir clock y tick (a vigilar):**
- **Alimentación:** a 240 MHz el pico de corriente (CPU + WiFi transmitiendo) sube; con un regulador justo hay brownout. Dimensionar el regulador de la PCB con esto.
- **Temperatura:** en carcasa cerrada calienta más; medir con todo andando.
- **Comparar:** si el AFE entra holgado a 160 MHz, probar de nuevo a 160 y comparar consumo.
- **Contador de ticks:** a 1 ms, `xTaskGetTickCount()` (32 bit) da la vuelta a los ~49 días (a 100 Hz eran ~497). FreeRTOS lo maneja; una comparación de ticks hecha a mano falla a los 49 días encendido.
- **Costo del tick:** 10 veces más interrupciones y cambios de contexto; no medido, no asumir que es despreciable.

### Mapa de la flash

| Partición | Para qué | Offset | Tamaño |
|---|---|---|---|
| bootloader + tabla | Fijos del chip | 0x0 | 36 KB |
| `nvs` | Datos que sobreviven a un reinicio (WiFi, config) | 0x9000 | 24 KB |
| `otadata` | Anota qué slot de app está activo | 0xf000 | 8 KB |
| `phy_init` | Calibración de la radio WiFi | 0x11000 | 4 KB |
| *(hueco de alineación)* | La app empieza en múltiplo de 64 KB | 0x12000 | 56 KB |
| `ota_0` | App | 0x20000 | 3 MB |
| `ota_1` | App (siguiente actualización) | 0x320000 | 3 MB |
| `model` | Modelos de `esp-sr`: **solo NS y VAD**. No lleva WakeNet ni MultiNet (la wake word es de Edge Impulse y va dentro de la app) | 0x620000 | 2 MB |
| `clips` | Audios locales de error para `audio_play` (1 MB ≈ 32 s a 16 kHz/16-bit mono) | 0x820000 | 1 MB |
| **TOTAL usado** | | 0x0 – 0x920000 | **9,125 MB** |
| **LIBRE** | Reserva (SD offline, logs, lo que venga) | 0x920000 – 0x1000000 | **6,875 MB** |
| Flash total | | | 16 MB |

Modelos de `esp-sr` (medidos en el repo): NSNet1 813 KB, NSNet2 334 KB, NSNet3 113 KB, VADNet1 289 KB. Lo máximo que podría entrar son ~1,1 MB, así que 2 MB sobra.

Puntos abiertos:
- **NS y VAD hoy son WebRTC** (default de `esp-sr`, no una elección nuestra): algoritmo clásico de Google, sin red neuronal, liviano en CPU; con WebRTC la partición `model` queda vacía. NSNet/VADNet son redes neuronales de Espressif. **Decisión pendiente:** en la Fase B se prueban los dos modos con las mismas tomas y se mide CPU, RAM, latencia y calidad (ver `plan_dante.md`).
- Cambiar `sdkconfig.defaults` no actualiza un `sdkconfig` existente: hay que borrarlo y reconfigurar. Flashear con una tabla nueva borra lo guardado en `nvs`.

## 2. Memoria

- RAM interna (~512 KB): la escasa. PSRAM (8 MB): sobra. Audio y tensores de modelos en PSRAM; DMA del I2S y pilas de tareas críticas en RAM interna.
- Falta medir WiFi + TLS (compañero) y `memory_alloc_mode` del AFE, que decide cuánta RAM interna toma.
- Presupuesto medido por consumidor: `plan-presupuesto-ram-cpu.md`.

## 3. DSP y pipeline de audio

### 3.1 Flujo y datos en cada punto

```
mics ─▶ I2S/DMA ─▶ tarea de captura ─▶ RING 1 ─▶ tarea feed ─▶ AFE ─▶ tarea fetch ─┬▶ wake word (siempre)
                    (32→16 bit)        (~0,5 s)                                   └▶ RING 2 ─▶ sink ─▶ WebSocket ─▶ backend
                                                                                      (~60 s)
```

| # | Punto | Qué dato hay | Formato / tamaño | Flujo |
|---|---|---|---|---|
| 1 | Mics (INMP441 / ICS-43434, 2, L y R) | Audio digital I2S | 24 bit útiles en slot de 32; fs 16 kHz; BCLK = 16 000 × 2 × 32 = 1,024 MHz | — |
| 2 | I2S + DMA | Frames estéreo crudos | 32 bit × 2 = 8 B por frame; buffer DMA 6 × 240 frames = 1440 frames ≈ 90 ms (RAM interna) | 128 KB/s |
| 3 | Tarea de captura | Convierte 32→16 bit (shift fijo) y arma chunks | Estéreo 16 bit = 4 B por frame; chunk de 512 frames = 2 KB (32 ms) | 64 KB/s |
| 4 | Ring 1 | Chunks de entrada, sin encabezado | ~16 chunks ≈ 32 KB (~0,5 s) | 64 KB/s |
| 5 | Tarea feed → AFE | Canales intercalados (mic, mic; con AEC además la referencia del parlante) | int16 a 16 kHz | 64 KB/s |
| 6 | AFE (`esp-sr`) | Beamforming + NS + VAD (AGC y WakeNet apagados). Entrega audio limpio **mono**, `vad_state` y `raw_data` | Chunk de salida (`get_fetch_chunksize`) ≈ 512 muestras = 1 KB | 32 KB/s |
| 7 | Wake word (Edge Impulse) | Consume **todos** los chunks del AFE, con su propia ventana | — | 32 KB/s |
| 8 | Ring 2 | Chunk de salida + encabezado de 16 B por slot, y la tabla de segmentos | ~1040 B por slot; 60 s ≈ 1875 slots ≈ 1,9 MB en PSRAM | 32 KB/s |
| 9 | Sink | Elige un segmento, lo parte en mensajes | Mensaje ≈ 4 chunks ≈ 4 KB + header del cable | ≈ 270 kbit/s con TLS |
| 10 | Backend | Audio mono 16 kHz / 16 bit por segmento | — | — |

- **Hoy** (código): I2S → captura (tarea, core 1) → un ring de FreeRTOS → `tarea_envio` → UART0 (estudio). Sin AFE, sin ring 2, sin tabla.
- **Estudio:** WAV de 2 canales (ch0 mic crudo, ch1 salida del AFE) = 64 KB/s por UART a 921600 baud (el UART da ~1,44× el audio). No mandar 3 canales (96 KB/s no entran).

### 3.2 Captura (I2S → tarea de captura)

| Decisión | Estado | Nota |
|---|---|---|
| Formato 16 kHz / 16 bit en todo el sistema | Provisoria | Falta el datasheet del ICS-43434: frecuencia de muestreo y rango de reloj (BCLK 1,024 MHz). Si el mic no lo cubre, cambia el mic o el formato. |
| Captura en tarea propia, de mayor prioridad que el AFE | Cerrada | Hecho en A2 (core 1, prioridad 6); falta probar en el labo. |
| Core y prioridad exactos | Abierta | WiFi corre por defecto en core 0 y el AFE crea sus tareas. Se cierra midiendo. |
| La captura convierte 32→16 bit y escribe en el ring 1; otra tarea hace `feed` | Cerrada | Así el DMA nunca se atrasa. |
| Chunk de captura = chunk de entrada del AFE, leído en runtime | Cerrada | No hardcodear: lo fija la librería. |
| Ganancia/shift de captura fijos en firmware | Provisoria | Calibrado con INMP441 en DevKit. Con el mic, puerto acústico y carcasa de producción hay que re-medir y reentrenar. |
| Pérdida de audio siempre detectable (overflows del DMA + ring lleno) | Cerrada | Un hueco silencioso el modelo lo aprende como ambiente. |

El INMP441 entrega 32 bits por slot pero solo 24 son útiles.

### 3.3 AFE y wake word

| Decisión | Estado | Nota |
|---|---|---|
| AFE (`esp-sr`) con 2 mics: NS y VAD activos, AGC y WakeNet apagados | Provisoria | Se integra como medición. El costo "~1,2 MB PSRAM y ~46 % de un núcleo" es estimación sin fuente: medir. El beamforming hacia el backend se mide en precisión del backend (crudo vs. AFE), no solo en SNR: el NS puede perjudicar al ASR. |
| Wake word "Órbita" con modelo propio de Edge Impulse | Provisoria | Corre sobre la salida del AFE. Falta validar que entrenamiento y despliegue coincidan. |
| Al ring 2 va la **salida** del AFE (mono) | Cerrada | `fetch` entrega un solo canal; el estéreo solo existe si se saltea el AFE. |
| Entrada del AFE con AEC | Riesgo | `feed` pide la referencia del parlante al final (mic, mic, ref). Condiciona el ruteo del parlante en la PCB. No diseñar todavía. |

Puntos abiertos:
- La wake word necesita su propio búfer de ventana (Edge Impulse): consume cada chunk siempre, no solo en `server_send`.
- **Falsos negativos y falsos positivos de la wake word (hay que lidiar con los dos):**
  - *Falso negativo* (el médico dijo "Órbita" y el ESP32 no lo detectó): el backend, que recibe el audio de toda la consulta, debe poder detectar un "Órbita" que el ESP32 perdió y **avisar** para tratar ese tramo como pedido urgente. Hace falta un evento `srv_` nuevo en la FSM (no existe en el vault). El audio ya está en el backend; el ring 2 lo conserva hasta la confirmación por si hay que reenviar.
  - *Falso positivo* (el ESP32 detecta una wake word que no se dijo): se pausa el resumen y se manda un pedido urgente de más. El backend debe poder descartarlo y devolver al equipo al resumen (otro evento `srv_`, a definir). Métricas: FRR y falsas activaciones por hora (ver `plan_dante.md`).
- `vad_cache`: cuando el VAD recorta el inicio del habla, `fetch` entrega ese cache; si el ring 2 lo ignora, se pierde el comienzo de la frase.
- Crudo + procesado para el WAV de estudio: `afe_fetch_result_t` trae `raw_data`; verificar que sea el crudo y esté alineado.

### 3.4 Rings, encabezado y tabla de segmentos

**Políticas (A5, 2026-10-10)**

| Tema | Decisión |
|---|---|
| Qué se guarda | Siempre, también en `server_back` y `audio_play`: sin huecos en la historia clínica. Se puede achicar a ~40 s dejando de grabar en `audio_play` (cuesta huecos). |
| Reintentos | El segmento se retiene hasta `srv_respuesta_recibida` (~11 s con 2 intentos de 5 s + backoff de 1 s). Si se agotan, la FSM lo descarta y queda anotado el hueco. |
| Pre-roll | ~1 s (≈31 chunks) antes de la wake word. |
| Ring lleno | Se descarta el segmento pendiente más viejo; el hueco queda en la tabla. |

**Los dos rings**

| | Ring 1 | Ring 2 |
|---|---|---|
| Une | captura → tarea `feed` | tarea `fetch` → sink |
| Contiene | chunks de entrada (estéreo, 16 bit, ~2 KB), sin encabezado | chunks de salida (mono) con encabezado de 16 B |
| Tamaño | ~16 chunks ≈ 32 KB (~0,5 s) | ~60 s ≈ 1,9 MB en PSRAM (segmento de 2 s + reconexión de WiFi ≈ 35 s; falta sumar el largo máximo de `audio_play`) |
| Para qué | Seguro por si `feed()` bloquea (sin él el I2S queda sin leer y el DMA pierde audio). Se puede sacar si en la Fase B se comprueba que no bloquea. | Retención, reintentos, pre-roll |
| Estructura | Ring de FreeRTOS (`NOSPLIT`, un chunk por ítem) | Ring de slots propio + tabla de segmentos. El de FreeRTOS no permite releer, ni pre-roll, ni mandar primero el urgente. |

**Encabezado del chunk (solo ring 2, 16 bytes).** Guarda lo que se sabe al escribir el chunk.

| Campo | Tipo | Qué hace |
|---|---|---|
| `seq` | u32 | Nº de chunk, sube de a 1. Comparar con `(int32_t)(a - b)` para que valga al dar la vuelta (~4,4 años a 32 ms). Se reinicia en cada arranque: el backend necesita además un id de sesión. |
| `lost_samples` | u32 | Muestras que perdió el DMA justo antes de este chunk (0 = ninguna). Reconstruye el tiempo y alinea el WAV de estudio. No se rellena con ceros en silencio. |
| `n_samples` | u16 | Muestras por canal de este chunk (el `vad_cache` puede traer otro tamaño). |
| `flags` | u8 | bit 0 `VOZ` (el VAD detectó voz); resto reservado. |
| `version` | u8 | Versión del formato. |
| (reservado) | u32 | Futuro. |

- Cómo llega `lost_samples`: el callback de overflow del DMA (A1) suma a un contador atómico; la tarea `fetch` lo lee, lo pone en 0 y lo copia al chunk. Es aproximado por la latencia del AFE: alcanza para detectar y contar.
- Es **interno**: no viaja al backend. El sink lo usa para armar el header del cable de cada mensaje (que resume `seq`, `lost_samples`, etc.) y manda solo el audio de los chunks.
- No lleva inicio/fin de segmento ni "urgente": se deciden después de guardar el chunk y los pre-roll se solapan.
- Alineación: con ese orden de campos no hay relleno; verificar con `_Static_assert(sizeof == 16)`, sin `__attribute__((packed))`. Con 16 B el payload queda alineado a 16 B. El costo en PSRAM es despreciable aunque el chunk baje a 10 ms (6000 slots, 2 MB).
- Dónde se codea: struct, ring y tabla en `firmware/components/orbita_audio/` (A5–A6); lo llena la tarea `fetch` y lo lee el sink. **Todavía no hay código.**

**Tabla de segmentos (ring 2).** Una entrada por segmento: `seq_inicio`, `seq_fin`, tipo (fondo / urgente), estado (pendiente / enviando / enviado / confirmado) y hueco previo (chunks descartados). La wake word crea una entrada **urgente** con `seq_inicio = seq_detección − pre-roll`: ese es su marcador. Dos segmentos pueden compartir chunks (el pre-roll del urgente incluye la cola del resumen de fondo).

**Cómo lee el sink.** Los slots son de tamaño fijo y el de un chunk sale de `seq % cantidad_de_slots`, así que el sink lee un rango por posición sin sacar nada; leer no libera. Elige el segmento por la tabla (el urgente primero); dentro de un segmento va en orden. El espacio se libera al confirmarse el segmento.

**Reglas de implementación**
- El mutex protege solo la tabla y los índices, nunca el `send` de red (bloquearía al productor y el DMA perdería audio).
- Un segmento "enviando" no se descarta.
- El espacio se libera por rangos (el pre-roll solapado puede necesitar un chunk de otro segmento).
- El callback del DMA solo incrementa un contador (sin mutex, `malloc` ni logs).
- Mutex alcanza: son ~31 slots por segundo.

### 3.5 Sink, mensajes y contrato con Comunicación

| Decisión | Estado | Nota |
|---|---|---|
| Sink intercambiable (empezar / recibir chunk / cerrar) | Provisoria | Falta definir quién es dueño del chunk y si puede bloquear. El sink UART de estudio puede leer en orden; la lógica de elegir segmentos va solo en el sink del socket (compañero). |
| Audio aislado en `firmware/components/orbita_audio/`; `main` solo integra | Provisoria | Hecho para `audio_capture`, `wav_writer` y `esp-sr`. Faltan captura, constantes y rings (A5–A8). |
| Mensaje de WebSocket | Abierta | ~100–130 ms (4 chunks, ~4 KB), binario (base64 suma 33 %), múltiplo de un chunk. Header del cable por mensaje, definido por Comunicación (sugerido: id de segmento, `seq` del primer chunk, cantidad de chunks, tipo, `lost_samples` total, hueco previo). Cada mensaje agrega ~70–100 B de cabeceras (WebSocket + TLS + TCP/IP, estimado): ~12 % a 20 ms, ~5 % a 100 ms, ~0,5 % a 1 s; el costo real es CPU y radio por mensaje. Medir ventana TCP (lwIP, buffer de ~5,7 KB limita a ~28 KB/s con 200 ms de latencia) y RAM de TLS. |
| Tamaño del segmento (`dev_fin_segmento`) | Abierta | Provisorio 1–2 s; depende de la latencia aceptable para "¿qué hora es?". |
| Recibir mientras se envía (WebSocket full-duplex) | Abierta | La FSM es secuencial; para que el backend interrumpa en plena conversación, los `srv_` deberían poder llegar durante `server_send`. Decide Comunicación; impacta la FSM. |
| Mono vs. estéreo hacia el backend | Abierta | Con AFE sale mono (ver 3.3). |
| Política de captura: continua vs. solo tras la wake word | Abierta | Con escucha continua ya definida para la consulta, queda decidir fuera de sesión. |

Puntos abiertos del flujo:
- **FSM:** `dev_fin_segmento` incluye "buffer lleno" como motivo de corte; con esta política ring lleno es pérdida. Propuesta: solo tiempo o VAD. Falta actualizar `órbita..md`.
- Pipelining: ¿se envía el siguiente segmento sin esperar la respuesta? Si la respuesta tarda más que un segmento, el backlog crece y el ring descarta.
- Id de segmento y de sesión; `srv_respuesta_recibida` debe decir a qué segmento responde.
- "Retomar donde quedó" (FSM) vs. reenviar el segmento entero (reintento idempotente: el backend deduplica).
- Largo máximo de una respuesta de `audio_play`.
- A verificar en el chip: tamaño real del chunk del AFE, unidad de `event->size` (en bytes, a 8 B por frame son `size/8` muestras) y si `feed()` bloquea.

### 3.6 Para retomar: cierre del segmento y latencia (decidir con calma)

**Cómo se anidan** (el header interno de 16 B no viaja al backend: el sink lo usa para armar el header del cable):

```
segmento (1-2 s)   = [mensaje][mensaje] ... [mensaje]        ← unidad de la FSM
mensaje (~100 ms)  = [header del cable] + audio de ~4 chunks  ← lo que sale por el socket
chunk en ring 2    = [header interno 16 B] + audio (32 ms)    ← solo dentro del ESP32
```

**Velocidades:** 128 KB/s en el I2S (32 bit × 2 mics) → 64 KB/s al pasar a 16 bit → 32 KB/s con el AFE mono. 256 kbit/s es lo normal para voz a 16 kHz/16 bit; para WiFi es poco.

**`VOZ` es un bit por chunk** (32 ms), no por segmento ni por mensaje. Fin de frase = N chunks seguidos sin `VOZ` (silencio de ~0,5–1 s: valor típico, sin medir).

**El tamaño del segmento no es un tema de sockets** (eso es el mensaje). Es una decisión de producto y de backend:
- Corto: más llamadas (más costo), riesgo de cortar una frase, libera memoria rápido, reenvía poco si falla.
- Largo: más contexto para el backend, pero el resumen sale más tarde y se reenvía más si falla.
- **Un segmento de tiempo fijo largo es inviable para el pedido urgente:** si el médico termina de hablar justo al empezar el segmento, se espera todo el tiempo restante. Por eso el urgente **no** tiene largo fijo: se cierra por silencio (VAD).
- El segmento de fondo (resumen) tolera demora; conviene cortarlo en una pausa de voz con un máximo.

**Latencia del pedido urgente** = silencio que se espera para dar por terminada la frase (~0,5–1 s) + proceso del backend + respuesta. El largo del segmento no suma si el audio ya viaja mientras se habla (los mensajes salen antes de cerrar el segmento).

**Aviso de wake word al backend.** Los ~100 ms del mensaje son el tiempo de *juntar* audio, no de enviar. El aviso ("urgente, pausá el resumen") es un mensaje de control de pocos bytes que sale apenas se detecta, sin esperar el ritmo del audio; después sale el pre-roll (~1 s ya guardado, 32 KB, decenas de ms) y luego el audio en vivo. Estimado desde que termina "Órbita": detección en el ESP32 ~0,1–0,4 s (depende de la ventana y las inferencias por ventana del modelo de Edge Impulse; es el tramo más grande) + red decenas de ms ≈ 0,2–0,5 s. Medir en la Fase B. Al saltar la wake word, la FSM corta el segmento de fondo en ese momento, así que un segmento de fondo largo no afecta la latencia del urgente.

**Decisiones al retomar (en orden de importancia)**
1. **¿Quién cierra el segmento urgente?** (a) el equipo, cuando ve ≥ X ms sin `VOZ` (`dev_fin_segmento`); o (b) el backend, con ASR en streaming, que detecta el fin de frase sobre el audio que ya recibió y puede **responder antes** de que el equipo cierre. Hoy la FSM espera el cierre del equipo para pasar a `server_back`. Propuesta: (a) como cierre formal más (b) permitido; depende de que los `srv_` puedan llegar durante `server_send` (punto 2).
2. **Recibir mientras se envía / pipelining:** hoy la FSM es secuencial; el WebSocket no lo es. Impacta la FSM y el ring.
3. **Tamaño y criterio del segmento de fondo** (pausa de voz con máximo; el número hay que acordarlo con quien arme el backend).
4. **Qué resumen de `VOZ` lleva el header del cable** (p. ej. "hay voz", "ms de silencio final" por mensaje), para que el backend pueda decidir sin recalcular.
5. **Falsos negativos/positivos de la wake word:** eventos `srv_` para "el backend detectó un Órbita que el ESP32 perdió" y "no era un pedido" (ver 3.3).
6. Id de segmento y de sesión; `srv_respuesta_recibida` con el id; retomar vs. reenviar entero; sacar "buffer lleno" de `dev_fin_segmento` en el vault.

## 4. Depuración en producto

| Decisión | Estado | Nota |
|---|---|---|
| Logs por UART0 (GPIO 43/44); USB-Serial-JTAG (19/20) libre para uso propio | Provisoria | Hoy asume el chip puente USB-UART del DevKit. En la PCB es un CP2102/CH340 o un conector de programación; alternativa más barata: USB nativo para flasheo y logs, sin puente. Decisión de BOM y ruteo. La PCB necesita conector o pads para flasheo y logs. |
| Streaming de estudio por UART0 con `esp_log_level_set(NONE)` | Solo estudio | No frena los logs de ROM al arrancar ni el handler de pánico. No es para producción. |

## 5. Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA (la tabla de particiones ya reserva dos slots).
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con los rings).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.
- Verificar: separación de mics (50 mm) vs. lo que soporta el beamforming de `esp-sr`; licencia comercial y continuidad de Edge Impulse; ley de datos personales e historia clínica digital.
- El proyecto en OneDrive puede trabar builds de ESP-IDF.
