# Audio por WebSocket (protocolo `orbita-pcm-v2`, continuo)

Rama `juani`. Prototipo alfa: audio mono continuo al servidor (`plan_prototipo_alfa.md`). No implementa la FSM del producto ni la wake word local; incluye una prueba opcional de detección por transcripción en el servidor.

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

## Prueba de voz de vuelta al parlante (sin STT)

### Aislar primero el downlink con una senal conocida

`python tools/ws_audio_server.py --test-tone` envia una vez por conexion el mismo tono del ensayo local de Dante (1 kHz, 3 s, pico PCM 16000), por el camino normal `play/PLAY/play_end`. No espera audio util de los microfonos; el uplink sigue activo. La ganancia del firmware sigue en 0,25. Alternativa para voz conocida: `--play-wav archivo.wav` (mono, 16 kHz, PCM 16-bit, hasta 15 s). Ambos modos son excluyentes con replay y STT. Se guarda `test_play_*.wav` como referencia.

El servidor muestra `PLAY TX: id=... bytes=... crc32=...`; el firmware actualizado muestra `PLAY RX` con el mismo ID, bytes y CRC al aceptar el fin (PCM antes de ganancia). Para el tono: `bytes=96000 crc32=6e479b9f`. Si coinciden, los bytes de red llegaron intactos hasta la entrada de playback; no verifica el contenido posterior del ring ni la salida electrica I2S. Si no suena, enfocar la salida bajo operacion simultanea de mics/red/parlante. Para esta comparacion hay que recompilar y flashear el firmware con el log RX.

Integracion 15/10 en `juani` con `origin/dante`: conserva su ganancia Q15 (8192) y prueba de tono archivada. `main` inicia playback una sola vez antes del pipeline y registra `RAM antes de playback`, `despues de playback`, `despues de pipeline/AFE` y `arranque completo` (libre interna, mayor bloque interno y PSRAM). Los valores reales se deben recoger del monitor, aun pendientes.

```powershell
python tools/ws_audio_server.py --replay-window 2 12 --segment-seconds 10
```

Una vez por conexion v2, recorta los segundos 2 a 12 del PCM recibido (no desde el boot), guarda `replay_*.wav` en la carpeta de salida y devuelve esos 10 s como `play` + binarios `PLAY` + `play_end`. El recorte es exacto a nivel de muestra; si el uplink tiene huecos, se cuenta solo el PCM que llego. Son 320000 B de PCM en 79 mensajes binarios de hasta 4096 B, enviados en rafaga. La tarea de recepcion del servidor sigue recibiendo y confirmando el audio del ESP durante el envio. No requiere Whisper ni un WAV externo; es incompatible con `--stt-window-seconds`.

Hablar durante ese intervalo y despues escuchar la repeticion. El servidor debe mostrar `Playback terminado` con razon `end`, no `play_reject` ni `underrun`; el uplink debe seguir avanzando. Si corta la conexion antes del segundo 12, no devuelve una toma parcial. Reiniciar la ESP o reconectar abre una nueva sesion y repite la prueba. Los mics pueden captar el parlante, pero el recorte se devuelve una sola vez: no hay bucle de eco. Validacion fisica pendiente.

## Prueba local de STT y respuesta por audio

El firmware mantiene el flujo v2 abierto y no manda un `end`. Para probar el recorrido sin cambiar la ESP, el servidor analiza **una sola ventana inicial** (8 s por ejemplo), mientras la conexión sigue recibiendo audio. Instalar la dependencia opcional y ejecutar:

```powershell
python -m pip install -r tools/requirements-speech.txt
python tools/ws_audio_server.py --stt-window-seconds 8
```

El modelo `tiny` de faster-whisper se descarga la primera vez y corre localmente en CPU. El servidor imprime la transcripción y busca la palabra completa `Órbita` sin distinguir mayúsculas ni tilde. Si la encuentra, manda `orbita` y reproduce por WebSocket `tools/respuesta_orbita.wav`, si existe. Ese WAV todavía falta: al recibirlo, convertirlo a PCM mono, 16 kHz, 16-bit y guardarlo con ese nombre. Sin el WAV, la detección funciona y queda registrada, pero no hay audio de respuesta. La ventana se analiza una vez por conexión; no es escucha continua de wake word ni STT en tiempo real.

## Downlink: respuesta por el parlante (integrado en `juani`, prueba de placa pendiente)

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
- **RX PLAY integrado con `orbita_playback` en `juani`:** buffer propio de 4096 B en PSRAM, fuera de la cola de 512 B. `audio_ws_playback` copia PCM al ring del parlante, asocia el ID de 64 bits del servidor con el token de 32 bits de playback y reporta fin/corte. Contrato binario v1 propuesto; validar el header con el servidor y medir recepción/reproducción simultánea en placa. Detalle: [audio_ws_playback/README.md](firmware/components/orbita_audio_ws/README.md) y [orbita_ws/README.md](firmware/components/orbita_ws/README.md).
- Diferencias con la FSM (backoff 1-8 s sin tope de intentos, sin timeout de 5 s de respuesta ni `srv_*`) siguen sin acordar con Juani. No modifican el vault.
- Mono L (`mic_l`) o AFE: la decisión L/mezcla sigue abierta.

## Verificación

```powershell
python -m unittest discover -s tools/tests -v
```

34 pruebas (1 salteada: fixture C): protocolo v1, sesión v2, helpers STT/PLAY, tono conocido y recorte exacto con devolución PLAY por socket real, verificando que el uplink y los ACK sigan funcionando.

Estado en hardware (13/10/2026): warm-up, retención y corte de Wi-Fi validados con v1. **v2 probado en la placa**: sesión con `hello`/`ready`, 5 segmentos de 10 s, catch-up de 48 s de audio en 15 s tras un corte largo y huecos marcados (27 s perdidos al desbordar el ring). Falta: corte corto con la sesión en curso, tomas largas (10 min / 1 h) y mirar `sin_ack` y la pila del sink en el monitor.
