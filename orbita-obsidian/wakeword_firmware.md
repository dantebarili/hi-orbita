# Wake word en el chip

Modelo microWakeWord `orbita.tflite`, siempre activo en el modo WebSocket (desde el 29/10, rama `prototipo-alfa-wakeword`). Cada detección abre una conversación (ver `protocolo_comunicacion.md`, "Política de captura"). Se validó primero con `okay_nabu` en el banco `wakeword-banco` (ya borrado del firmware: recuperar de git, commit `ad1c8e3`). El entrenamiento está en `orbita-dataset/docs/entrenamiento.md`.

## Concepto
El modelo no escucha audio: mira un **espectrograma**. Si el chip lo arma distinto que el notebook, aunque sea un poco, el modelo falla **sin dar error**. Por eso el chip usa el **mismo código C** del entrenamiento (`esp-micro-speech-features`, fork del microfrontend de TFLM que usa `pymicro-features`) y se verifica número por número.

```
promedio (L+R)/2, crudo (sin AFE: el modelo no lo vio) ─► int16, 16 kHz
  │  ventana de 30 ms (480 muestras) que avanza de a 10 ms
  ▼
cada ventana: Hann → FFT 512 → 40 bandas mel (125-7500 Hz) → energía → reducción de ruido → PCAN → log
  │  = 1 columna de 40 números (las features), una cada 10 ms
  ▼
int8 con la escala del modelo ─► de a 3 columnas (30 ms nuevos) ─► modelo ─► probabilidad 0-255
  ▼
promedio de las últimas 5 > cutoff (0,4) ─► DETECCIÓN
```

**Dos ventanas distintas:**
- **Lo que mira cada probabilidad: alrededor de 1,5 s.**
  - Cada 30 ms sale una probabilidad que responde "¿se dijo la palabra en el último ~1,5 s, terminando ahora?".
  - El modelo recibe solo 30 ms por llamada, pero guarda adentro las columnas anteriores (variables de estado, como la memoria de un FIR).
  - Son 1,5 s porque se entrenó con clips de 1,5 s (`clip_duration_ms`) y la palabra al final. El campo receptivo de la red da unas 155 columnas: cuenta a mano desde los kernels de la celda 11, sin verificar.
- **La ventana de 5 probabilidades: 150 ms.**
  - No mira audio: confirma que el modelo siga seguro 5 salidas seguidas, para que un pico aislado no dispare.
  - Agrandarla da menos falsos positivos y más latencia. Se calibra con la curva de `wakeword_ref.py`.

## El código, función por función
Componente `firmware/components/orbita_wakeword/`. El C++ (TFLite Micro) está aislado en `wakeword_modelo.cc`; el resto es C y lo llama con `extern "C"`.

**TFLite Micro (TFLM)** es el intérprete que ejecuta el `.tflite`. El `.tflite` es un **archivo de datos**: el grafo de operaciones y los pesos int8, unos 60 KB. TFLM no hace `malloc` mientras corre (usa una **arena**, un bloque fijo de RAM reservado al arrancar) y solo compila las operaciones que se le registran.

### `main.c` (modo WebSocket)
`orbita_wakeword_start()` va antes que el I2S, el playback y el pipeline, para que la arena tome RAM interna antes que el AFE. El pipeline arranca con `on_crudo = orbita_wakeword_on_crudo`.

### `wakeword_modelo.cc`: el modelo (C++)
| Función | Qué hace |
|---|---|
| `reservar(bytes, &interna)` | Pide memoria alineada a 16 bytes (lo exige TFLM): primero en RAM interna; si no hay, en PSRAM. Avisa dónde quedó. |
| `registrar_ops()` | Carga en el *op resolver* las 20 operaciones que usan los modelos microWakeWord (lista de ESPHome; `okay_nabu` usa 13). Si al modelo le falta una, falla `AllocateTensors` al arrancar, no en medio de la ejecución. |
| `ww_modelo_init(tflite, len, arena, &info)` | Se llama una vez. Ver los pasos abajo. |
| `ww_modelo_invoke(features, &prob)` | Copia 3 × 40 int8 a la entrada (memoria dentro de la arena), corre `Invoke()` (ejecuta todo el grafo) y devuelve la probabilidad uint8. |

Pasos de `ww_modelo_init`:
1. Copia el `.tflite` desde flash (viene embebido con `EMBED_FILES`, sin alineación garantizada) a RAM alineada, interna si entra, porque los pesos se leen en cada inferencia.
2. `GetModel` no copia nada: "mira" los bytes como estructura (*flatbuffer*). Se chequea la versión del formato.
3. `registrar_ops()`.
4. Crea las **variables de recurso**: la memoria interna del modelo streaming, en una arena aparte de 1 KB. Por eso el audio tiene que llegar continuo.
5. Construye el intérprete con *placement new* (`new (buffer) Tipo(...)`), que arma un objeto C++ dentro de un buffer `static` ya reservado, sin heap.
6. `AllocateTensors` reparte la arena entre los resultados intermedios de las capas, reusando el lugar de los que ya no se necesitan. Ahí queda fijo cuánta arena usa.
7. Lee del modelo lo que espera: entrada `[1, 3, 40]` int8 (stride × bandas) con su escala y cero, y salida `[1, 1]` uint8. Si no coincide, no es un modelo microWakeWord v2 y se corta.

### `wakeword.c`: audio → detección (C)
En el orden en que actúan:

| Función | Dónde corre | Qué hace |
|---|---|---|
| `orbita_wakeword_start()` | `app_main`, una vez | Llama a `ww_modelo_init`. Calcula el factor de cuantización y el cutoff en 0-255. Configura el frontend con los mismos parámetros que el entrenamiento (`FrontendPopulateState`). Crea el ring en PSRAM y la tarea `wakeword`. |
| `orbita_wakeword_on_crudo(chunk, frames, hueco)` | Tarea de captura (prio 6), cada 64 ms | Promedia los dos mics del chunk estéreo (`orbita_mono_lr`), lo copia al ring con timeout 0 y marca si hubo hueco. Si el ring está lleno, descarta, cuenta `overruns` y marca hueco en el siguiente. Nada más: no puede frenar la captura. |
| `tarea_wakeword()` | Tarea propia (core 1, prio 4) | Espera chunks del ring. Por cada uno: si viene con hueco, `reiniciar_por_hueco()`; después `procesar_audio()`. Cada 10 s, `log_resumen()`. |
| `procesar_audio(muestras, n)` | Tarea `wakeword` | El núcleo. En bucle: `FrontendProcessSamples` consume muestras y, cada 160 nuevas (10 ms), devuelve una columna de 40 uint16. `cuantizar` la pasa a int8 y la escribe en la fila `s_paso` de la entrada. Con 3 filas: `ww_modelo_invoke` y `decidir`. Si 1024 no se divide justo en columnas, no importa: el frontend y `s_paso` arrastran el resto al chunk siguiente. |
| `cuantizar(v, out)` | Dentro de `procesar_audio` | `int8 = round(v × 0,0390625 / escala) + cero`, recortado a −128…127. El 0,0390625 es el que usa el notebook (`feature = uint16 / 25,6`); escala y cero se leen del modelo. ESPHome usa constantes fijas que suponen escala 26/256, pero el modelo trae 26/255 (difiere en el 6 % de los valores). Va en float32, igual que `wakeword_ref.py`. |
| `decidir(p)` | Dentro de `procesar_audio` | Guarda `p` en un buffer circular de 5 y detecta si `suma > cutoff × 5` (el promedio, sin dividir). **Espera `s_ignorar`:** arranca en −100 y sube con cada inferencia por debajo del cutoff; recién en 0 puede detectar (~3 s). Al detectar, loguea y vuelve a −100. Es la lógica de ESPHome. |
| `reiniciar_deteccion()` | `decidir`, arranque, huecos | Vacía las 5 probabilidades y pone la espera en −100. |
| `reiniciar_por_hueco()` | `tarea_wakeword` | Si se perdió audio: `FrontendReset` (la reducción de ruido y el PCAN tienen memoria), `s_paso = 0` y `reiniciar_deteccion()`. La memoria del modelo no se puede resetear sin recargarlo; la espera de 3 s la "lava". |
| `log_resumen()` | `tarea_wakeword`, cada 10 s | Imprime detecciones, probabilidad máxima, tiempos máximos de frontend e `Invoke`, huecos, overruns, contadores de la captura, stack y RAM interna libre. |

## Medido en PC (`tools/wakeword_ref.py`, 25/10)
- `okay_nabu`: entrada [1,3,40] int8, escala 26/255, cero −128; salida uint8 con escala 1/256; 13 operaciones.
- Se detecta igual atenuado hasta −72 dBFS de RMS: el PCAN normaliza el nivel, así que no hace falta ganancia; lo que limita es la SNR.
- 0 disparos en 10 min de ruido de laboratorio.

## Medido en placa (`okay_nabu`, prueba bit a bit, 25/10)
- **Bit a bit: PASA.**
  - 697 columnas × 40 features y 232 probabilidades **idénticas** (diferencia 0).
  - 1 columna no se pudo leer: un log del arranque cortó la línea.
  - Detecta en la misma inferencia que la PC (211).
- **Tiempos máximos:** `frontend` 1,2 ms por columna e `Invoke` 2,1 ms. Por 30 ms de audio: 3 × 1,2 + 2,1 ≈ **5,7 ms, como mucho el 19 % del core 1**.
- **Memoria:**
  - Arena 25 804 B de 32 768; modelo y arena en RAM interna.
  - RAM interna libre 228 KB.
  - Stack de la tarea: usa ~2,6 KB de 6 KB.

## Verificación
1. **PC:** `wakeword_ref.py run modelo.tflite audio.wav --json modelo.json --dump ref.txt` da las features y probabilidades de referencia.
2. **Bit a bit** (chip vs PC, ya pasó el 25/10): se sacó del firmware el 29/10; para repetirla, recuperar `CONFIG_ORBITA_WW_PRUEBA_WAV` de `ad1c8e3` y usar `wakeword_ref.py comparar`.
3. **En vivo:** aciertos a 0,5, 1, 2 y 3 m; falsos por hora con charla o TV de fondo; contadores de la captura en 0.

## Abierto para la integración
- Corre sobre el crudo (promedio L+R) aunque el AFE esté activo; falta medir aciertos con el promedio (se validó con un solo mic).
- La carga con el WiFi y el AFE encendidos (RAM interna e `invoke_max` en el resumen).
- AEC para detectar mientras suena el parlante.
- Los eventos `dev_wake_word` de la FSM.
