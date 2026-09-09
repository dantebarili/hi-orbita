# Instrucciones de flasheo y prueba (temporal — borrar después)

Guía paso a paso para la primera vez. Vas a usar **dos cables/puertos distintos** (ver [firmware/NOTAS_SERIAL.md](firmware/NOTAS_SERIAL.md) para el porqué): uno para flashear/ver logs, otro para el protocolo de comandos+datos.

## 0. Conectar la placa

Conectá **un solo cable primero** (el del chip puente USB-UART, suele ser el que dice "UART" o el conector más "genérico" de la placa) y abrí el Administrador de Dispositivos → Puertos (COM y LPT) para ver qué puerto apareció (ej. `COM3`).

Después conectá el **segundo cable** (USB nativo, directo al chip, suele decir "USB" en la placa) y fijate qué otro puerto COM nuevo aparece (ej. `COM5`).

Si tenés dudas de cuál es cuál, desconectá uno a la vez y mirá cuál puerto desaparece del Administrador de Dispositivos.

**Anotá los dos:**
- Puerto A (UART0, chip puente) → logs/flasheo → `COM___`
- Puerto B (USB-Serial-JTAG nativo) → comandos/datos → `COM___`

## 1. Abrir una terminal con ESP-IDF activado

Desde el menú Start de Windows, buscá **"ESP-IDF PowerShell"** (o "ESP-IDF CMD") y abrila. Ya tiene el entorno (Python, toolchain, `idf.py`) activado — no hace falta activar nada a mano.

Navegá a la carpeta del firmware:

```powershell
cd D:\Users\Dante\Desktop\Orbita\hi-orbita\firmware
```

## 2. Setear el target (solo la primera vez)

```powershell
idf.py set-target esp32s3
```

## 3. Compilar

```powershell
idf.py build
```

Si hay errores de compilación, pegámelos y los vemos juntos antes de seguir.

## 4. Flashear (usa el Puerto A — UART0)

```powershell
idf.py -p COM3 flash
```

(Reemplazá `COM3` por el puerto A que anotaste en el paso 0.)

## 5. Ver los logs (mismo Puerto A)

```powershell
idf.py -p COM3 monitor
```

Deberías ver los `ESP_LOGI` de inicialización (I2S, buffer PSRAM reservado, etc.). Para salir del monitor: `Ctrl+]`.

**Importante:** este puerto (A) es de **solo lectura de logs** — no manda comandos ni recibe audio. Podés dejarlo abierto en una ventana mientras trabajás con el otro puerto en paralelo, para ver si algo falla (`ESP_LOGE`) durante las pruebas.

## 6. Preparar el entorno de Python (una sola vez)

En **otra** terminal (puede ser PowerShell normal, no hace falta que sea la de ESP-IDF) desde la raíz del proyecto:

```powershell
cd D:\Users\Dante\Desktop\Orbita\hi-orbita\tools
pip install -r requirements.txt
```

## 7. Correr el cliente (usa el Puerto B — USB nativo)

```powershell
python orbita_serial.py COM5
```

(Reemplazá `COM5` por el puerto B que anotaste en el paso 0.)

Debería abrirse una ventana de gráfico con el RMS en vivo de ambos canales. Escribí `g` y Enter en esa misma terminal para disparar una grabación de 10 segundos — al terminar, se guarda un archivo `orbita_AAAAMMDD_HHMMSS.wav` en la carpeta `tools/`.

## Qué mirar / posibles problemas

- **Si `idf.py monitor` no muestra nada:** revisá que estés usando el Puerto A (UART0), no el B.
- **Si el gráfico de Python no se mueve:** revisá que estés usando el Puerto B, y que no tengas `idf.py monitor` abierto sobre ese mismo puerto B a la vez (aunque en teoría no debería estar ahí).
- **Si al escribir `g` no pasa nada:** fijate en la ventana del Puerto A (logs) si aparece algún `ESP_LOGE` de error de lectura I2S.
- **Errores de compilación en el paso 3:** son los que más nos interesa que me muestres — hay configuraciones (`sdkconfig.defaults`) que todavía no probamos con un build real (ver nota en ese archivo).
