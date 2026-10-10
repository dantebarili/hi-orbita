# Arquitectura — decisiones que afectan al producto final

Solo decisiones que sobreviven al prototipo. Pruebas y pasos de trabajo: `plan_dante.md`. FSM (estados, `dev_`/`srv_`, reintentos): `órbita..md`. Hardware y pines: `CLAUDE.md`. Presupuesto RAM/CPU: `plan-presupuesto-ram-cpu.md`.

Estado: **Cerrada** · **Provisoria** (vale hasta medir/verificar) · **Propuesta** (sin validar) · **Abierta**.

El documento va de la base hacia arriba: 1 configuración del chip → 2 memoria → 3 captura → 4 AFE y wake word → 5 ring → 6 consumidor → 7 depuración → 8 pendientes.

Vocabulario (de menor a mayor):

| Nombre | Qué es | Tamaño |
|---|---|---|
| Muestra | Un número de un mic | 2 bytes |
| Frame de I2S | Un par L+R (`FRAMES_PER_BLOCK` en el código) | 4 bytes. Hoy se leen 1600 por vez (100 ms) |
| Bloque | Lo que se le da al AFE en cada `feed` | ~512 muestras por canal (a verificar) |
| Ítem | Lo que se guarda en el ring de una vez | Un bloque, o lo que se decida |
| Segmento | Lo que se manda al backend entre un `dev_f_stop_send` y el siguiente | 1–2 s, provisorio |

## 1. Configuración del chip (`sdkconfig.defaults` y `partitions.csv`)

Hecho el 2026-10-10 (A0). Compila limpio desde cero. `sdkconfig` está en `.gitignore`; lo que vale para el equipo es `sdkconfig.defaults`.

| Decisión | Estado | Consecuencia |
|---|---|---|
| Flash 16 MB | Cerrada | Es lo que trae el N16R8. La flash de la PCB tiene que ser de 16 MB; si el módulo cambia, se rehace la tabla. Si el valor declarado es mayor que la flash real, no arranca. |
| Tabla de particiones propia | Cerrada | Es lo más caro de cambiar: un equipo instalado no puede mover sus particiones por OTA. Por eso se reservó de más. |
| Dos slots de app (`ota_0`, `ota_1`) | Propuesta | Cuesta 3 MB de flash (sobra). El OTA escribe en el slot libre y, si falla, vuelve al anterior. Con un solo slot, un corte de luz en plena actualización puede dejar el equipo muerto. |
| 3 MB por slot | Provisoria | La app hoy pesa 240 KB. Con `esp-sr`, WiFi, TLS y Edge Impulse puede llegar a 1,5–2 MB (estimación). Se confirma con `idf.py size`. |
| CPU a 240 MHz | Cerrada | Más corriente y calor que a 160 MHz. Todo lo medido de CPU vale solo a esta frecuencia. Ver "riesgos" abajo. |
| Tick de FreeRTOS a 1 ms (`FREERTOS_HZ=1000`) | Cerrada | `vTaskDelay` y timeouts con granularidad de 1 ms (antes 10 ms). Los `vTaskDelay(pdMS_TO_TICKS(x))` se adaptan; los de ticks crudos (`vTaskDelay(10)`) esperan 10 veces menos. Ver "riesgos" abajo. |
| Stack de `app_main` 8192 bytes | Provisoria | Es RAM interna (la escasa). Cada tarea nueva tiene su stack, medido con `uxTaskGetStackHighWaterMark`. |

Riesgos de subir clock y tick (a vigilar, no resueltos):
- **Alimentación:** a 240 MHz el pico de corriente (CPU + WiFi transmitiendo) sube. Con un regulador justo hay caídas de tensión y reinicios (brownout). Dimensionar el regulador de la PCB con esto.
- **Temperatura:** dentro de una carcasa cerrada, 240 MHz calienta más. Medir con el equipo cerrado y todo andando.
- **Medición:** si el AFE entra holgado a 160 MHz, vale probar de nuevo a 160 y comparar consumo.
- **Contador de ticks:** con tick de 1 ms, `xTaskGetTickCount()` (32 bits) da la vuelta a los ~49 días (a 100 Hz eran ~497). Las funciones de FreeRTOS lo manejan; una comparación de ticks hecha a mano en nuestro código falla a los 49 días de encendido.
- **Costo del tick:** 10 veces más interrupciones de tick por segundo y más cambios de contexto entre tareas de igual prioridad. No está medido; no asumir que es despreciable.

### Mapa de la flash

| Partición | Para qué | Offset | Tamaño |
|---|---|---|---|
| bootloader + tabla | Fijos del chip | 0x0 | 36 KB |
| `nvs` | Datos que sobreviven a un reinicio (WiFi, config) | 0x9000 | 24 KB |
| `otadata` | Anota qué slot de app está activo | 0xf000 | 8 KB |
| `phy_init` | Calibración de la radio WiFi | 0x11000 | 4 KB |
| *(hueco de alineación)* | La app tiene que empezar en múltiplo de 64 KB | 0x12000 | 56 KB |
| `ota_0` | App | 0x20000 | 3 MB |
| `ota_1` | App (siguiente actualización) | 0x320000 | 3 MB |
| `model` | Modelos de `esp-sr`: **solo NS (NSNet) y VAD (VADNet)**. No lleva WakeNet ni MultiNet (la wake word es de Edge Impulse y va dentro de la app) | 0x620000 | 2 MB |
| `clips` | Audios locales de error para `audio_play` (1 MB ≈ 32 s a 16 kHz/16-bit mono) | 0x820000 | 1 MB |
| **TOTAL usado** | | 0x0 – 0x920000 | **9,125 MB** |
| **LIBRE** | Reserva (SD offline, logs, lo que venga) | 0x920000 – 0x1000000 | **6,875 MB** |
| Flash total | | | 16 MB |

Tamaños de modelos de `esp-sr` (medidos en el repo): NSNet1 813 KB, NSNet2 334 KB, NSNet3 113 KB, VADNet1 289 KB. Lo máximo que podría entrar es ~1,1 MB, así que 2 MB sobra. Lo que se empaca depende del `sdkconfig`.

Puntos abiertos:
- El `sdkconfig` usa NS y VAD de **WebRTC**, no NSNet/VADNet como dice el plan. No fue una elección nuestra: es el default de `esp-sr` (`Kconfig.projbuild`). WebRTC es el algoritmo clásico de Google (sin red neuronal, liviano en CPU); NSNet/VADNet son redes neuronales de Espressif. Con WebRTC la partición `model` queda vacía. **Decisión pendiente:** en la Fase B se prueban los dos modos (WebRTC y NSNet/VADNet) con las mismas tomas y se mide CPU, RAM, latencia y calidad (ver `plan_dante.md`).
- Flash en **QIO a 80 MHz** (antes DIO, que era el default de IDF, no una elección). Es el modo más rápido para flash quad. Se apagó `ESPTOOLPY_FLASH_MODE_AUTO_DETECT` porque la flash es conocida. El header de la imagen sigue diciendo `dio` (es normal en S3: el ROM arranca en DIO y el bootloader pasa a QIO). **Falta confirmar en el chip**: en el log de arranque tiene que decir `SPI Mode : QIO`. Si no arranca, volver a DIO.
- Cambiar `sdkconfig.defaults` no actualiza un `sdkconfig` ya existente: hay que borrarlo y reconfigurar. Flashear con una tabla nueva borra lo guardado en `nvs`.

## 2. Memoria

- La RAM interna (~512 KB) es la escasa; la PSRAM (8 MB) sobra. Audio y tensores de modelos en PSRAM; DMA del I2S y pilas de tareas críticas en RAM interna.
- Falta medir WiFi + TLS (compañero) y `memory_alloc_mode` del AFE, que decide cuánta RAM interna toma.
- Presupuesto medido por consumidor en `plan-presupuesto-ram-cpu.md`.

## 3. Captura (I2S → tarea de captura)

| Decisión | Estado | Nota |
|---|---|---|
| Formato 16 kHz / 16-bit en todo el sistema | Provisoria | Falta datasheet del ICS-43434: frecuencia de muestreo y rango de reloj. 32 bits por slot a 16 kHz = BCLK de 1,024 MHz. Si el mic no lo cubre, cambia el mic o el formato. |
| Captura en tarea propia, de mayor prioridad que el AFE | Cerrada | Hoy corre en `app_main` (a corregir). |
| Qué core y qué prioridad exacta | Abierta | WiFi corre por defecto en core 0 y el AFE crea sus propias tareas. Se cierra midiendo. |
| La captura convierte 32→16 bit y solo hace `feed` | Cerrada | Nunca se atrasa el DMA. |
| Bloque de captura = bloque de entrada del AFE, leído en runtime | Cerrada | No hardcodear; lo fija la librería. |
| Ganancia/shift de captura fijos en firmware | Provisoria | Calibrado con INMP441 en DevKit. Con el mic, el puerto acústico y la carcasa de producción hay que re-medir y reentrenar. |
| Pérdida de audio siempre detectable (overflows del DMA + ring lleno) | Cerrada | Un hueco silencioso el modelo lo aprende como ambiente. |

Gotcha: el INMP441 entrega 32 bits por slot pero solo 24 son útiles.

## 4. AFE y wake word

```
I2S ──read──▶ tarea de captura ──feed──▶ AFE ──fetch──▶ tarea de salida ─┬─▶ wake word (siempre)
 (DMA)        (bloques de 512)          (NS, VAD,       (audio limpio,   └─▶ ring ──▶ sink ──▶ backend
                                         beamforming)    mono)                (solo en server_send)
```

| Decisión | Estado | Nota |
|---|---|---|
| AFE (`esp-sr`) con 2 mics: NS y VAD activos, AGC y WakeNet apagados | Provisoria | Se integra como medición. El costo "~1,2 MB PSRAM y ~46 % de un núcleo" es estimación sin fuente: medir. El valor del beamforming hacia el backend se mide en precisión del backend (crudo vs. AFE), no solo en SNR: el NS puede perjudicar al ASR. |
| Wake word "Órbita" con modelo propio de Edge Impulse | Provisoria | Corre sobre la salida del AFE. Falta validar que entrenamiento y despliegue coincidan. |
| Lo que va al ring es la **salida** del AFE (mono) | Cerrada | Con AFE en el camino, `fetch` entrega un solo canal; el estéreo solo existe si se saltea el AFE. |
| Formato de entrada con AEC | Riesgo | `feed` pide canales intercalados con la referencia de reproducción al final (mic, mic, ref). Condiciona el ruteo del parlante en la PCB. No diseñar todavía. |

Puntos abiertos:
- **La wake word necesita su propio búfer de ventana** (Edge Impulse): consume cada bloque del AFE siempre, no solo en `server_send`.
- **`vad_cache`:** cuando el VAD recorta el inicio del habla, `fetch` entrega ese cache. Si el ring lo ignora, se pierde el comienzo de la frase.
- **Crudo + procesado para el WAV de estudio:** `afe_fetch_result_t` trae `raw_data`; verificar que sea el crudo y esté alineado.

## 5. Ring buffer

El tipo de ring depende de las políticas, no al revés: **primero las políticas**, después si alcanza el ring de FreeRTOS o hace falta uno propio.

| Parámetro | Valor |
|---|---|
| Reintentos de la FSM (`error_reintento → server_send`) | **A completar.** El ring de FreeRTOS es de consumo único: lo leído no se puede releer. Reenviar tras un reintento exige retener el segmento hasta tener confirmación, o un buffer circular propio con puntero de lectura. |
| Pre-roll (audio previo a la wake word) | **A completar** (depende de la política de captura). Tampoco es posible con el ring de FreeRTOS. |
| Tipo | **Abierta.** `NOSPLIT` con un ítem por bloque era la idea inicial. Ojo: si el sink retiene un ítem esperando ACK, el espacio se libera en orden y bloquea todo el ring. |
| Memoria | PSRAM (decidido). `xRingbufferCreateWithCaps` existe en IDF 5.5.5, pero deja también el struct de control en PSRAM (no confirmado que sea seguro en S3). Alternativa: `xRingbufferCreateStatic`, struct en RAM interna y datos en PSRAM. |
| Cantidad de rings | **Sugerido, sin validar** (Dante investiga cómo se suele hacer): dos. Ring 1 corto captura→AFE; ring 2 largo post-AFE→socket (mono, 32 KB/s). |
| Tamaño / segundos de corte de WiFi a absorber | **A completar** |
| Política cuando se llena (descartar viejo / nuevo / cortar) | **A completar.** La FSM trata "buffer lleno" como corte normal de segmento (`dev_f_stop_send`), pero acá es pérdida: aclarar cuál es cuál. |
| Cómo se avisa la discontinuidad al consumidor | **A completar** |

**¿Ring 1 sobra?** `afe_config_t` ya tiene `afe_ringbuf_size` y `fetch` informa `ringbuff_free_pct`: `feed` ya encola en un buffer interno. Hay que saber si `feed()` bloquea cuando se llena (los headers no lo dicen; leer ejemplos de `esp-sr`). Si bloquea y la captura lo llama directo, el DMA pierde muestras; si no bloquea, ring 1 no hace falta.

## 6. Consumidor y contrato del ítem

| Decisión | Estado | Nota |
|---|---|---|
| Consumidor intercambiable ("sink": empezar / recibir ítem / cerrar) | Provisoria | Falta definir quién es dueño del ítem y si puede bloquear. |
| Cada ítem lleva encabezado: nº de secuencia, canales/muestras, flag de discontinuidad | Propuesta | Detecta pérdidas locales (ring/DMA), no de red. El formato en el cable es del frente Comunicación; esto es interfaz interna. |
| Audio aislado en `firmware/components/orbita_audio/`; `main` solo integra | Provisoria | Hecho para `audio_capture`, `wav_writer` y la dependencia `esp-sr`. Faltan la tarea de captura, las constantes de bloque y el ring (se mudan con A5–A8, cuando se defina su interfaz). |
| Segmento de transmisión (`f_stop`) | Abierta | Provisorio 1–2 s; depende de la latencia aceptable para "¿qué hora es?". |
| Mono vs. estéreo hacia el backend | Abierta | Ver sección 4. |
| Política de captura: continua vs. solo tras la wake word | Abierta | Condiciona qué entra al ring y si hace falta pre-roll. |

## 7. Depuración en producto

| Decisión | Estado | Nota |
|---|---|---|
| Logs por UART0 (GPIO 43/44); USB-Serial-JTAG (19/20) libre para uso propio | Provisoria | Hoy asume el chip puente USB-UART del DevKit. En la PCB eso es un CP2102/CH340 o un conector de programación; alternativa más barata: USB nativo para flasheo y logs, sin puente. Decisión de BOM y ruteo. La PCB necesita conector o pads para flasheo y logs. |
| Streaming de estudio por UART0 con `esp_log_level_set(NONE)` | Solo estudio | No frena los logs de ROM al arrancar ni el handler de pánico. No es para producción. |

## 8. Pendientes de producto (sin dueño todavía)

- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA (la tabla de particiones ya reserva dos slots).
- Resiliencia offline: rol de la SD y política de pérdida de datos (liga con el ring).
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.
- Verificar: separación de mics (50 mm) vs. lo que soporta el beamforming de `esp-sr`; licencia comercial y continuidad de Edge Impulse; ley de datos personales e historia clínica digital.
- El proyecto en OneDrive puede trabar builds de ESP-IDF.
