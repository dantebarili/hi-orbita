# Banco PN532 I2C + IRQ / ESP32-S3

Proyecto ESP-IDF independiente, preparado sobre `main` en `d1915e3` (06/11/2026). No está conectado al arranque, audio, red, sesión ni FSM de `firmware/`. El componente se entrega para que el integrador lo pruebe y luego lo incorpore explícitamente.

**Integrado en `firmware/` (rama `integracion-nfc`, 06/11):** pines 17/18/21 y sesión en `orbita_audio_ws/audio_ws_nfc.c`. Este banco sigue con 10/11/12.

Estado: **lectura de tarjeta y funcionamiento con IRQ probados en placa y confirmados por Juani**. La versión IRQ compila con ESP-IDF 5.5.5. Las pruebas host validan tramas, checksum, truncados y UID.

## Cableado de este banco

Conectar sin alimentación. Selector del PN532 V3: **1 ON, 2 OFF**.

| PN532 | ESP32-S3 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO10 |
| SCL | GPIO11 |
| IRQ (tira larga) | GPIO12 |

RSTO queda sin conectar. Verificar que IRQ no supere 3,3 V; su entrada tiene pull-up interna. SDA/SCL usan las pull-up del módulo. La advertencia genérica de ESP-IDF sobre pull-up externas no es una medición de resistencia.

**Estos son pines del banco.** `orbita-obsidian/pines.md` propone 17/18/21 para el NFC del producto. Al integrar, acordar SDA/SCL/IRQ y actualizar ese documento; no copiar 10/11/12 como asignación definitiva de PCB. Los pines del banco están definidos en `main/main.c`.

## Compilar, cargar y observar

Abrir una terminal con ESP-IDF 5.5.5 activado y, desde la raíz del repo:

```powershell
cd experiments/nfc-pn532-irq
idf.py build
idf.py -p COM12 -b 115200 flash monitor
```

Reemplazar COM12 por el USB-UART real. Logs por UART0 a 115200; salir con Ctrl+]. Este banco configura ESP32-S3 y flash de 16 MB; no habilita PSRAM, Wi-Fi ni audio.

**Flashear reemplaza el programa de la placa y escribe la tabla de particiones de laboratorio (single factory).** Respaldar datos persistentes antes de usar una placa en servicio. Para volver a Órbita, compilar/flashear `firmware/` con su configuración y particiones originales.

Arranque esperado: `NFC IRQ: SDA=10 SCL=11 IRQ=12 activo bajo...`, luego `PN532 confirmado: firmware ...`. Acercar una tarjeta ISO14443A y comprobar `TARJETA uid=...`. Mantenerla apoyada no repite el mismo UID. Retirarla y volverla a acercar debe producir otra lectura; probar también una tarjeta distinta. La retirada se estima tras tres búsquedas sin detección: no garantiza presencia física.

Un timeout IRQ o respuesta inválida registra el error y detiene el banco, sin reset automático ni fallback a polling. Revisar las líneas `pn532`; cortar/reponer alimentación de ambas placas antes de reintentar. No escribe tarjetas ni autentica usuarios.

## Funcionamiento y guía de integración

- ISR de flanco descendente: solo entrega un semáforo mediante `xSemaphoreGiveFromISR` y cede CPU si corresponde. No usa I2C, logs, red ni memoria dinámica.
- `wait_ready` espera IRQ baja con timeout de 1 s y comprueba el nivel antes de dormir y después de cada aviso. Esto cubre IRQ ya activa y notificaciones anteriores; no consulta periódicamente el byte RDY.
- IRQ anuncia tanto ACK como respuesta. La tarea lee ambos por I2C y valida status, trama y contenido. Se conservan búsquedas finitas y pausas de 150 ms entre búsquedas: IRQ no elimina esa búsqueda periódica.
- `firmware/components/orbita_nfc` (el banco lo usa desde ahí, `EXTRA_COMPONENT_DIRS`): API `open(nfc, bus, irq_pin)`, `begin`, `read_uid`, `close` y `wait_card` (06/11: InAutoPoll, UM0701-02 §7.3.13; el PN532 busca solo y el IRQ avisa al encontrar tarjeta, sin polling desde el ESP; lo usa `firmware/`, este banco sigue con `read_uid`). Inicializar contexto a cero y mantenerlo vivo hasta cerrar; un solo llamador, sin cierre simultáneo con una operación. El llamador posee el bus.
- Driver de laboratorio: tramas normales de hasta 64 bytes, UID de 4/7/10 bytes, sin recuperación automática. Las llamadas bloquean la tarea con tiempos acotados; no llamarlas desde tareas/callbacks de audio o red.
- Al integrar, usar una tarea NFC de baja prioridad y enviar una **copia** del UID por cola al dueño de sesión. Acordar login/logout y el contrato de servidor con la FSM. El UID no prueba criptográficamente identidad.
- El servicio ISR GPIO es compartido y no se desinstala al cerrar. Esta ISR usa el servicio sin `ESP_INTR_FLAG_IRAM`; si el firmware ya instala un servicio con IRAM, adaptar handler, contexto y semáforo a ese contrato antes de integrarlo. GPIO12 debe quedar reservado a este lector durante la prueba.

## Prueba host del parser

Desde esta carpeta, con un compilador C instalado (sin depender de ESP-IDF):

```sh
mkdir -p test-build
cc -std=c11 -Wall -Wextra -Werror -I../../firmware/components/orbita_nfc tests/test_frame.c ../../firmware/components/orbita_nfc/pn532_frame.c -o test-build/test_frame
./test-build/test_frame
```

En Developer PowerShell de Visual Studio:

```powershell
New-Item -ItemType Directory -Force test-build | Out-Null
cl /nologo /W4 /WX /I ../../firmware/components/orbita_nfc tests/test_frame.c ../../firmware/components/orbita_nfc/pn532_frame.c /Fe:test-build/test_frame.exe /Fo:test-build/
.\test-build\test_frame.exe
```

## Referencias

- [NXP UM0701-02](https://www.nxp.com/docs/en/user-guide/141520.pdf): §6.2 tramas/I2C, §6.3.3 handshake IRQ (ACK y respuesta), §7.2.10 SAM, §7.3.1 reintentos y §7.3.5 Type A.
- [Elechouse PN532 V3](https://www.elechouse.com/elechouse/images/product/PN532_module_V3/PN532_%20Manual_V3.pdf), pág. 3: alimentación y selector de interfaz.
