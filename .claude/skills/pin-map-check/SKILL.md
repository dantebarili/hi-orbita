---
name: pin-map-check
description: Chequea cualquier GPIO propuesto o existente en el firmware contra los guardrails de pines de CLAUDE.md (flash, PSRAM, JTAG, UART0, strapping). Usar antes de proponer, revisar, o aceptar un GPIO_NUM_* nuevo, o al tocar audio_capture.h, main.c, o cualquier archivo que declare pines.
user-invocable: false
---

Guardrails de pines (CLAUDE.md — fijos, no negociables porque el módulo N16R8 va soldado 1:1 en la PCB final):

| Rango / pin | Uso | Regla |
|---|---|---|
| GPIO 26–32 | Flash quad | Nunca mapear periféricos ahí |
| GPIO 33–37 | PSRAM octal | Nunca mapear periféricos ahí |
| GPIO 19/20 | USB-Serial-JTAG nativo | Reservado — canal de comandos/datos (`tools/orbita_serial.py`) |
| GPIO 43/44 | UART0 (logs) | Reservado — no reasignar, ahí van los `ESP_LOGI/ESP_LOGE` |
| GPIO 0, 3, 45, 46 | Strapping | Evitar salvo necesidad explícita; si se usa, leer el datasheet del ESP32-S3 primero y decirlo explícitamente |
| GPIO 4/5/6 | I2S mics (BCLK, WS, DIN) | En uso hoy — no reasignar sin que el usuario lo pida |

Antes de proponer o aceptar un `GPIO_NUM_*` nuevo (NFC, LED, botones, amplificador clase D, microSD, programación/debug):

1. Verificar que no caiga en ninguno de los rangos reservados de la tabla.
2. Si el pin es strapping, decir explícitamente que lo es y por qué se justifica igual (o recomendar otro).
3. Recordar que todo pin definido debe funcionar igual en el DevKit y en el módulo soldado en PCB — no proponer algo que dependa de hardware exclusivo del DevKit (ej. LEDs/botones ya cableados en la placa de desarrollo que no van a estar en producción).

Si la asignación de pines completa todavía no está cerrada (ver CLAUDE.md, sección "Infraestructura de placa" y "Pines"), señalarlo en vez de asumir que un pin libre lo sigue estando — confirmar contra el estado actual del código (`audio_capture.h`, `main.c`) antes de recomendar uno.
