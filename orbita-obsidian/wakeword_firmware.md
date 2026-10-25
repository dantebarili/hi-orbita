# Wake word en el chip (banco de prueba)

Branch `wakeword-banco`. Corre un modelo de microWakeWord sobre la captura actual: primero `okay_nabu` (oficial, se sabe que anda), después `orbita.tflite`. El entrenamiento está en `orbita-dataset/docs/entrenamiento.md`.

## Concepto
El modelo no escucha audio: mira un **espectrograma**. Si el chip lo arma distinto que el notebook, aunque sea un poco, el modelo falla **sin dar error**. Por eso el chip usa el **mismo código C** del entrenamiento (`esp-micro-speech-features`, fork del microfrontend de TFLM que usa `pymicro-features`) y se verifica número por número.

```
mic L, crudo (sin AFE: el modelo no lo vio) ─► int16, 16 kHz
  │  ventana de 30 ms (480 muestras) que avanza de a 10 ms
  ▼
cada ventana: Hann → FFT 512 → 40 bandas mel (125-7500 Hz) → energía → reducción de ruido → PCAN → log
  │  = 1 columna de 40 números (las features), una cada 10 ms
  ▼
int8 con la escala del modelo ─► de a 3 columnas (30 ms nuevos) ─► modelo ─► probabilidad 0-255
  │  el modelo es streaming: recuerda lo anterior (en el entrenamiento vio clips enteros de unos 1,5 s)
  ▼
promedio de las últimas 5 > cutoff (0,97 okay_nabu / 0,85 orbita) ─► DETECCIÓN
```

## El código, bloque por bloque
Componente `firmware/components/orbita_wakeword/`. El C++ (TFLite Micro) está aislado en `wakeword_modelo.cc`, y el resto es C. `extern "C"` en `wakeword_modelo.h` hace que C lo pueda llamar.

**TFLite Micro (TFLM)** es el intérprete que ejecuta el `.tflite`. El `.tflite` es un **archivo de datos**: el grafo de operaciones y los pesos int8, unos 60 KB. Para microcontroladores, TFLM no hace `malloc` mientras corre (usa una **arena** fija) y solo compila las operaciones que se le registran.

**1. Preparar el modelo (`ww_modelo_init`, una vez)**
1. **Copia** del `.tflite` (embebido en flash con `EMBED_FILES`) a RAM alineada a 16 bytes, interna si entra: los pesos se leen en cada inferencia.
2. **`GetModel`**: no copia nada, "mira" los bytes del archivo como estructura (*flatbuffer*) y chequea la versión del formato.
3. **Op resolver**: la caja de herramientas. Tiene las 20 operaciones de ESPHome (`okay_nabu` usa 13). Si falta una, falla al preparar el modelo, no al correrlo.
4. **Variables de recurso**: la memoria interna del modelo streaming (operaciones `VAR_HANDLE`/`READ_VARIABLE`/`ASSIGN_VARIABLE`). Por eso el audio tiene que llegar **continuo**.
5. **Intérprete + `AllocateTensors`**:
   - El intérprete se construye con *placement new* (`new (buffer) Tipo(...)`) dentro de un buffer `static`, sin heap.
   - `AllocateTensors` reparte la arena entre los resultados intermedios y fija cuánta usa (`arena_used_bytes`, se loguea).
6. **Se lee del modelo**, no se escribe a mano:
   - Entrada `[1, 3, 40]` int8: 3 columnas (*stride*) × 40 bandas, con su escala y cero.
   - Salida `[1, 1]` uint8.

**2. Una inferencia (`ww_modelo_invoke`)**: copia 3×40 int8 a la entrada (memoria dentro de la arena) → `Invoke()` ejecuta el grafo → lee la probabilidad. Tiene que tardar mucho menos que 30 ms.

**3. Del audio a la probabilidad (`procesar_audio`, `wakeword.c`)**: recibe 1024 muestras (64 ms) del ring.
- `FrontendProcessSamples` consume de a 160 muestras y entrega una columna cada vez que completa una ventana.
- `cuantizar` pasa la columna a int8 y la escribe en la fila `s_paso` de la entrada.
- Con 3 filas llenas: `Invoke` → `decidir`.
- Las cuentas no cierran justo (1024 no es múltiplo de 480) y no importa: `s_paso` y el frontend arrastran el resto al chunk siguiente.

**4. `cuantizar`**: `int8 = round(v × 0,0390625 / escala) + cero`.
- El notebook usa `feature = uint16 × 0,0390625`, y el modelo cuantiza con su escala y cero.
- ESPHome usa constantes fijas que suponen escala 26/256, pero el modelo trae 26/255 (difiere en el 6 % de los valores). Por eso se lee del modelo.
- Va en float32, igual que `wakeword_ref.py`, para que la prueba bit a bit dé exacta.

**5. `decidir`** (misma lógica que ESPHome):
- Guarda las últimas 5 probabilidades en un buffer circular y detecta si `suma > cutoff_u8 × 5` (el promedio sin dividir).
- **Espera (`s_ignorar`)**: arranca en −100 y sube con cada inferencia por debajo del cutoff. Recién en 0 detecta.
- Son unos 3 s al arrancar y después de cada detección, para que el modelo "caliente" su memoria y no dispare dos veces con la misma palabra.

**6. Huecos**: si la captura perdió audio (DMA o ring lleno), el chunk llega marcado con `hueco`.
- Se resetea el frontend (su reducción de ruido y el PCAN tienen memoria), la ventana y la espera.
- La memoria del modelo no se puede resetear sin recargarlo; la espera de 3 s la "lava".

**Tarea y ring**:
- `on_crudo` (en la captura, prioridad 6) solo copia el mic elegido a un ring de 16 chunks (1 s) en PSRAM.
- La tarea `wakeword` (core 1, prioridad 4, por debajo de captura y feed) hace todo lo demás.
- Cada 10 s loguea detecciones, probabilidad máxima, tiempos de frontend e `Invoke`, huecos y los contadores de la captura.

## Medido en PC (`tools/wakeword_ref.py`, 25/10)
- `okay_nabu`: entrada [1,3,40] int8, escala 26/255, cero −128; salida uint8 con escala 1/256; 13 operaciones.
- "Okay Nabu" con voz sintética de Windows: probabilidad 1,0, detectado.
- Se sigue detectando atenuado hasta −72 dBFS de RMS: el PCAN normaliza el nivel, así que no hace falta ganancia; limita la SNR.
- 0 disparos en 10 min de ruido de laboratorio.
- **A medir en placa:** arena usada (26 a 50 KB), tiempo de `Invoke`, stack y RAM interna libre.

## Verificación
1. **PC**: `wakeword_ref.py run modelo.tflite audio.wav --dump f.txt` da las features y las probabilidades de referencia.
2. **Bit a bit**: con `CONFIG_ORBITA_WW_PRUEBA_WAV` el chip procesa `prueba.wav` y vuelca lo mismo. Se compara con `wakeword_ref.py comparar log.txt f.txt`. Pasa con ±1 LSB.
3. **En vivo**: aciertos a 0,5, 1, 2 y 3 m; falsos por hora con charla o TV de fondo; contadores de la captura en 0.

## Abierto para la integración
- Si corre sobre el audio crudo o sobre la salida del AFE (se decide con datos).
- La carga con el WiFi encendido.
- AEC para detectar mientras suena el parlante.
- Los eventos `dev_wake_word` de la FSM.
