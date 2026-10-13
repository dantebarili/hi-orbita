# Prueba de audio por WebSocket

Rama `integración`. No sustituye la prueba UART ni implementa todavía la FSM del producto.

## Recorrido

`I2S → pipeline de Dante → ring out → tarea sink → audio_ws_sink → orbita_ws → servidor → WAV`

La captura, AFE, pines, ring y `audio_sink.h` no cambian. `send(ctx,msg)` corre en la tarea sink existente; no se envía desde los callbacks de captura. El adaptador serializa y envía antes de devolver: nunca conserva `msg->audio`. Hay un solo consumidor del ring. Wi-Fi/WebSocket provienen del laboratorio local; ese directorio sigue independiente.

`Orbita integration` permite elegir UART (predeterminado, conserva `test_loop`) o WebSocket. En WebSocket no se compila `test.c`: no se instalan sus drivers, buffers ni callbacks. La red obtiene IP antes de encender I2S. Un servidor ausente no bloquea la captura, pero sus mensajes se descartan y cuentan.

## Ejecutar

1. Detener el servidor anterior del laboratorio si ocupa el puerto 8765. En una PowerShell, desde la raíz de **hi-orbita**, usar un Python con `websockets==17.1`:

   ```powershell
   python -m pip install -r tools/requirements-websocket.txt
   python tools/ws_audio_server.py
   ```

   En esta computadora también sirve el Python ya instalado en el laboratorio:

   ```powershell
   & ..\websocket-lab\.venv\Scripts\python.exe tools/ws_audio_server.py
   ```

2. En **IDF PowerShell v5.5.5**, entrar en `hi-orbita\firmware`:

   ```powershell
   $env:PYTHONUTF8 = "1"
   idf.py menuconfig
   ```

   En `Orbita integration`: seleccionar WebSocket, configurar SSID, contraseña, URI `ws://IP-DE-LA-PC:8765` y duración (60 s). La PC y la ESP deben tener acceso entre sí por la red local; permitir al servidor en el firewall de la red privada. La IP puede cambiar.

   En `Orbita pipeline`: primera comparación sin AFE (mono L); luego activar AFE y modo VAD 4 para probar la salida procesada. La decisión definitiva L/mezcla sigue abierta. El VAD se informa; no se eliminan silencios.

   Si se reutiliza un `sdkconfig` antiguo, verificar `CONFIG_I2S_ISR_IRAM_SAFE=y` buscando `I2S_ISR_IRAM_SAFE` con `/`. Los defaults no reemplazan valores existentes. Se mantienen flash 16 MB y PSRAM octal. No copiar configuraciones de otra placa.

3. Compilar. Cuando se decida probar físicamente, cargar y abrir monitor (COM9 era el puerto observado; comprobarlo):

   ```powershell
   idf.py build
   idf.py -p COM9 flash monitor
   ```

   `PYTHONUTF8` evita un error de impresión de ESP-SR en Windows. No hace falta borrar NVS para esta prueba.

4. Hablar frente al micrófono. El servidor anuncia inicio, caudal y `WAV GUARDADO`. La ESP muestra `audio_ws: WAV GUARDADO`. Abrir el WAV en `tools/prueba_websocket/` y su JSON. Ambos quedan fuera de Git.

Una toma completa por arranque; reiniciar ESP para repetir. Después sigue drenando el ring sin enviar. 60 s nominales terminan al final de un mensaje: normalmente **60,032 s**, 469 mensajes de 2048 muestras. El PCM necesita **32 000 B/s**, más header y protocolo de red.

## Qué comprueba y qué no

- `.wav` / `complete`: fin confirmado, cantidad y CRC acumulado coinciden, sin pérdidas detectadas. `.gaps.wav` / `complete_with_gaps`: se terminó pero hubo huecos o errores de captura. `.incomplete.wav`: faltó fin válido (corte, error o cierre). Los parciales son reproducibles tras su cierre normal; un corte abrupto del proceso/PC puede dejar el header WAV sin finalizar.
- Los huecos se anotan en JSON, **no se rellenan**: el WAV concatena lo recibido. CRC verifica transporte/guardado; escuchar verifica calidad audible. Ninguno prueba por sí solo que el micrófono capture correctamente.
- Los contadores de error son **desde el arranque**, incluyendo antes de `ready`: clasificación conservadora; un error anterior puede marcar una toma posterior. El AFE puede perder audio internamente sin reportarlo; `clean` significa sin pérdidas **detectadas**, no una garantía absoluta.
- El JSON incluye secuencias, pérdidas, CRC, contadores del pipeline, máximo uso del ring, máximos de tiempo, mínimos libres de pila y RAM libre al final. `send_us_max_previous` excluye la llamada final aún en curso e incluye el handshake inicial; no es promedio ni latencia exclusiva del socket. Aún hay que medir estos valores en la placa con Wi-Fi + AFE.
- Si falla un envío no se repite: pudo haber llegado. Tras reconectar se inicia otro archivo con ID nuevo. `dropped_before` cuenta muestras descartadas o de entrega incierta antes del nuevo intento; no es una medida exacta de pérdida física. Un ACK final perdido puede dejar un WAV completo en PC y provocar otra toma en ESP.
- No hay retención hasta ACK ni reenvío (A6 pendiente). Aunque el ring almacena unos 32,8 s, el sink descarta ante falta de conexión. Las esperas de red afectan al sink, no directamente a la tarea de captura. El contador genérico `msgs_enviados` también incluye mensajes drenados después de terminar; los mensajes realmente enviados están en el resumen de la toma.

## Contrato provisional del cable

Texto JSON `start` describe `orbita-pcm-v1`, ID aleatorio de 64 bits en 16 dígitos hex, PCM mono s16le/16 kHz, objetivo de muestras y origen. El servidor responde `ready <id>`. Cada binario contiene este header seguido del PCM; todo little-endian, sin enviar una struct C directamente:

| Offset | Campo | Bytes |
|---|---|---|
| 0 | `ORBA` | 4 |
| 4 | versión 1 / tamaño header 40 | 1 + 1 |
| 6 | flags reservados, cero | 2 |
| 8 | ID de toma | 8 |
| 16 | índice de mensaje, desde cero | 4 |
| 20 | secuencia del primer slot | 4 |
| 24 | muestras perdidas antes | 4 |
| 28 | slots / muestras / slots voz / slots VAD válido | 2 × 4 |
| 36 | CRC32 del PCM del mensaje (zlib) | 4 |

Máximo 4 slots = 2048 muestras = **4136 bytes** con header. JSON `end` contiene muestras/mensajes/CRC acumulado y estadísticas; el servidor valida, cierra WAV, guarda reporte y responde `saved <id> clean|gaps`. Es confirmación de archivo cerrado, no garantía de persistencia ante pérdida de alimentación del disco.

`ready/saved` no son `srv_respuesta_recibida`. Toma e ID no son sesión de médico ni segmento FSM. El transporte de laboratorio conserva reconexión fija de 5 s sin límite, ping/pong 20/20 s y espera de controles de 10 s; difiere de la FSM (reintentos exponenciales limitados, heartbeat 10/3, respuesta 5 s). Capturar después de obtener IP tampoco resuelve la escucha local del producto. Estas diferencias no modifican el vault.

Antes de backend real: acordar canal/mezcla, header, sesión+segmento, cierre, ACK/retención, autenticación/TLS y prioridad urgente. Poder recibir mientras se envía no decide que la FSM sea concurrente. El servidor actual es de laboratorio en LAN, sin IA ni autenticación.

## Verificación sin placa

```powershell
python -m unittest discover -s tools/tests -v
```

Prueba guardado exacto, CRC, huecos, errores DMA, orden, wrap, fin inválido, parciales y socket real en localhost con puerto libre. `test_audio_ws_wire.c` se compila con un compilador C nativo junto con `audio_ws_wire.c`; recibe una ruta para escribir el paquete de referencia. Con `ORBITA_WIRE_FIXTURE` apuntando a ese archivo, Python verifica igualdad byte a byte del serializador C.

Para volver a la prueba de audio anterior, seleccionar UART y recompilar/cargar. Los cambios de `menuconfig` son locales: no subir SSID/contraseña, `sdkconfig`, binarios ni grabaciones.

Validación local (13/10/2026): ESP-IDF 5.5.5, ESP-SR 2.6.0 y cliente WebSocket 1.8.0. Compiladas las variantes WebSocket sin AFE, WebSocket con AFE/VAD 4 y UART. Pasaron 12 pruebas Python (incluido fixture producido por C) y la prueba C nativa. Revisiones FSM y embedded completadas; se corrigió el manejo del reporte ante fallo de disco. No se flasheó ni midió esta integración en hardware: quedan toma audible y mediciones de RAM/pila/DMA con Wi-Fi + AFE.
