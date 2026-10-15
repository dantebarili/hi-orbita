# Recepcion PLAY del alfa

`orbita_ws_start` acepta `on_downlink` y `downlink_ctx`. Sin callback, un `play`
se rechaza; no se simula un parlante ni se informa reproduccion exitosa.
No cambia el comportamiento de envio del microfono. Solo una conexion/instancia.

## Recorrido e integracion

`on_ws_event -> ws_rx_feed -> ws_downlink_control / ws_downlink_binary -> on_downlink`.
Un unico reconstructor conserva el orden entre controles y audio, incluso con
fragmentacion WebSocket y varios eventos por frame. Texto: 512 B. Binarios: un
buffer de 4096 B en PSRAM, reservado al iniciar; no pasan por la cola RX.
`ready`, `ack` y texto generico siguen llegando a `orbita_ws_receive`.

Eventos de `orbita_ws_downlink.h` para conectar con Dante:

| Evento | Adaptador a playback |
|---|---|
| `PLAY_BEGIN` | `play_begin`; false si no puede aceptar la respuesta |
| `PLAY_DATA` | `play_push`; copiar PCM antes de retornar; false si no cabe |
| `PLAY_END` | `play_end`; termino la entrada, puede quedar audio por sonar |
| `PLAY_STOP` | `play_stop`; vaciar audio. Si `rejected`, ya se encola rechazo |
| `DOWNLINK_RESET` | cancelar/vaciar todo por cambio de conexion, sin respuesta |
| `CONVERSATION_BEGIN/END` | avisar al responsable del LED |

El callback corre dentro del despacho de eventos de Espressif: debe ser corto,
no bloquear, ni llamar a I2S ni a `orbita_ws_send`. Puede ejecutarse en la tarea
emisora cuando una escritura falla. Proteger el ring compartido con playback.
No guardar `event` ni `pcm`: ambos se reutilizan. La copia al ring pertenece al
consumidor. Un BEGIN rechazado debe dejar playback intacto; un STOP/RESET debe
cancelar tambien audio pendiente de reproducir. Los ids no se reutilizan.

Cuando playback termina realmente, llamar `orbita_ws_play_done(connection_id,
play_id, reason, played_ms)`. `play_reject` esta disponible para fallos asincronos;
si BEGIN/DATA/END devuelve false, el transporte ya lo encola. No duplicar reportes
para STOP con `rejected=true` ni RESET. Las funciones encolan sin esperar: `ESP_OK`
significa encolado, `ESP_ERR_NO_MEM` cola llena, `ESP_ERR_INVALID_STATE` conexion
obsoleta. La tarea `ws_play_reports` envia fuera del callback y registra fallos;
no reintenta un envio ambiguo. El servidor debe ignorar play_id obsoletos incluso
si una desconexion coincide con el comienzo de una escritura.

Los BEGIN sucesivos solo se admiten despues de END; playback decide si todavia
esta ocupado. STOP viejo no afecta otro id. Secuencia incorrecta, exceso de bytes
o fallo de rearmado abortan la respuesta; no se reproduce un stream con huecos.
La desconexion cancela el audio incluso si ya se habia recibido END.

## Contrato propuesto PLAY v1 (coordinar con servidor)

Las notas originales daban los campos, pero no su codificacion. Esta es la
codificacion implementada y probada; todavia no es un contrato validado con el
servidor final. JSON <=512 B; ids son cadenas de 16 digitos hexadecimales.

`play`: `{ "type":"play", "play_id":"0123456789abcdef", "rate":16000,
"format":"s16le" }`. `channels` opcional debe ser 1. `bytes` opcional debe ser
par y <=480000; si existe, END exige haber recibido exactamente esa cantidad.
Sin `bytes`, el stream sigue limitado a 480000 B (15 s). Controles desconocidos
se entregan por la cola; JSON malformado, claves repetidas y profundidad >4 se
rechazan. cJSON reserva memoria solo para controles acotados, nunca por PCM.

Cada mensaje WebSocket binario tiene header + PCM, **4096 B en total como maximo**:

| Offset | Bytes | Campo little-endian |
|---|---:|---|
| 0 | 4 | ASCII `PLAY` |
| 4 | 1 | version = 1 |
| 5 | 1 | header_bytes = 24 |
| 6 | 2 | flags = 0 |
| 8 | 8 | play_id unsigned |
| 16 | 4 | seq, comienza en 0, crece uno por mensaje |
| 20 | 4 | largo del PCM en bytes, par y >0 |
| 24 | hasta 4072 | PCM s16le mono 16000 Hz |

`play_end` y `play_stop` llevan `play_id`. `orbita` lleva `id` y `at_index`
(uint32); `conversation_end` lleva `id`. `play_done` lleva `play_id`, `reason`
(`end`, `stopped`, `underrun`) y `played_ms`; `play_reject` lleva `play_id`.

## Verificacion y memoria

Pruebas host (tras resolver dependencias con ESP-IDF; compilador C en PATH):

```
cmake -S tools/tests/downlink -B <carpeta-temporal>
cmake --build <carpeta-temporal> --config Debug
ctest --test-dir <carpeta-temporal> -C Debug --output-on-failure
```

Se prueban 4095 cortes de frame, frames de continuacion, ping intercalado,
limites, secuencias, ids, rechazo de consumidor y reset. Para placa: falta medir
downlink simultaneo con uplink, latencia del callback, pila y RAM minima.
Costo fijo adicional: 4096 B PSRAM, pila de reportes 6144 B interna, cola de 8
reportes pequenos y estado. La prioridad de red sigue 4; reportes usa 3/core 0.
PSRAM es requisito: si no se puede reservar, start devuelve NO_MEM.
El prebuffer de 400 ms, ring de playback y driver I2S pertenecen a `orbita_playback`.

Los reportes publicos tambien encolan el cierre local por conexion/ID, aplicado antes del siguiente evento RX. El consumidor debe haber detenido su audio al reportar. Si la cola TX esta llena, el cierre local puede quedar encolado aun devolviendo NO_MEM; es seguro reintentar con el mismo ID.
