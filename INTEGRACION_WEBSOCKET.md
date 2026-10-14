# Audio por WebSocket (protocolo `orbita-pcm-v2`, continuo)

Rama `integración`. Prototipo alfa: audio mono continuo al servidor (`plan_prototipo_alfa.md`). No implementa la FSM del producto ni la wake word.

## Recorrido

`I2S → pipeline → ring out → tarea sink → audio_ws_sink → orbita_ws → servidor → WAV`

`send(ctx,msg)` corre en la tarea sink; nunca desde los callbacks de captura. El adaptador serializa y envía antes de volver y no guarda `msg->audio`.

Arranque (`main.c`): primero I2S, pipeline y sink; después Wi-Fi y WebSocket, **sin esperar IP**. En el menú `Orbita integration` se elige UART (predeterminado, conserva `test_loop`) o WebSocket; en WebSocket no se compila `test.c`.

## Comportamiento

- **Warm-up de 5 s** (`WARMUP_MS`): se descarta el arranque (escalón de continua del micrófono y overflows del DMA al crear las tareas). Los overflows de ese tramo salen en el log como `dma_overflows durante el arranque`.
- **Retención (S1):** sin conexión, el sink espera con el mensaje en la mano y el ring acumula (~32,8 s); si se llena, descarta lo más viejo y lo marca en `lost`.
- **Reconexión (S2):** WebSocket con backoff 1-2-4-8 s (tope 8 s; vuelve a 1 s con el primer `ack` de la sesión); Wi-Fi reintenta cada 5 s. Heartbeat 10 s / pong 3 s (como la FSM). Un `send` fallido lo cierra la propia biblioteca. Si su tarea muere (`FINISH`), el sink la reinicia (`orbita_ws_keepalive`). La tarea de red va fija al core 0 y el sink tiene 6 KB de pila.
- **Ventana de acks y timeout:** cada envío espera hasta 5 s (antes 1 s: abortaba conexiones que TCP habría recuperado solo). No se mandan más de 32 mensajes (~4 s) sin confirmar: el drenado del atraso sigue el ritmo del servidor. Si el servidor no confirma en 5 s, la ventana se apaga en esa sesión (log `sigo sin ventana`).
- **Un solo `hello` por conexión:** el servidor rechaza el segundo. Si no llega el `ready`, solo se vuelve a esperar; si falla un envío con la conexión intacta, se reintenta el mismo paquete.
- Tras reconectar se abre sesión nueva (id nuevo) y se reenvía el mensaje retenido. El servidor mide el hueco entre sesiones por `seq`.
- Cada 30 s el monitor imprime una línea: `msgs`, `ack`, `sin_ack`, `reconex`, `espera_max`, `perdidas`, `ring`, `dma`, `i2s`, `pila_sink`, `ram_int`.

## Ejecutar

1. En una PowerShell, desde la raíz del repo (sirve `websockets` 16 o el 17.1 de `tools/requirements-websocket.txt`):

   ```powershell
   python tools/ws_audio_server.py --segment-seconds 10
   ```

   **Ojo con OneDrive:** la carpeta de salida por defecto (`tools/prueba_websocket`) queda dentro de OneDrive si el repo está en el Escritorio. En una prueba de 10 min el servidor se congeló ~173 s (sospecha: escritura bloqueada por la sincronización). Para tomas largas usar `--output` fuera de OneDrive (por ejemplo `C:\orbita_pruebas`).

   Abrirlo **antes** de la ESP. Permitir Python en el firewall de la red privada. `Ctrl+C` cierra el segmento en curso y deja el WAV legible.

2. En la terminal de ESP-IDF 5.5.5, en `firmware`: `idf.py menuconfig` → `Orbita integration`: WebSocket, SSID, contraseña y URI `ws://IP-DE-LA-PC:8765` (la IP de la PC puede cambiar). La opción "Segundos nominales por toma" ya no se usa. Si el `sdkconfig` es viejo, verificar `CONFIG_I2S_ISR_IRAM_SAFE=y`. No subir SSID/contraseña ni `sdkconfig`.

3. `idf.py build`, luego `idf.py -p COMx flash monitor`.

## Qué se guarda

En `tools/prueba_websocket/` (fuera de Git), por sesión (una por conexión):

```
sesion_<fecha>_<id>_000.wav / .json   segmentos de --segment-seconds (30 s por defecto)
sesion_<fecha>_<id>.json              resumen: mensajes, duración, huecos, hueco vs sesión anterior
```

El segmento en curso no se puede abrir hasta que se cierra (por rotación o `Ctrl+C`); un cierre brusco del proceso lo deja con el header sin completar. Los huecos se anotan en el JSON, no se rellenan. Para analizar: `python tools/analizador_wav_generico.py tools/prueba_websocket/`.

## Contrato del cable (provisional)

| Quién | Mensaje | Cuándo |
|---|---|---|
| ESP → srv (JSON) | `hello` {protocol `orbita-pcm-v2`, id 16 hex, rate 16000, channels 1, format s16le, source `mic_l`/`afe`} | una vez por conexión |
| srv → ESP (JSON) | `{"type":"ready","id":…}` | respuesta al `hello` |
| ESP → srv (binario) | header 40 B + PCM | cada 128 ms, sin fin |
| srv → ESP (JSON) | `{"type":"ack","index":N}` | cada 8 mensajes (~1 s) |

Header (little-endian, ya no cambia respecto de v1): `ORBA`, versión 1, tamaño 40, flags 0, id de **sesión** (8 B), índice de mensaje (empieza en 0 y sigue sin cortes), `seq` del primer slot, muestras perdidas antes, slots/muestras/slots con voz/slots VAD válido, CRC32 del PCM. Máximo 4 slots = 2048 muestras = 4136 B. El servidor rechaza índices repetidos o salteados, CRC malo o id distinto.

El servidor sigue aceptando `start`/`end` (v1, toma fija) si el primer mensaje es `start`; el firmware ya no lo usa.

## Downlink: respuesta por el parlante (acordado 14/10, a implementar)

| Quién | Mensaje | Para qué |
|---|---|---|
| srv → ESP (JSON ≤ 512 B) | `orbita` {id, at_index} | El servidor detectó "Órbita" en la transcripción. El ESP lo registra y **enciende el LED de conversación**. |
| srv → ESP (JSON) | `conversation_end` {id} | Fin de la conversación: apaga el LED. (Nombre a confirmar con Juani.) |
| srv → ESP (JSON) | `play` {play_id, rate 16000, format s16le} | Viene una respuesta. `bytes` es opcional: con TTS en streaming no se conoce el total. |
| srv → ESP (binario) | header `PLAY` (play_id, seq, largo) + PCM, ≤ 4 KB | Audio del parlante, en streaming. El TTS puede entregar chunks de ~2 s; el ESP solo ve frames. |
| srv → ESP (JSON) | `play_end` {play_id} | No viene más audio de esa respuesta. |
| srv → ESP (JSON) | `play_stop` {play_id} | Cortar ya (el servidor cancela). |
| ESP → srv (JSON) | `play_done` {play_id, reason: end/stopped/underrun, played_ms} | Terminó o se cortó. |
| ESP → srv (JSON) | `play_reject` {play_id} | No cabe en el buffer. |
| ESP → srv (JSON, Fase 2) | `barge_in` {play_id} | El usuario interrumpió (AEC + VAD en el ESP); el ESP ya cortó localmente. |

- **Reproducción en tiempo real:** el ESP junta un prebuffer (inicial 400 ms) y reproduce mientras sigue llegando; si se vacía, se registra `underrun`. El servidor puede mandar más rápido que tiempo real; el buffer en PSRAM (máximo de una respuesta, hoy 15 s = 480 KB) absorbe la diferencia.
- **Mic durante el parlante:** el ESP sigue mandando mensajes con el flag de "silencio salteado" (S8) y sin audio útil; el servidor los ignora. Sin AEC no hay interrupción por voz.
- **Reanudar tras reconexión:** el `ready` trae `last_index` (último mensaje recibido) y el ESP retoma desde `last_index + 1`. El ring conserva lo enviado y sin `ack` (tercer cursor, A6).

## Límites

- Hoy no hay reenvío desde el último `ack` (se hace en el alfa, ver arriba): lo perdido por un corte largo (>~32 s) queda como hueco. Los `ack` hoy solo se cuentan.
- Una toma cortada por reconexión termina en una sesión y sigue en otra; sin autenticación ni TLS (LAN de laboratorio). Envío ≤ 8 KiB; mensajes **recibidos** por la cola RX ≤ 512 B (`ORBITA_WS_MAX_CONTROL_BYTES`, S4: libera ≈ 46 KB de RAM interna), cola RX de 4.
- **Para Juani:** los frames binarios `PLAY` (hasta 4 KB) ya no caben en la cola RX. Hay que darles un camino de recepción propio (buffer propio, sin pasar por la cola; `ws_rx` hoy rechaza todo lo > 512 B). Un `PLAY` que llegue antes de eso se descarta con "Mensaje RX rechazado" en el log.
- Diferencias con la FSM (backoff 1-8 s sin tope de intentos, sin timeout de 5 s de respuesta ni `srv_*`) siguen sin acordar con Juani. No modifican el vault.
- Mono L (`mic_l`) o AFE: la decisión L/mezcla sigue abierta.

## Verificación

```powershell
python -m unittest discover -s tools/tests -v
```

22 pruebas (1 salteada: fixture C): protocolo v1, sesión v2 (continuidad, ack, rotación de segmentos, huecos, hueco entre sesiones, rechazos) y socket real en localhost.

Estado en hardware (13/10/2026): warm-up, retención y corte de Wi-Fi validados con v1. **v2 probado en la placa**: sesión con `hello`/`ready`, 5 segmentos de 10 s, catch-up de 48 s de audio en 15 s tras un corte largo y huecos marcados (27 s perdidos al desbordar el ring). Falta: corte corto con la sesión en curso, tomas largas (10 min / 1 h) y mirar `sin_ack` y la pila del sink en el monitor.
