# Plan — Presupuesto de RAM y CPU (ESP32-S3 N16R8)

Objetivo: saber, **antes de integrar**, si AFE + wake word + WiFi/TLS/WebSocket + audio de salida entran en el ESP32-S3, y cuánto sobra. Si no entra, se cambia el diseño o el hardware ahora, no después de la PCB.

## Conceptos base (repaso)

| Recurso | Cantidad | Nota |
|---|---|---|
| RAM interna (SRAM) | ~512 KB | La escasa. DMA del I2S, pilas de tareas críticas y buena parte de WiFi/TLS viven acá. |
| PSRAM (octal) | 8 MB | Sobra, pero más lenta. Audio, buffers grandes, tensores de modelos. |
| Flash | 16 MB | Programa, modelos, clips de error. |
| CPU | 2 núcleos, hasta 240 MHz | Hay que repartir tareas entre núcleos. |

Idea clave: la PSRAM sobra, la RAM interna se llena primero. El audio en PSRAM casi no cuenta; lo que hay que vigilar es RAM interna y CPU de los "plugins".

## Decisión previa: chunk provisorio

El buffer de audio es barato (mono 16 kHz / 16-bit = 32 KB/s):

| Chunk | Ping-pong (2 buffers) | % de PSRAM |
|---|---|---|
| 0,5 s | 32 KB | 0,4 % |
| 2 s | 128 KB | 1,6 % |
| 10 s | 640 KB | 8 % |

- Tomar **1–2 s como valor provisorio** para el presupuesto; se afina con el compañero (frente Comunicación).
- **Pregunta abierta que lo define:** ¿cuánto se acepta esperar la respuesta a una pregunta puntual ("¿qué hora es?") desde que el médico termina de hablar?
- Hay 3 niveles de buffer: DMA del I2S (chico, RAM interna), frame del AFE (lo fija la librería, es un dato) y chunk de transmisión (el único que se elige).
- Ojo: con reintentos (`error_reintento`) el micrófono sigue produciendo audio; ping-pong de 2 buffers puede no alcanzar. Se resuelve en el tema "pérdida de datos" (pendiente).

## Pasos

Método: sumar **un consumidor a la vez y medir después de cada uno**, así se sabe cuánto cuesta cada pieza y cuál rompe el presupuesto.

- [ ] **0. Fijar el chunk provisorio (1–2 s)** según la latencia aceptable para una pregunta puntual y el overhead del WebSocket; afinar con el compañero.
- [ ] **1. Línea base del firmware actual (Etapa 1).** Agregar al `main.c` logs de memoria libre al arrancar y después de reservar el buffer de audio. Sirve además para aprender las herramientas.
- [ ] **2. Experimento mínimo de ESP-AFE con 2 mics.** Medir RAM interna, PSRAM y CPU. Es la medición más importante (mayor incertidumbre).
- [ ] **3. Agregar un modelo de Edge Impulse de prueba** (sin dataset propio: para memoria y CPU importa la arquitectura, no el entrenamiento). Comparar con la estimación de RAM/ROM/latencia de Edge Impulse Studio para el target ESP32.
- [ ] **4. Pedirle al compañero la medición de WiFi + WebSocket con TLS** (mismas herramientas).
- [ ] **5. Salida de audio + AEC:** estimar/medir cuando se defina el hardware de salida.
- [ ] **6. Sumar todo y revisar:** margen de RAM interna, uso de CPU por núcleo, propuesta de reparto de tareas entre núcleos.

## Herramientas de medición (ESP-IDF)

- Memoria libre por tipo: `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` y `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`.
- Peor caso alcanzado: `heap_caps_get_minimum_free_size(...)` (el mínimo libre desde el arranque, no el actual).
- Fragmentación: `heap_caps_get_largest_free_block(...)` (puede haber memoria libre pero no un bloque grande contiguo).
- Pilas de tareas: `uxTaskGetStackHighWaterMark()`.
- CPU por tarea: estadísticas de run-time de FreeRTOS (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`, `vTaskGetRunTimeStats`).
- Medir siempre en condiciones reales (WiFi conectado, audio corriendo), no en reposo.

## Tabla a completar

| Consumidor | RAM interna | PSRAM | CPU (núcleo) | Fuente del dato |
|---|---|---|---|---|
| Firmware actual (Etapa 1) | ? | ? | ? | medir |
| ESP-AFE (2 mics, NS, VAD) | ? | ? | ? | doc de esp-sr + medir |
| Modelo de Edge Impulse | ? | ? | ? | Studio + medir |
| WiFi + WebSocket con TLS | ? | ? | ? | doc de ESP-IDF + medir (compañero) |
| Salida de audio + AEC | ? | ? | ? | por medir |
| Buffers de audio (ping-pong) | — | por chunk | — | cálculo |
| NFC, LED, SD | pequeño | — | pequeño | datasheet |

Los valores de esp-sr no se toman de memoria: leerlos de su documentación oficial.

## Pendiente para más adelante (fuera de este plan)

- Política de captura: audio continuo vs. solo tras la wake word, y su consentimiento.
- Seguridad: TLS, autenticación del dispositivo, aprovisionamiento de WiFi, OTA.
- Pérdida de datos: buffer circular, rol de la SD.
- Puntos abiertos del vault: tamaño de chunk (`f_stop`), `audio_play` interrumpido, heartbeat global, clips de error.
- Estados que faltan en la FSM: falla de `audio_init`, login NFC, apagado.
- Actualizar el vault: la tabla de `dev_wake_word()` todavía dice "WakeNet" (ahora es Edge Impulse).
- Verificar: separación de mics (50 mm) vs. lo que soporta el beamforming de esp-sr; licencia comercial y continuidad de Edge Impulse; ley de datos personales y de historia clínica digital.
- Crear `GOTCHA.md` (incluir: GPIO 26–32 flash, 33–37 PSRAM, strapping; dos puertos USB; OneDrive y builds; 32 bits por slot del INMP441 vs. 24 útiles).
