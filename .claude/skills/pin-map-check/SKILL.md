---
name: pin-map-check
description: Chequea cualquier GPIO propuesto o existente en el firmware contra los pines asignados y reservados de orbita-obsidian/orbita_pinout.md (flash, PSRAM, JTAG, UART0, strapping). Usar antes de proponer, revisar, o aceptar un GPIO_NUM_* nuevo, o al tocar audio_capture.h, main.c, o cualquier archivo que declare pines.
user-invocable: false
---

Los pines asignados y los guardrails (fijos, no negociables porque el módulo N16R8 va soldado 1:1 en la PCB final) están en `orbita-obsidian/orbita_pinout.md`: leerlo primero, no hay otra lista.

Antes de proponer o aceptar un `GPIO_NUM_*` nuevo (NFC, LED, botones, amplificador clase D, microSD, programación/debug):

1. Verificar que no caiga en ninguno de los pines de "No usar" ni en uno ya asignado del pinout.
2. Si el pin es strapping, decir explícitamente que lo es y por qué se justifica igual (o recomendar otro).
3. Recordar que todo pin definido debe funcionar igual en el DevKit y en el módulo soldado en PCB — no proponer algo que dependa de hardware exclusivo del DevKit (ej. LEDs/botones ya cableados en la placa de desarrollo que no van a estar en producción).

Si la asignación de pines completa todavía no está cerrada (ver "Sin pin todavía" en `orbita-obsidian/orbita_pinout.md` y "Infraestructura de placa" en `CLAUDE.md`), señalarlo en vez de asumir que un pin libre lo sigue estando — confirmar contra el estado actual del código (`audio_capture.h`, `main.c`) antes de recomendar uno.
