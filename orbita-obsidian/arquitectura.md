# Arquitectura — decisiones que afectan al producto final

Solo decisiones que sobreviven al prototipo. Pruebas, criterios de "terminado" y pasos de trabajo viven en `plan_dante.md`. FSM (estados, `dev_`/`srv_`, reintentos, interrupción urgente) en `órbita..md`. Hardware y pines en `CLAUDE.md`. Presupuesto RAM/CPU en `plan-presupuesto-ram-cpu.md`.

Estado: **Cerrada** · **Provisoria** (vale hasta medir/verificar) · **Propuesta** (sin validar) · **Abierta**.

Vocabulario (la palabra "chunk" se usaba para tres cosas distintas), de menor a mayor:

| Nombre | Qué es | Tamaño | Estado |
|---|---|---|---|
| **Muestra** | Un número de un mic | 2 bytes | |
| **Frame de I2S** | Un par L+R = 2 muestras. Es el "frame" del código (`FRAMES_PER_BLOCK`) | 4 bytes | Hoy se lee de a 1600 (100 ms) |
| **Bloque** | Lo que se le da al AFE en cada `feed` | ~512 muestras por canal (a verificar) | Todavía no existe |
| **Ítem** | Lo que se guarda en el ring de una vez | Un bloque, o lo que se decida | Hoy: lo de cada `xRingbufferSend` |
| **Segmento** | Lo que se manda al backend entre un `dev_f_stop_send` y el siguiente (`f_stop` en la FSM) | 1–2 s, provisorio | Todavía no existe |

## Pipeline de audio (provisorio)

```
I2S ──read──▶ tarea de captura ──feed──▶ AFE ──fetch──▶ tarea de salida ─┬─▶ wake word (siempre)
 (DMA)        (bloques de 512)          (NS, VAD,       (audio limpio,   └─▶ ring 2 ──▶ sink ──▶ backend
                                         beamforming)    mono)                (solo en server_send)
```

- La captura lee el I2S, convierte 32→16 bit y solo hace `feed` (nunca se atrasa el DMA).
- Lo que va al ring es la **salida** del AFE (mono), no la entrada.
- **Ring 1 (captura→AFE) es dudoso:** el AFE ya tiene buffer interno de entrada. Sobra si `feed()` no bloquea; si bloquea, la captura deja de leer el I2S y el DMA pierde muestras. Se resuelve leyendo ejemplos de `esp-sr`.
- Para el WAV de estudio se necesita crudo + procesado juntos (`raw_data` de `afe_fetch_result_t`, a verificar).

| Decisión | Estado | Nota |
|---|---|---|
| Formato 16 kHz / 16-bit en todo el sistema | Provisoria | Falta datasheet del ICS-43434: fs **y rango de SCK** (32 bits/slot a 16 kHz = BCLK 1,024 MHz). Si no cubre, cambia el mic o el formato. |
| Captura en tarea propia y prioritaria (mayor que el AFE) | Cerrada | Hoy corre en `app_main` (a corregir). |
| Qué core y qué prioridad exacta | Abierta | WiFi corre por defecto en core 0 y el AFE crea sus tareas (`afe_perferred_core`/`priority`). Falta tabla de reparto, se cierra midiendo. |
| Bloque de captura = bloque de entrada del AFE, leído en runtime | Cerrada | No hardcodear; lo fija la librería. |
| Ganancia/shift de captura fijos en firmware | Provisoria | Calibrado con INMP441 en DevKit. Con el mic, puerto acústico y carcasa de producción hay que re-medir y reentrenar. |
| Pérdida de audio siempre detectable (overflows del DMA + ring lleno) | Cerrada | Un hueco silencioso el modelo lo aprende como ambiente. |
| Consumidor intercambiable ("sink": empezar / recibir ítem / cerrar) | Provisoria | Falta definir quién es dueño del ítem y si puede bloquear (ver ring: reintentos). |
| Audio aislado en `firmware/components/orbita_audio/`; `main` solo integra | Propuesta | Evita conflictos de merge con el componente del compañero. |
| Cada ítem lleva encabezado: nº de secuencia, canales/muestras, flag de discontinuidad | Propuesta | Detecta pérdidas **locales** (ring/DMA), no de red. El formato en el cable es del frente Comunicación; esto es interfaz interna. |
| Mono vs. estéreo hacia el backend | Abierta | Con AFE en el camino, `fetch` entrega **un solo canal**: el estéreo solo existe si se saltea el AFE. |
| Política de captura: continua vs. solo tras la wake word | Abierta | Condiciona qué entra al ring y si hace falta pre-roll. |

## Ring buffer

El tipo de ring depende de las políticas, no al revés. **Decidir primero las políticas**; recién después, si alcanza el ring de FreeRTOS o hace falta una estructura propia.

| Parámetro | Valor |
|---|---|
| Reintentos de la FSM (`error_reintento → server_send`) | **A completar.** El ring de FreeRTOS es de consumo único: lo leído y devuelto no se puede releer. Reenviar tras un reintento exige retener el segmento hasta tener confirmación, o un buffer circular propio con puntero de lectura. |
| Pre-roll (audio previo a la wake word) | **A completar** (depende de la política de captura). Tampoco es posible con ringbuf. |
| Tipo | **Abierta.** `NOSPLIT` con un ítem por bloque era la sugerencia inicial; vale solo si las políticas de arriba lo permiten. Ojo: si el sink retiene un ítem esperando ACK, el espacio se libera en orden y bloquea todo el ring. |
| Memoria | PSRAM (decidido). `xRingbufferCreateWithCaps` existe en IDF 5.5.5, pero ubica también el struct de control (con spinlock) en PSRAM; no confirmado que sea seguro en S3. Alternativa: `xRingbufferCreateStatic` con struct en RAM interna y datos en PSRAM. |
| Cantidad de rings | **Sugerencia de Claude, sin validar** (Dante investiga cómo se suele hacer): dos. Ring 1 corto captura→AFE; ring 2 largo post-AFE→socket (mono, 32 KB/s). Ver puntos abiertos abajo. |
| Tamaño / segundos de corte de WiFi a absorber | **A completar** |
| Política cuando se llena (descartar viejo / nuevo / cortar) | **A completar.** Ojo: la FSM trata "buffer lleno" como corte normal de segmento (`dev_f_stop_send`), mientras acá es pérdida. Aclarar cuál es cuál. |
| Cómo se avisa la discontinuidad al consumidor | **A completar** |

Puntos abiertos del esquema de dos rings (revisión del `embedded-reviewer`, 2026-10-08):
- **¿Ring 1 sobra?** `afe_config_t` ya tiene `afe_ringbuf_size` y `fetch` informa `ringbuff_free_pct`: `feed` ya encola en un buffer interno. Hay que saber si `feed()` **bloquea** cuando se llena (los headers no lo dicen; leer ejemplos/doc de `esp-sr`). Si bloquea y la captura llama directo, se pierden muestras del DMA; si no bloquea, ring 1 no hace falta.
- **Ring 2 solo en `server_send`.** La wake word consume cada bloque del AFE siempre, con su propia ventana (Edge Impulse): hace falta un búfer de ventana aparte; no es el ring 2.
- **`vad_cache`:** cuando el VAD recorta el inicio del habla, `fetch` entrega ese cache. Si el ring 2 lo ignora, se pierde el comienzo de la frase.
- **Alineación crudo/AFE** para el WAV de estudio: `afe_fetch_result_t` trae `raw_data`. Posible alternativa a emparejar por secuencia y aplauso; verificar en doc que esté alineado y sea realmente el crudo.

## AFE y wake word

| Decisión | Estado | Nota |
|---|---|---|
| AFE (`esp-sr`) con 2 mics: NS y VAD activos, AGC y WakeNet apagados | Provisoria | Se integra como medición. Costo "~1,2 MB PSRAM y ~46 % de un núcleo": **estimación sin fuente verificada**, medir. El valor del beamforming hacia el backend también se mide en precisión del backend (crudo vs. AFE), no solo SNR: el NS puede perjudicar al ASR. |
| Wake word "Órbita" con modelo propio de Edge Impulse (reemplaza WakeNet) | Provisoria | Corre sobre la salida del AFE. Falta validar que entrenamiento y despliegue coincidan. |
| Segmento de transmisión (`f_stop`) | Abierta | Provisorio 1–2 s; depende de la latencia aceptable para "¿qué hora es?". |
| Formato de entrada del AFE con AEC | Riesgo | `feed` pide canales intercalados con la referencia de reproducción al final: mic, mic, ref. Condiciona ruteo del parlante en la PCB (`dev_wake_word_durante_playback`). No diseñar todavía. |

## Memoria

- La RAM interna (~512 KB) es la escasa; PSRAM (8 MB) sobra. Audio y tensores de modelos en PSRAM; DMA del I2S y pilas de tareas críticas en RAM interna.
- Falta medir WiFi + TLS (compañero) y `memory_alloc_mode` del AFE, que decide cuánta RAM interna toma.
- Presupuesto medido por consumidor en `plan-presupuesto-ram-cpu.md` (tabla a completar).

## Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA.
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con el ring).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.
- Verificar: separación de mics (50 mm) vs. lo que soporta el beamforming de `esp-sr`; licencia comercial y continuidad de Edge Impulse; ley de datos personales e historia clínica digital.
- Gotchas: el INMP441 entrega 32 bits por slot pero solo 24 son útiles; el proyecto en OneDrive puede trabar builds de ESP-IDF.

## Depuración en producto

| Decisión | Estado | Nota |
|---|---|---|
| Logs por UART0 (GPIO 43/44), USB-Serial-JTAG (19/20) libre para uso propio | Provisoria | Hoy asume el chip puente USB-UART del DevKit. En la PCB eso es un CP2102/CH340 o un conector de programación; alternativa más barata: USB nativo para flasheo y logs, sin puente (lo contrario de lo elegido). Decisión de BOM y ruteo. |
| Streaming de estudio por UART0 con `esp_log_level_set(NONE)` | Solo estudio | No frena logs de ROM al arrancar ni el handler de pánico. No es para producción. |

Config actual: `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` y `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`. La PCB final necesita conector o pads para flasheo y logs.
