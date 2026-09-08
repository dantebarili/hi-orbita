# Notas — Dos puertos USB / Serial (Etapa 1)

## Por qué hay dos cables

El DevKit N16R8 tiene **dos periféricos serie distintos**, cada uno con su propio conector USB:

1. **UART0** — vía chip puente USB-UART (el puerto "clásico" de la placa).
2. **USB-Serial-JTAG nativo** — GPIO 19/20, conector USB nativo del propio chip.

Son dos cables, pero se conectan a la **misma PC** (dos puertos USB de tu compu, no dos computadoras). Windows los muestra como dos puertos COM distintos (ej. `COM3` y `COM5`).

## Por qué los separamos así

Si los logs (`ESP_LOGI`) y nuestro protocolo de comandos/datos compartieran el mismo periférico (USB-Serial-JTAG), el sistema instalaría el driver dos veces (una para consola, otra para nuestro código) y chocarían (`ESP_ERR_INVALID_STATE`).

Solución en `sdkconfig.defaults`:
```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```
Esto manda todos los logs por **UART0** y desactiva el espejo automático por USB-Serial-JTAG, dejando este último **libre para uso exclusivo nuestro**.

## Uso de cada cable

| Cable | Para qué | Cómo se abre |
|---|---|---|
| UART0 (chip puente) | Ver logs de debug (`ESP_LOGI`/`ESP_LOGE`) | `idf.py monitor` — solo lectura pasiva, no es parte del protocolo |
| USB-Serial-JTAG nativo (GPIO19/20) | Comandos (`'g'` para grabar) + datos (amplitud en vivo, audio) | Script propio en Python con `pyserial`, abriendo el COM que le corresponda (ej. `serial.Serial('COM5', ...)`) |

**Cómo identificar cuál COM es cuál:** conectar ambos cables, ver Administrador de Dispositivos → Puertos (COM y LPT); desconectar un cable a la vez para ver cuál puerto desaparece.

## Del lado del firmware

En `main.c`, `usb_serial_jtag_driver_install()` inicializa el periférico nativo para lectura/escritura propia (no confundir con el log, que ya sale solo por UART0 gracias a la config de arriba).
