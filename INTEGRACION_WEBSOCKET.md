# Audio por WebSocket (protocolo `orbita-pcm-v2`, continuo)

Prototipo alfa (`plan_prototipo_alfa.md`): audio mono continuo al servidor y respuesta por el parlante cuando el servidor la manda. No implementa la FSM del producto ni la wake word local. Mientras no exista el backend real, `tools/ws_audio_server.py` hace de servidor.

## Recorrido

Subida: `I2S → pipeline → ring out → tarea sink → audio_ws_sink → orbita_ws → servidor → WAV`
Bajada: `servidor → orbita_ws (ws_downlink) → audio_ws_playback → orbita_playback → I2S → parlante`

`send(ctx,msg)` corre en la tarea sink; nunca desde los callbacks de captura. El adaptador serializa y envía antes de volver y no guarda `msg->audio`.

Arranque (`main.c`): I2S de los mics, parlante (su ring en PSRAM va antes que el AFE), pipeline y sink; después Wi-Fi y WebSocket, **sin esperar IP**. En `Orbita integration` → "Modo del firmware": prototipo por WebSocket (predeterminado) o captura por UART (estudio de audio, `test.c` + `tools/orbita_serial.py`).

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

## Demo actual: devolución de un tramo por el parlante

Mientras no haya backend, la demo muestra el recorrido completo: el ESP manda audio sin parar y el servidor le devuelve un tramo de lo que escuchó.

```powershell
python tools/ws_audio_server.py --replay-window 2 12 --segment-seconds 10
```

Una vez por conexión, el servidor recorta los segundos 2 a 12 del PCM recibido (contados desde el `hello`, no desde el boot), lo guarda como `replay_*.wav` y lo devuelve con `play` + frames `PLAY` (79 de hasta 4 KB, en ráfaga) + `play_end`. El uplink y los `ack` siguen durante el envío. Hablar en ese tramo y escuchar la repetición; el servidor debe mostrar `Playback terminado` con razón `end`. Reiniciar el ESP (o reconectar) abre otra sesión y repite la demo. Los mics oyen el parlante, pero el tramo se devuelve una sola vez: no hay bucle de eco.

Otra opción: `--play-wav archivo.wav` (mono, 16 kHz, 16 bit, hasta 15 s) envía un audio conocido una vez por conexión.

**Validado en placa (15/10, Juani):** captura, envío continuo y reproducción del tramo 2–12 s. **El amplificador tiene que ir a 5 V externos:** alimentado desde el 3V3 del DevKit no sonaba (causa eléctrica sin medir). Volumen limitado por software (`ORBITA_PLAY_GAIN_Q15`, `arquitectura.md` §2).

## STT continuo de laboratorio (servidor Python)

No requiere cambiar ni flashear la ESP: usa la subida continua v2 y el downlink PLAY existentes. No implementa TTS, LLM ni la FSM final; detectar Órbita en el servidor es una prueba, la wake word del producto sigue prevista en el dispositivo.

```powershell
python -m pip install -r tools/requirements-websocket.txt -r tools/requirements-speech.txt
python tools/ws_audio_server.py --stt-continuous --output C:\orbita_pruebas
```

Arranca **pausado**. En la consola del servidor: **G** inicia/reanuda transcripción, **P** pausa, **E** muestra estado; en Windows no hace falta Enter. En otras terminales, comando y Enter. **Ctrl+C** apaga el servidor. Para ejecución sin teclado: `--start-recording`. Salir del monitor ESP-IDF con Ctrl+] no detiene el firmware ni su envío.

- La ESP manda siempre. Incluso pausado, el servidor valida longitud, secuencia y CRC y contesta `ready`/`ack`; descarta PCM sin escribir WAV, JSON de sesión ni texto. No hay timeout por inactividad de audio: ping/pong mantiene y supervisa el socket. Las desconexiones reales siguen provocando reconexión del firmware. Una ESP a la vez por consola.
- Activo: ventanas de 4 s con 1 s de solapamiento (`--stt-chunk-seconds`, `--stt-overlap-seconds`), Whisper `tiny` en CPU/int8 y español, VAD y tiempos por palabra. Modelo descargado/cargado en la primera inferencia; puede demorarse. `--stt-model base` permite comparar precisión y costo de CPU. Es transcripción por ventanas sucesivas, no resultado instantáneo por cada muestra.
- Texto nuevo en consola y en `stt_<sesion>.jsonl` (UTF-8, tiempos, detección y ventanas omitidas). Por defecto **no guarda audio**. Agregar `--save-audio` guarda WAV mono 16 kHz/16-bit de las ventanas procesadas, incluyendo el solapamiento; sirven para comparar audio y texto. No es archivo íntegro de toda la captura: se descartan parciales al pausar/desconectar y ventanas omitidas por atraso. Los archivos pueden crecer mientras esté activo; usar P cuando no se necesiten.
- P limpia parciales/cola e invalida la inferencia en curso: Whisper puede terminar internamente, pero ese resultado no se imprime, guarda ni dispara otro pitido. Un archivo cuya escritura ya empezó o un pitido ya enviado pueden terminar. G empieza con audio nuevo; lo recibido durante la pausa no se recupera.
- La palabra disparadora se configura con `--wake-word gato` para comparar reconocimiento sin cambiar modelo/ESP; por defecto `orbita`. El JSONL registra `keyword` y `detected` (conserva `orbita` para la detección original).
- Al reconocer la palabra completa configurada (Órbita por defecto) (sin distinguir tilde/mayúsculas) manda un seno de 1 kHz y 200 ms por `play`/binarios PLAY/`play_end`. No necesita WAV de respuesta. Usa `play_done`/`play_reject`, evita superponer respuestas y suprime repeticiones de la zona solapada (guardia adicional de 1,5 s de audio). No garantiza exactitud de wake word: hay que medir falsos positivos/negativos con voz real.
- Recepción y STT separados: una inferencia y como máximo dos ventanas pendientes. Si la CPU no alcanza, descarta la ventana pendiente más vieja y avisa; nunca acumula audio indefinidamente ni detiene los ACK por esperar Whisper. También limita a 128 los detalles de huecos en memoria en este modo. Usar salida fuera de OneDrive para evitar que su sincronización demore las escrituras.

Implementación: `tools/ws_audio_live.py` (controles, ventanas, worker y PLAY), `ws_audio_session.py` (validación/ACK sin archivos) y `ws_audio_speech.py` (Whisper y timestamps). Las demos anteriores conservan su comportamiento.

## Prueba local de STT y respuesta por audio

El firmware mantiene el flujo v2 abierto y no manda un `end`. Para probar el recorrido sin cambiar la ESP, el servidor analiza **una sola ventana inicial** (8 s por ejemplo), mientras la conexión sigue recibiendo audio. Instalar la dependencia opcional y ejecutar:

```powershell
python -m pip install -r tools/requirements-speech.txt
python tools/ws_audio_server.py --stt-window-seconds 8
```

El modelo `tiny` de faster-whisper se descarga la primera vez y corre localmente en CPU. El servidor imprime la transcripción y busca la palabra completa `Órbita` sin distinguir mayúsculas ni tilde. Si la encuentra, manda `orbita` y reproduce por WebSocket `tools/respuesta_orbita.wav`, si existe. Ese WAV todavía falta: al recibirlo, convertirlo a PCM mono, 16 kHz, 16-bit y guardarlo con ese nombre. Sin el WAV, la detección funciona y queda registrada, pero no hay audio de respuesta. La ventana se analiza una vez por conexión; no es escucha continua de wake word ni STT en tiempo real.

## Downlink: respuesta por el parlante

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
- **RX PLAY:** buffer propio de 4096 B en PSRAM, fuera de la cola de 512 B. `audio_ws_playback` copia el PCM al ring del parlante, asocia el ID de 64 bits del servidor con el token de 32 bits de playback y reporta fin/corte. Contrato binario v1 propuesto: acordarlo con el backend real. Detalle: [audio_ws_playback/README.md](firmware/components/orbita_audio_ws/README.md) y [orbita_ws/README.md](firmware/components/orbita_ws/README.md).
- Diferencias con la FSM (backoff 1-8 s sin tope de intentos, sin timeout de 5 s de respuesta ni `srv_*`) siguen sin acordar con Juani. No modifican el vault.
- Sin AFE: mono L (`mic_l`) por defecto o mono R (`mic_r`) con `CONFIG_ORBITA_RAW_MIC_RIGHT`. La decision final L/R/mezcla sigue abierta.

## Verificación

```powershell
python -m unittest discover -s tools/tests -v
```

48 pruebas (1 salteada: fixture C): protocolo v1, sesión v2, helpers STT/PLAY y recorte exacto con devolución PLAY por socket real, verificando que el uplink y los ACK sigan funcionando. El parser del downlink tiene pruebas en C para PC (`tools/tests/downlink`).

Estado en hardware (13/10/2026): warm-up, retención y corte de Wi-Fi validados con v1. **v2 probado en la placa**: sesión con `hello`/`ready`, 5 segmentos de 10 s, catch-up de 48 s de audio en 15 s tras un corte largo y huecos marcados (27 s perdidos al desbordar el ring). Falta: corte corto con la sesión en curso, tomas largas (10 min / 1 h) y mirar `sin_ack` y la pila del sink en el monitor.


## Diagnóstico local del parlante

En `menuconfig` → `Orbita integration`, activar `Diagnostico: solo tono local del parlante (sin captura ni red)` y recompilar/flashear. Repite 1 kHz durante 1 s y silencio de 3 s, con la ganancia y pines existentes de `orbita_playback.h`. Muestra STARTED/DONE y errores de begin/push/end; no inicia mics, AFE, Wi-Fi ni sockets. No requiere servidor. Desactivar la opción y volver a flashear para retomar STT/replay. Opción desactivada por defecto; no modifica credenciales.


### Captura continua y tonos desde el servidor

Con el diagnóstico local **desactivado** en menuconfig, ejecutar `python tools/ws_audio_server.py --test-tones --segment-seconds 10 --output C:\orbita_pruebas`. La ESP captura/envía continuamente y el servidor guarda WAV/JSON normales y devuelve una secuencia PLAY de tres tonos de 1 s, separados por 1 s de silencio, una vez por conexión. No usa STT, G/P ni wake word. RESET en la ESP para repetir desde un arranque nuevo.


### Diagnóstico integrado con CRC y pitidos periódicos

El prototipo WebSocket vuelve a encolar un tono local de 1 kHz/1 s al inicializar playback, antes de pipeline/red (token 0, sin reportarlo al servidor). Captura y red arrancan normalmente. Mantener desactivado `Diagnostico: solo tono local...` en menuconfig.

```powershell
python tools/ws_audio_server.py --test-tones-every 5 --segment-seconds 10 --output C:\orbita_pruebas
```

Tras ready, el servidor espera 5 s y envía tres pitidos de 250 ms separados por 250 ms (1,25 s, 40000 B de PCM), repitiendo el inicio de la tanda cada 5 s mientras sigue guardando la subida. Si aún espera play_done/play_reject, omite la tanda y avisa; no superpone respuestas. Cada conexión tiene su propio temporizador, cancelado al desconectar. No usa STT ni G/P.

Comparar `PLAY TX` (Python) y `PLAY RX` (ESP): mismo id, bytes y CRC. `PLAY BEGIN` asocia id a token; `DIAG DONE token=...` imprime bytes/CRC retirados del ring **antes de ganancia**, bytes aceptados por I2S, errores y picos antes/después. Para el tono local: token=0, 32000 B, CRC `5df4a34c`. Para estos tonos el pico esperado antes/después es 16000/4000 con la ganancia actual. El diagnóstico toma un snapshot antes de liberar la respuesta para no mezclar contadores entre IDs. CRC/contadores correctos no demuestran que las señales físicas lleguen al amplificador: si sigue el silencio, medir alimentación y salida I2S.


### Solo downlink, micrófonos inactivos

Desde la carpeta actual `hi-orbita`, en menuconfig → Orbita integration, desactivar `Diagnostico: solo tono local...` y activar `Diagnostico: WebSocket y parlante SIN microfonos`. Recompilar y flashear. Este modo excluye la llamada a orbita_audio_i2s_init, pipeline/AFE y sink: no genera relojes de captura ni manda hello/PCM. Los módulos pueden seguir alimentados físicamente; el firmware no inicia su interfaz I2S. Conserva tono local de 1 s y CRC; espera su fin antes de iniciar Wi-Fi/WebSocket. Una espera de 5 s agotada detiene el arranque con error. Main mantiene el keepalive que normalmente hace el sink.

Servidor: `python tools/ws_audio_server.py --downlink-tones-only`. No espera subida ni crea grabaciones; envía tres pitidos cortos cada 5 s desde la conexión y recibe play_done/play_reject. Si una tanda sigue sin confirmación, omite la siguiente para no superponer audio. Logs esperados de ESP: `SIN MICROFONOS...`, `LOCAL BOOT`, `DIAG DONE token=0`, `Tono local terminado; iniciando red SIN captura`, conexión y PLAY/DIAG periódicos. No debe aparecer `orbita_audio: I2S inicializado` ni warm-up/sesión de audio_ws. Para retomar captura, desactivar la opción y reflashear.


### Captura cruda y pitidos locales, sin red

En Orbita integration, dejar el modo prototipo, desactivar los dos diagnósticos anteriores y activar `Diagnostico: microfonos y pitidos locales SIN red`. Flashear desde `hi-orbita/firmware`. No requiere servidor.

Usa los drivers actuales: inicia I2S de micrófonos y después playback; una tarea en core 1 lee continuamente bloques estéreo de 1024 frames y descarta los datos tras medir picos. No inicia pipeline, AFE, sink, Wi-Fi ni WebSocket. La memoria no crece: un bloque crudo reutilizado de 8192 B y buffers fijos de tono, además del ring acotado de playback. No almacena una grabación ni hace loopback de la voz.

La ESP genera tres tonos locales de 250 ms (1 kHz) con silencios de 250 ms, comenzando una tanda cada 5 s. Se mantienen CRC/bytes/picos de playback. Log `mics_parlante`: STARTED/DONE, frames leídos, errores, overflows DMA, picoL/picoR y RAM/PSRAM libres. Hablar cerca de cada micrófono permite comprobar que sus picos cambian. Si suena, luego se prueba el pipeline y finalmente la red por separado; no demuestra todavía funcionamiento con AFE ni con todo el integrado.

### Prueba STT con un microfono

Con equipo apagado, desconectar solo SD del mic con L/R a GND. Dejar SD del mic con L/R a 3V3 conectado. En Orbita pipeline: AFE desactivado y `Sin AFE: enviar microfono derecho` activado. En Orbita integration: prototipo WebSocket y los tres diagnosticos desactivados. El hello informa `source=mic_r`; captura sigue estereo, salida mono derecha. Esto no deshabilita electricamente ningun microfono ni resuelve la causa del fallo de banco.

Servidor: `python tools/ws_audio_server.py --stt-continuous --wake-word gato --save-audio --output C:\orbita_pruebas`. Esperar modelo listo y pulsar G para transcribir; P pausa sin cortar la conexion. No activar tonos periodicos en esta prueba.

## Resultado de banco y traspaso a Claude (16/10/2026)

Validado por Juani: captura continua sin AFE, mono R, STT Python con palabra `gato` y pitido de respuesta por WebSocket/parlante. Solo SD del mic con L/R a 3V3 conectado a GPIO 6; SD del mic con L/R a GND desconectado. Ambos mics conservan alimentacion y relojes. No se modificaron pines, audio_capture, configuracion I2S ni ganancia de playback. La captura sigue estereo; tarea_feed selecciona indice 1 para el ring mono. Esto es una configuracion de prueba, NO una solucion al problema con ambos SD.

Observaciones de banco: con ambos SD conectados no sonaba el parlante, incluso en diagnostico local sin Wi-Fi/WS/AFE. Al quitar ambos SD o separar la linea del GPIO 6 sonaba. Con solo el mic L/R=3V3 conectado sonaba; con solo L/R=GND no. Al intercambiar L/R, el fallo siguio al canal izquierdo segun la prueba reportada. No se establecio la causa electrica/software. CRC coincidente y bytes aceptados por I2S no verifican la senal fisica en el amplificador.

Cambios para revisar en el frente Audio: opcion ORBITA_RAW_MIC_RIGHT en orbita_pipeline (default n, solo sin AFE); indice y nombre compartidos en pipeline.h; seleccion en tarea_feed y source del hello consistente. orbita_playback conserva API, pines y ganancia: se agregaron CRC/bytes/picos/errores y snapshot antes de publicar LIBRE. audio_ws_playback agrega CRC de recepcion y tono de arranque de 1 s (token 0, sin play_done al servidor). Hay tres modos de diagnostico excluyentes en Orbita integration, todos apagados por defecto; apagarlos para STT. No se cambiaron FSM ni sdkconfig.defaults. La opcion R se activa localmente en menuconfig: sdkconfig no se versiona.

Servidor: ws_audio_live separa recepcion/ACK de inferencia; G/P/E controlan STT sin cortar la subida. Whisper tiny CPU/int8, ventanas 4 s con solapamiento 1 s, cola acotada, palabra configurable. Sin LLM/TTS; la deteccion devuelve un pitido sintetizado de 200 ms por PLAY. Texto JSONL y WAV opcionales solo durante STT activo; --save-audio guarda ventanas procesadas, no un archivo continuo completo. El WAV de respuesta opcional del modo STT antiguo no interviene en este pitido.
