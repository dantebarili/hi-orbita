# Diseño del ring (ring in, ring out, header y tabla de segmentos)

Movido de `arquitectura.md` (antes §3.4/§3.5). Contexto general y vocabulario: [[arquitectura]].

**Políticas (A5, 2026-10-10)**

| Tema | Decisión |
|---|---|
| Qué se guarda | Siempre, también en `server_back` y `audio_play`: sin huecos en la historia clínica. Se puede achicar a ~40 s dejando de grabar en `audio_play` (cuesta huecos). |
| Reintentos | El segmento se retiene hasta `srv_respuesta_recibida` (~11 s con 2 intentos de 5 s + backoff de 1 s). Si se agotan, la FSM lo descarta y queda anotado el hueco. |
| Pre-roll | ~1 s (≈31 slots) antes de la wake word: se detecta cuando "Órbita" ya terminó, y sin pre-roll el pedido llegaría cortado. El backend también recibe el "Órbita" y puede verificarlo. Se ajusta al medir. |
| Ring lleno | Se descarta el segmento pendiente más viejo; el hueco queda en la tabla. |

**Los dos rings**

| | Ring in | Ring out |
|---|---|---|
| Une | captura → tarea `feed` | `feed` (sin AFE) o `fetch` (con AFE) → sink. Una tarea de procesamiento (nunca la captura) llama siempre al mismo callback `on_audio` (propuesta A5) |
| Contiene | chunks de entrada (estéreo, 16 bit, 4 KB), sin header | slots mono con header de 16 B |
| Tamaño | 8 chunks = 32 KB (~0,5 s) | ~60 s ≈ 1,9 MB en PSRAM (segmento de 2 s + reconexión de WiFi ≈ 35 s; falta sumar el largo máximo de `audio_play`) |
| Para qué | Desacopla la captura de todo lo que sigue, con o sin AFE: si `feed` tarda o bloquea, el I2S igual se lee. Queda siempre. | Retención, reintentos, pre-roll |
| Estructura | Ring de FreeRTOS (`NOSPLIT`, un chunk por ítem; se protege solo) | Ring de slots propio + tabla de segmentos (el de FreeRTOS no permite releer, ni pre-roll, ni mandar primero el urgente) |

**Header del slot (solo ring out, 16 bytes; definido en `orbita_audio/include/audio_ring_out.h`).** Guarda lo que se sabe al escribir el slot. Es **interno**: no viaja al backend; el sink arma con él el header del cable de cada mensaje.

| Campo | Tipo | Qué hace |
|---|---|---|
| `seq` | u32 | Nº de slot, sube de a 1. Comparar con `(int32_t)(a - b)` (vale al dar la vuelta, ~4,4 años a 32 ms). Se reinicia en cada arranque: el backend necesita además un id de sesión. |
| `lost_samples` | u32 | Muestras perdidas justo antes de este slot (0 = ninguna). Reconstruye el tiempo y alinea el WAV de test; no se rellena con ceros. |
| `n_samples` | u16 | Muestras de audio válidas en el slot, mono (el `vad_cache` puede traer otro tamaño). |
| `flags` | u8 | bit 0 `VAD_OK` (el VAD midió este bloque; sin AFE no hay VAD), bit 1 `VOZ` (detectó voz; solo vale con `VAD_OK`). Ambos en 0 = sin VAD: **compuerta abierta**, se manda igual. |
| `version` | u8 | Versión del formato. |
| (reservado) | u32 | Futuro. |

- `lost_samples` suma lectura fallida, overflow del DMA (A1), ring in lleno, chunk inválido y, con AFE, `fetch` inválido (la deriva del AFE no se mide): contadores que la tarea que escribe el slot (`feed` o `fetch`) lee, pone en 0 y copia (aproximado con AFE por su latencia).
- No lleva inicio/fin de segmento ni "urgente": se deciden después de guardar el slot y los pre-roll se solapan; eso vive en la tabla.
- Sin relleno: `_Static_assert(sizeof == 16)`, sin `__attribute__((packed))`. Costo en PSRAM despreciable aunque el slot baje a 10 ms (6000 slots, 2 MB).
- Hecho (13/10): formato del slot (`audio_ring_out.h`, 1040 B con `_Static_assert`), ring out v1 (`audio_ring_out.c`), productor en `pipeline.c` y tarea sink. Compila con y sin AFE; sin probar en el chip. Falta (A6): tabla de segmentos y header del cable.

**Ring out v1 (A5, 2026-10-13)**

| Tema | Decisión |
|---|---|
| Estructura | Cola FIFO circular de `ORBITA_RING_SLOTS` (1024 = 32,8 s, 1,04 MB) slots en PSRAM (`heap_caps_calloc`). Dos índices (`write_idx`, `read_idx`) y `ocupados`; el índice avanza con `(i+1) % N`. Sin confirmación ni pre-roll: leer **saca** el slot. El `max_ocupados` de las stats dice cuánto hace falta. |
| Protección | Spinlock (`portMUX`, interrupciones apagadas ~20–50 µs) sobre índices, contadores y el `memcpy`. Sin mutex (revisión 13/10): con prio 5 en core 0, WiFi/TLS podían desalojar al sink con el mutex tomado y el productor esperar o perder el slot. `write` siempre entra, sin espera ni timeout. Nunca en la ISR ni durante el `send` de red. A medir: duración real con PSRAM y flash activos. |
| Escritura | `orbita_ring_out_write(audio, n, flags, lost)`: copia el audio (el buffer del productor se reutiliza al volver), completa `seq` y `version`. Ring lleno: descarta el slot **más viejo** y suma sus muestras (`n_samples + lost_samples`) a `lost_samples` del nuevo más viejo. |
| Lectura | `orbita_ring_out_read(dst)`: copia header + `n_samples` muestras (no el slot entero: la cola es audio viejo de otra vuelta, y no debe llegar al backend) al `dst` del sink y lo libera. El sink manda `2*n_samples` bytes, nunca 1024. Vacío → `ESP_ERR_NOT_FOUND` (el sink espera y reintenta: v1 sin notificación). Que el sink tenga su copia resuelve quién es dueño del slot. |
| Capacidad | 1024 slots = **32,8 s**: cubre los ~35 s documentados (segmento de 2 s + reconexión de WiFi) casi justo. El diseño pide ~60 s (1875 slots, 1,9 MB): subir `ORBITA_RING_SLOTS` si `max_ocupados` o la red lo piden. Si el sink o la red se cuelgan más, se pisa audio (queda anotado en `lost_samples`/`descartados`). |
| Arranque | `orbita_ring_out_init` va **antes de crear las tareas** productora y sink (no es seguro contra uso en paralelo) y **antes de iniciar el AFE** (reserva un bloque contiguo de 325 KB, ~1,9 MB a 60 s: con la PSRAM fragmentada por `esp-sr` puede fallar). |
| Para el sink | Detecta pérdidas por `hdr.lost_samples` y saltos de `hdr.seq`. `orbita_ring_out_get_stats`: ocupados, máximo, escritos, descartados (A12). |
| Pendiente (A6) | Cursor de confirmación (3er puntero) para retener hasta `srv_respuesta_recibida`; tabla de segmentos; pre-roll. Esto cambia `read` a "mirar + liberar". No direccionar por `seq % N` (2³² no es múltiplo de 313: al dar la vuelta salta de slot): usar el índice físico o N potencia de 2. Con el ring lleno hay que decidir si se descarta el nuevo o el más viejo ya enviado sin confirmar. |

**Tabla de segmentos (ring out).** Una entrada por segmento: `seq_inicio`, `seq_fin`, tipo (fondo / urgente), estado (pendiente / enviando / enviado / confirmado) y hueco previo (slots descartados). Es el índice de la cinta: qué mandar, en qué orden y en qué estado. Reemplaza al ring de FreeRTOS, a las banderas de la toma de test (`grabando`, `capture_done`) y a los flags de segmento en el slot. La wake word crea una entrada **urgente** con `seq_inicio = seq_detección − pre-roll`; dos segmentos pueden compartir slots.

Quién la toca (con mutex; son pocas filas):
- La tarea que escribe (`feed` o `fetch`), por slot (~31/s): antes de pisar un slot, mira si su segmento se necesita.
- Wake word: agrega la fila urgente. Corte por tiempo o VAD (`dev_fin_segmento`): cierra una fila y abre otra.
- Sink, en cada ciclo: elige el pendiente más prioritario (urgente primero) y lo marca "enviando" / "enviado". Lee por posición (`seq % cantidad_de_slots`); leer no libera.
- Confirmación del backend: "confirmado" y libera. Timeout o error: vuelve a "pendiente".

Alternativas (el patrón es habitual: TCP guarda lo enviado sin confirmar): (2) ring FIFO de tres punteros (escritura / envío / confirmación) + buffer aparte para el urgente con el pre-roll copiado; (3) un buffer por segmento. Cualquiera con confirmación por segmento necesita igual una lista de segmentos en vuelo. **Plan incremental:** v1 = ring de tres punteros para el fondo y lista de segmentos consecutivos, sin urgente ni pre-roll; v2 = tabla con prioridad.

**Reglas de implementación**
- El mutex protege solo la tabla y los índices, nunca el `send` de red (bloquearía al productor y el DMA perdería audio).
- Un segmento "enviando" no se descarta; el espacio se libera por rangos (el pre-roll solapado puede necesitar un slot de otro segmento).
- El callback del DMA solo incrementa un contador (sin mutex, `malloc` ni logs).

