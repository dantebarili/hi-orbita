---
name: esp32-firmware-engineer
description: Criterio de ingeniería de firmware ESP-IDF para Órbita (ESP32-S3 N16R8). Usar al escribir o revisar código con tareas/colas/ISR de FreeRTOS, memoria (RAM interna, PSRAM, DMA), sdkconfig/particiones, o al diagnosticar un reinicio, panic, watchdog o log de arranque. Complementa a pin-map-check (pines) y fsm-check (FSM); no los reemplaza.
user-invocable: false
---

Criterio de ingeniero senior ESP-IDF: correctitud y depurabilidad antes que elegancia. El equipo tiene C oxidado y no cursó embebidos: explicar el concepto la primera vez que aparece, y no generar código completo sin pedido explícito (CLAUDE.md).

## Contexto: leerlo, no preguntarlo

Chip (ESP32-S3 N16R8, 16 MB flash, 8 MB PSRAM octal), pines y guardrails están en `CLAUDE.md`; decisiones en `orbita-obsidian/arquitectura.md`; ESP-IDF 5.5.x (ver `.vscode/settings.example.json`). Solo preguntar lo que falte de verdad. Si un dato de hardware no está y cambia la respuesta (pin, tensión, tiempo), decirlo y marcarlo como supuesto; verificar contra datasheet o el MCP `espressif-docs` y citar la sección. No afirmar de memoria.

## Escribir y revisar

- **ISR mínima.** Solo APIs `...FromISR`, sin `malloc`, sin logs, sin bloquear. Trabajo real en una tarea vía cola o notificación. Con `portYIELD_FROM_ISR` si despertó una tarea más prioritaria.
- **ISR con flash ocupada.** Una ISR (o lo que llama) sin `IRAM_ATTR` falla con "Cache disabled but cached memory region accessed" si la flash está en escritura (NVS, OTA, Wi-Fi). Para el I2S: `CONFIG_I2S_ISR_IRAM_SAFE` (ya en `sdkconfig.defaults`).
- **Sección crítica.** En el S3 (dos núcleos) `portENTER_CRITICAL` pide un spinlock (`portMUX_TYPE`). Cortas, sin logs ni llamadas a drivers adentro. Preferir mutex.
- **Prioridades.** Las de las tareas del sistema (Wi-Fi, `esp_timer`, tcpip) son altas: una tarea propia por encima que no se bloquea los deja sin CPU y salta el watchdog. Tareas fijadas a un núcleo solo con motivo medido (captura en core 1, red en core 0 ya están decididas en `arquitectura.md`).
- **Tiempo.** `vTaskDelay` en ticks crudos depende de `CONFIG_FREERTOS_HZ` (1 ms acá): usar `pdMS_TO_TICKS`. Periódicas con `vTaskDelayUntil`.
- **Pilas.** Medir con `uxTaskGetStackHighWaterMark` (en bytes en ESP-IDF, no en palabras) y anotar el mínimo. Cuidar logs con formato, JSON y TLS.
- **Errores.** Chequear y propagar `esp_err_t`; limpiar en fallo de init parcial. No "arreglar" un watchdog desactivándolo: buscar el bucle, bloqueo o sección larga.
- **`_Static_assert`** para tamaños de struct, slots y mensajes que se asumen entre módulos.

## Memoria

- RAM interna (~512 KB) es la escasa; PSRAM sobra pero es más lenta. Audio y tensores de modelos en PSRAM; DMA del I2S y pilas críticas en RAM interna (`arquitectura.md`).
- Reservas con `heap_caps_malloc` y la capacidad explícita (`MALLOC_CAP_INTERNAL`, `MALLOC_CAP_SPIRAM`, `MALLOC_CAP_DMA`). En el S3 el DMA puede acceder a PSRAM, pero con alineación a línea de caché y más latencia: no asumir ni que sirve ni que no; verificar en la doc del periférico antes de poner un buffer de DMA en PSRAM.
- Sin `malloc`/`free` variables en caminos calientes (fragmentación): buffers estáticos o pools. Medir `heap_caps_get_free_size` y `heap_caps_get_minimum_free_size` antes y después de cada función grande (Wi-Fi, AFE, TLS).
- Tamaño de código y RAM: `idf.py size`, `size-components`.

## Configuración

- **Los cambios van en `sdkconfig.defaults`**, no en `sdkconfig` (generado, ignorado por git y bloqueado por el hook). Cambiar `sdkconfig.defaults` no actualiza un `sdkconfig` existente: borrarlo y reconfigurar. Flashear otra tabla de particiones borra el `nvs`.
- Particiones: ver mapa en `arquitectura.md` §1. No cambiar el layout sin confirmar flash, OTA y NVS necesarios.
- `menuconfig` sirve para descubrir una opción; lo que se entrega es la clave en `sdkconfig.defaults`.
- Si se suma `esp-sr` u otro componente, fijar la versión en `idf_component.yml` y comprobar su rango de `idf` contra 5.5.x.

## Diagnosticar un reinicio o panic

1. **Juntar datos.** Log completo desde el reset (no solo el final), versión de ESP-IDF, y el **ELF del mismo build que se flasheó** (si se recompiló, las direcciones no sirven). Anotar puerto y comando.
2. **Clasificar la etapa:** build/link, flasheo, arranque (tabla de particiones, imagen), pánico en ejecución, o falla funcional sin panic.
3. **Leer la causa** en la primera línea del panic:

| Mensaje | Causa típica | Mirar |
|---|---|---|
| `LoadProhibited` / `StoreProhibited` | puntero nulo o liberado, handle sin inicializar | primer frame propio, init parcial fallido |
| `InstrFetchProhibited` | puntero a función corrupto, pila pisada | buffer que desborda hacia la pila |
| `Cache disabled but cached memory region accessed` | ISR o callback fuera de IRAM con flash ocupada | `IRAM_ATTR`, `*_ISR_IRAM_SAFE` |
| `Task watchdog got triggered` | tarea sin ceder, bloqueo, tarea de alta prioridad que no duerme | prioridades, `vTaskDelay`, sección crítica |
| `Interrupt wdt timeout` | ISR o sección crítica demasiado larga, o un spinlock tomado | trabajo dentro de la ISR |
| `Stack canary watchpoint triggered (nombre)` | desborde de pila de esa tarea | tamaño de pila, arrays locales |
| `CORRUPT HEAP` / `assert failed` | escritura fuera de rango, doble `free` | `CONFIG_HEAP_POISONING_COMPREHENSIVE` |
| `Brownout detector was triggered` | caída de tensión por pico (Wi-Fi, parlante) | alimentación; en la PCB, dimensionar el regulador |

   El texto exacto puede variar con la versión: contrastar con la doc de 5.5.x.
4. **Decodificar el backtrace** con el ELF: `idf.py monitor` lo hace solo; a mano, `xtensa-esp32s3-elf-addr2line -pfiaC -e build/<app>.elf <direcciones>`. Mirar el primer frame propio. En dos núcleos el panic imprime registros de ambos: importa el del núcleo que falló.
5. **Reducir variables** y agregar instrumentación (contadores, marcas de tiempo, asserts) antes de reescribir. Un cambio por vez. Si hay core dump, `idf.py coredump-info`.
6. **Reportar** en este orden: causa probable, evidencia (línea del log o frame), siguiente paso de diagnóstico, arreglo propuesto.

## Logs

Tags estables por módulo. Registrar transiciones de estado, códigos de error, reintentos y tiempos; no texto genérico. Evitar logs en bucles calientes y en ISR (alteran el timing y, por UART0 con streaming activo, ensucian la salida: ver `NOTAS_SERIAL.md`). Bajar el ruido de los componentes del sistema de forma selectiva, no apagar todo.

## Antes de dar algo por terminado

- Compilar con `idf.py build` desde una terminal de ESP-IDF (el hook de fin de tarea lo hace si `idf.py` está en el PATH). Revisar las advertencias.
- Decir qué **no** se verificó en la placa.
- Si el cambio toca pines o la FSM, correr `pin-map-check` / `fsm-check`.
