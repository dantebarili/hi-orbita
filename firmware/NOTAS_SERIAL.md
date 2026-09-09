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
| USB-Serial-JTAG nativo (GPIO19/20) | Comandos (`'g'` para grabar) + datos (amplitud en vivo, audio) | `tools/orbita_serial.py`, abriendo el COM que le corresponda (ej. `serial.Serial('COM5', ...)`) |

**Cómo identificar cuál COM es cuál:** conectar ambos cables, ver Administrador de Dispositivos → Puertos (COM y LPT); desconectar un cable a la vez para ver cuál puerto desaparece.

## Del lado del firmware

En `main.c`, `usb_serial_jtag_driver_install()` inicializa el periférico nativo para lectura/escritura propia (no confundir con el log, que ya sale solo por UART0 gracias a la config de arriba).

---

## Guía paso a paso: flasheo y prueba (primera vez)

### 0. Conectar la placa

Conectá **un solo cable primero** (el del chip puente USB-UART, suele ser el que dice "UART" o el conector más "genérico" de la placa) y abrí el Administrador de Dispositivos → Puertos (COM y LPT) para ver qué puerto apareció (ej. `COM3`).

Después conectá el **segundo cable** (USB nativo, directo al chip, suele decir "USB" en la placa) y fijate qué otro puerto COM nuevo aparece (ej. `COM5`).

Si tenés dudas de cuál es cuál, desconectá uno a la vez y mirá cuál puerto desaparece del Administrador de Dispositivos.

**Anotá los dos:**
- Puerto A (UART0, chip puente) → logs/flasheo → `COM___`
- Puerto B (USB-Serial-JTAG nativo) → comandos/datos → `COM___`

### 1. Abrir una terminal con ESP-IDF activado

Desde el menú Start de Windows, buscá **"ESP-IDF PowerShell"** (o "ESP-IDF CMD") y abrila. Ya tiene el entorno (Python, toolchain, `idf.py`) activado — no hace falta activar nada a mano.

Navegá a la carpeta del firmware:

```powershell
cd D:\Users\Dante\Desktop\Orbita\hi-orbita\firmware
```

### 2. Setear el target (solo la primera vez)

```powershell
idf.py set-target esp32s3
```

### 3. Compilar

```powershell
idf.py build
```

Si hay errores de compilación, pegámelos y los vemos juntos antes de seguir.

### 4. Flashear (usa el Puerto A — UART0)

```powershell
idf.py -p COM3 flash
```

(Reemplazá `COM3` por el puerto A que anotaste en el paso 0.)

### 5. Ver los logs (mismo Puerto A)

```powershell
idf.py -p COM3 monitor
```

Deberías ver los `ESP_LOGI` de inicialización (I2S, buffer PSRAM reservado, etc.). Para salir del monitor: `Ctrl+]`.

**Importante:** este puerto (A) es de **solo lectura de logs** — no manda comandos ni recibe audio. Podés dejarlo abierto en una ventana mientras trabajás con el otro puerto en paralelo, para ver si algo falla (`ESP_LOGE`) durante las pruebas.

### 6. Preparar el entorno de Python (una sola vez)

En **otra** terminal (puede ser PowerShell normal, no hace falta que sea la de ESP-IDF) desde la raíz del proyecto:

```powershell
cd D:\Users\Dante\Desktop\Orbita\hi-orbita\tools
pip install -r requirements.txt
```

### 7. Correr el cliente (usa el Puerto B — USB nativo)

```powershell
python orbita_serial.py COM5
```

(Reemplazá `COM5` por el puerto B que anotaste en el paso 0.)

Debería abrirse una ventana de gráfico con el RMS en vivo de ambos canales. Escribí `g` y Enter en esa misma terminal para disparar una grabación de 10 segundos — al terminar, se guarda un archivo `orbita_AAAAMMDD_HHMMSS.wav` en la carpeta `tools/`.

### Qué mirar / posibles problemas

- **Si `idf.py monitor` no muestra nada:** revisá que estés usando el Puerto A (UART0), no el B.
- **Si el gráfico de Python no se mueve:** revisá que estés usando el Puerto B, y que no tengas `idf.py monitor` abierto sobre ese mismo puerto B a la vez (aunque en teoría no debería estar ahí).
- **Si al escribir `g` no pasa nada:** fijate en la ventana del Puerto A (logs) si aparece algún `ESP_LOGE` de error de lectura I2S.
- **Errores de compilación en el paso 3:** son los que más nos interesa que me muestres — hay configuraciones (`sdkconfig.defaults`) que todavía no probamos con un build real (ver nota en ese archivo).
