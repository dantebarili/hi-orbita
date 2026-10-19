# Wake word: Edge Impulse vs. modelo propio, e intro a redes neuronales

## Resumen
No hace falta entrenar en Edge Impulse (EI), pero tampoco tenés por qué meter EI si entrenás afuera. Para Órbita, la recomendación es entrenar en un notebook y correr el `.tflite` con TFLite Micro directo en ESP-IDF. Algunas cosas se confirmaron y otras no; están marcadas.

## ¿EI acepta un `.tflite` entrenado afuera?
**Sí, existe "Bring Your Own Model" (BYOM).** EI acepta TensorFlow SavedModel, ONNX y TFLite, y los deploya como librería C++ ([docs BYOM](https://docs.edgeimpulse.com/docs/~/revisions/0Qa5Kv0DQ8CPlSQTxNxL/edge-impulse-studio/bring-your-own-model-byom)). Lo que **no** se pudo verificar:
- Si un `.tflite` ya cuantizado a int8 se respeta tal cual o se re-cuantiza. La doc solo menciona la cuantización automática para ONNX.
- Cómo se integra el bloque DSP (MFCC/MFE) de EI con un modelo propio. Probablemente haya que replicar en el firmware exactamente el preprocesado con el que se entrenó.
- Un ejemplo de BYOM para keyword spotting en ESP32-S3. No se encontró.

Hay además un [hilo del foro](https://forum.edgeimpulse.com/t/critical-esp32-s3-failure-my-own-efficientnet-lite-int8-tflite-is-stuck-with-constant-output/17285) de alguien con un int8 propio en ESP32-S3 que daba salida constante, sin resolución. Es un aviso de que los parámetros de cuantización y el preprocesado son el punto frágil. Además, el S3 no es una placa oficialmente soportada: se usa la librería C++ exportada.

## RAM / CPU
No se encontraron números de KWS en ESP32-S3. Las referencias son de otro hardware:
- Benchmark de EI ([paper](https://arxiv.org/pdf/2212.03332)): MFCC + conv1d chica = ~30 ms DSP, ~35 ms inferencia, ~26 kB RAM DSP, ~20 kB RAM del modelo, 221 kB flash. El paper aclara que el DSP puede igualar o superar el tiempo de la red, y que la RAM no incluye el buffer de audio de entrada.
- En un ESP32 (no S3), con ESP-NN la clasificación bajó de 20 a 5 ms, pero el DSP tardó 140 ms ([blog EI](https://edgeimpulse.com/blog/announcing-official-support-for-the-espressif-esp-eye-esp32)). No se sabe cuánto se traslada al S3.
- Con el N16R8 (8 MB PSRAM) el modelo en sí no es el problema. El presupuesto lo van a apretar el AFE (beamforming, NSNet, VADNet) más el modelo corriendo a la vez. Hay que medirlo; encaja con el presupuesto RAM/CPU pendiente.

## La alternativa: microWakeWord
Ya existe un pipeline casi idéntico a lo que se quiere, y es abierto:
- [microWakeWord](https://github.com/OHF-Voice/micro-wake-word) (Apache-2.0) usa un modelo MixConv chico, cuantizado y en streaming para TFLite Micro.
- Entrena con **voces sintéticas** (Piper TTS) más aumentación con ruido y reverb, y tiene un notebook de entrenamiento (`basic_training_notebook.ipynb`).
- Corre en ESP32-S3 en Home Assistant ([blog](https://home-assistant.io/blog/2024/02/21/voice-chapter-6)).
- Features: 40 bandas cada 10 ms con ventana de 30 ms, usando el preprocesador de `micro_speech`; inferencia cada 30 ms. No hay cifras de RAM/CPU en el S3.
- El autor avisa que el notebook es para usuarios avanzados y puede no dar un modelo usable de entrada. Home Assistant también dice que cada palabra puede necesitar varias iteraciones.

## Comparación para Órbita

| | EI (entrenar en plataforma) | EI BYOM | Notebook propio + TFLite Micro |
|---|---|---|---|
| Control del entrenamiento y datos | Medio | Total | Total |
| Dependencia de terceros y planes | Alta | Media | Ninguna |
| Preprocesado | Lo da EI | Hay que replicarlo | Se define propio |
| Esfuerzo de integración | Bajo | Medio | Medio-alto |
| Componente C++ | Sí | Sí | Solo TFLite Micro |

## Riesgos a decidir ahora
1. **Licencia de ElevenLabs.** No se verificaron sus términos sobre usar el audio generado para entrenar modelos. Leerlos antes de comprometer el dataset. Piper es la alternativa abierta y gratuita.
2. **Voz sintética contra voz real.** Un modelo entrenado solo con TTS suele fallar con voces, acentos y micrófonos reales (INMP441 a distancia, con beamforming). Hace falta un set de validación con grabaciones reales (las del Frente 1 a distintas distancias) y mezclarlas en el entrenamiento. Alimenta el criterio de "terminado" del Frente Audio (tasa de falsos aciertos y rechazos).

## Decisiones sugeridas
1. La entrada del modelo es la salida ya filtrada del AFE, como dice el vault. Entrenar con audio crudo sin pasar por AFE sería un error de dominio.
2. El preprocesado (features) es parte del contrato del modelo: se define una sola vez y se comparte entre notebook y firmware.
3. Probar primero microWakeWord con "Órbita" como baseline, antes de escribir arquitectura propia.

---

## Intro corta: qué es una red neuronal (aplicada a este caso)

1. **Es una función con parámetros.** Entra un vector de números y sale otro. Acá entra un **espectrograma** (energía por frecuencia a lo largo del tiempo) y sale **una probabilidad** de que se haya dicho "Órbita".
2. **Una neurona** hace `salida = f(w1·x1 + w2·x2 + … + b)`. Los `w` son pesos, `b` es el sesgo y `f` es una no linealidad (típicamente ReLU). Es un filtro FIR con una no linealidad después.
3. **Una capa** son muchas neuronas en paralelo. **Una red** son capas en cascada. Las capas **convolucionales** (CNN) usan un filtro chico que se desliza sobre el espectrograma (convolución 2D), por eso funcionan bien con audio.
4. **Entrenar** es ajustar los pesos para minimizar un error sobre ejemplos etiquetados ("Órbita" / "no Órbita"), con **descenso por gradiente** y **backpropagation**: se calcula cuánto cambia el error al mover cada peso, y se mueve cada uno un poco en la dirección que lo reduce.
5. **Inferencia** es evaluar la función con los pesos ya fijos. Es lo único que hace el ESP32: multiplicar y sumar, sin aprender nada.
6. **Cuantización int8:** los pesos pasan de float32 a enteros de 8 bits, con una escala y un cero por tensor. El modelo pesa 4 veces menos y corre más rápido, con poca pérdida de precisión. Es lo estándar en microcontroladores.
7. **Overfitting:** el modelo memoriza los ejemplos de entrenamiento y falla con voces nuevas. Se combate con más variedad de datos, aumentación (ruido, reverb, distancia) y un set de validación separado.

**Pipeline completo:**
`audio 16 kHz → (AFE) → ventanas de 30 ms → 40 bandas de frecuencia → red int8 → probabilidad → umbral y promedio en varias ventanas → evento dev_wake_word`

## Qué leer (en orden)

**Fundamentos (gratis):**
1. [3Blue1Brown, serie "Neural Networks"](https://www.manning.com/livevideo/3blue1brown-neural-networks): unas 4 horas de video; qué es una red, descenso por gradiente y backpropagation.
2. [Michael Nielsen, *Neural Networks and Deep Learning*](https://stafforini.com/works/nielsen-2015-neural-networks-and/): libro online gratuito con reconocimiento de dígitos y la matemática incluida. Leer capítulos 1 a 3 y el de CNN.

**Específico TinyML:**
3. *TinyML*, Pete Warden y Daniel Situnayake (O'Reilly, 2019, 504 páginas, [ficha](https://www.oreilly.com/library/view/-/9781492052036/)). Tiene una sección "Wake-Word Detection" (un capítulo para armar la aplicación y otro para entrenar el modelo) y un capítulo sobre TensorFlow Lite for Microcontrollers. No se confirmó que use `micro_speech` por nombre, y al ser de 2019 algunas herramientas habrán cambiado: verificar el código contra el repo actual.
4. [Tutorial oficial de TensorFlow: reconocimiento de audio simple](https://www.tensorflow.org/tutorials/audio/simple_audio?hl=es): entrena un modelo de palabras clave con espectrogramas sobre Speech Commands, apto para Colab. Un usuario del foro lo calificó de difícil para principiantes en decodificación de audio e inferencia sobre un archivo individual.
5. [Slides de Harvard TinyML sobre keyword spotting en ESP32-S3 (XIAO)](https://tinyml.seas.harvard.edu/EdgeMLUP-23/assets/slides/3.XIAO_ESP32S3-Keyword_Spotting.pdf): no se abrieron, se desconoce el nivel de detalle. El curso de edX de Harvard no se pudo verificar.

**Matemática de fondo:** alcanzan álgebra lineal y análisis de señales de la carrera. Lo nuevo es la regla de la cadena con derivadas parciales, que 3Blue1Brown explica.

## Código para estudiar
- [`espressif/esp-tflite-micro`](https://components.espressif.com/components/espressif/esp-tflite-micro): componente oficial para ESP-IDF, Apache-2.0. Incluye `hello_world`, `micro_speech` y `person_detection`. Para ESP32-S3 pide ESP-IDF 4.4 o superior. Se instala con `idf.py add-dependency "espressif/esp-tflite-micro^1.4.1"`. Las páginas de las versiones 1.4.x mencionan ESP32-P4 en la descripción y no se confirmó el detalle para S3: revisar el README de la versión usada. Tampoco se verificó su relación con `esp-nn`.
- [microWakeWord](https://github.com/OHF-Voice/micro-wake-word): pipeline completo de entrenamiento con TTS sintético; leerlo como ejemplo de arquitectura y preparación de datos.

## Plan de estudio sugerido
1. 3Blue1Brown y capítulos 1 a 3 de Nielsen (el concepto).
2. Correr `hello_world` de `esp-tflite-micro` en el DevKit: cómo se carga un `.tflite`, cómo se reserva memoria y cómo se invoca.
3. `micro_speech` (keyword spotting de punta a punta) y recién ahí el entrenamiento en Colab.
