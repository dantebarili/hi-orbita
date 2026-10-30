# Corrimiento fijo L/R entre 2 INMP441 en un mismo bus I2S: experiencias, medición y compensación

Contexto: ESP32-S3, 2x INMP441 en un bus I2S (L/R), 16 kHz, separación ~21 mm, arreglo diferencial endfire de 1er orden. Se mide R adelantado ~0.5–1 muestra respecto de L, independiente de la posición de la fuente.

Números base (cálculo propio): τ0 = d/c = 0.021/343 ≈ 61.2 µs; T = 1/16 kHz = 62.5 µs → **τ0 ≈ 0.98 muestras**. Un error de 0.5 muestra (31 µs) equivale a ~51 % del retardo acústico máximo entre mics; 1 muestra equivale a todo el retardo endfire.

## 1. ¿Hay reportes de un retardo L/R constante en pares I2S MEMS? Magnitud y causa atribuida

### Takeaway
No se encontró ningún reporte público (GitHub, esp32.com, Hackaday, foros Pi) que documente un corrimiento fijo de ~0.5 muestra entre L y R en un mismo bus I2S con INMP441/ICS-43434/SPH0645. Los problemas reportados son de alineación de bits/slots o de orden de canales, y los proyectos de DOA con INMP441 no calibran el offset: usan la geometría y advierten que el desalineo de canales degrada la estimación.

### Cited Findings
- El INMP441 usa formato I2S estándar: el MSB sale **un ciclo de SCK** después del flanco de WS (offset de 1 bit, no de una muestra). Un formato mal configurado (Philips vs. MSB/left-justified) corre los datos un bit. — [Resumen de datasheet INMP441 (Farnell)](https://www.farnell.com/datasheets/1824785.pdf); [ESP32 Forum t=36764](https://www.esp32.com/viewtopic.php?t=36764)
- El ICS-43434 también entrega 24 bits en complemento a 2, MSB primero, "con un ciclo de SCK de retardo en el dato". La fuente es un foro de TDK, no el datasheet. — [TDK InvenSense community](https://adm.invensense.tdk.com/comment/13230)
- No se encontraron cifras de group delay ni una especificación de sincronismo entre los dos mics de un par estéreo en las búsquedas de los datasheets INMP441/ICS-43434. — [DS-000069 ICS-43434 v1.2](https://cdn.sparkfun.com/assets/a/9/c/9/1/DS-000069-ICS-43434-v1.2.pdf)
- En estéreo I2S estándar, el canal izquierdo se transmite durante WS bajo y el derecho durante WS alto. Por lo tanto, en el cable las palabras L y R están separadas medio frame. — [Jimmy Wong IoT (nRF52)](https://jimmywongiot.com/2021/06/26/i2c-mems-microphone-with-nordic-nrf52-series/)
- ESP-IDF (ESP32-S3), modo **PDM RX**: "The right slot is received first in stereo mode". Se puede invertir el orden con el flag de clock invert. — [ESP-IDF I2S, ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)
- En los foros de ESP32 se repiten problemas de orden de canales, como "swap the odd/even samples" o la señal útil solo en índices pares. Un hilo sobre el DAC lo atribuye al orden de palabras de 16 bits (MSW primero vs. little-endian). — [ESP32 Forum p=153138](https://esp32.com/viewtopic.php?p=153138); [ESP32 Forum p=110655](https://esp32.com/viewtopic.php?p=110655)
- esp-hal #5292 (ESP32-S3): el RX quedaba desalineado del frame un número **aleatorio de bits** por arranque (offsets 26, 24, 1, 9, 31). El síntoma era la ganancia escalada por potencias de 2, no un retardo L/R. La causa fue la configuración de loopback/slave en un setup full-duplex. — [esp-rs/esp-hal #5292](https://github.com/esp-rs/esp-hal/issues/5292)
- Congvinh167/DOA-Estimation (ESP32-S3, 4x INMP441, 2 buses I2S con clock compartido, 48 kHz, GCC-PHAT/SRP-PHAT): no calibra offsets. Calcula los retardos desde la geometría y advierte que "channel misalignment can degrade estimates". También recomienda "verify the actual channel assignment with a known source". — [GitHub Congvinh167/DOA-Estimation](https://github.com/Congvinh167/DOA-Estimation)
- Davisbe/esp32_sound_localization (3 ESP32 separados): uno de los nodos aparecía ~0.1 ms atrasado y el TODO propone un offset predeterminado. Es un caso multi-placa, no comparable con un par en un mismo bus. — [GitHub Davisbe](https://github.com/Davisbe/esp32_sound_localization)
- La literatura de patentes reconoce que los mics MEMS no tienen fase perfectamente apareada y propone corregir el desajuste de fase entre mics. — [US10694304 "Phase response mismatch correction"](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10694304)

### Inferences
- **Hipótesis A (la más probable para la parte constante de ~0.5 muestra):** cada INMP441 fija el instante de su muestra (salida del decimador) relativo a **su propio semiframe**, de modo que L y R quedan muestreados con medio período de WS de diferencia (0.5 muestra = 31.25 µs a 16 kHz). Así, el driver empareja (L_n, R_n) que no son simultáneos. Es consistente con que el offset no dependa de la posición de la fuente. **No está confirmado:** ningún datasheet consultado especifica el instante de muestreo respecto de WS. Además, el signo observado (R adelantado) depende del pipeline interno del mic y del orden en que el driver empaqueta los slots, y no se pudo verificar.
- **Hipótesis B (parte entera, 1 muestra):** el driver/DMA empareja R_{n} con L_{n+1} porque el frame arranca en el slot equivocado o por `ws_pol`/orden de slots. Daría exactamente ±1 muestra (o ±0.5 combinado con A) y podría cambiar entre arranques o configuraciones.
- **Hipótesis C (dispersión 0.5→1):** el rango 0.5–1 en vez de un valor fijo sugiere que a una parte eléctrica constante se le suma error de estimación (reverberación, fuente no equidistante, campo cercano, interpolación del pico de GCC-PHAT) o desajuste acústico (puerto, agujero del módulo, junta). Hay que separarlas con el experimento de la sección 5.
- No es un problema de 1 bit: un desplazamiento de un ciclo de SCK afecta la amplitud o el signo, no la temporización entre canales.

### Gaps
- No se encontró ningún reporte comunitario que mida explícitamente el retardo L/R de un par INMP441 en un mismo bus, ni el valor de group delay del INMP441/ICS-43434. El PDF del INMP441 en Farnell no se pudo leer completo (timeout), así que no se verificó si el datasheet dice algo sobre el instante de muestreo.
- No se encontró documentación de ODAS, ReSpeaker, Matrix Voice ni miniDSP UMA sobre este efecto concreto. En general esos productos usan códecs multicanal, PDM o TDM con muestreo simultáneo, no pares I2S estéreo; esto es inferencia y no se verificó.

## 2. Métodos de medición usados o recomendados

### Takeaway
Los proyectos de la comunidad usan GCC-PHAT con interpolación del pico y "fuente conocida" para verificar asignación y retardos. No hay un protocolo publicado de calibración de offset L/R. La forma robusta de medirlo con precisión sub-muestra es la pendiente de fase de la función de transferencia cruzada con un chirp, más la inversión endfire 0°/180°.

### Cited Findings
- GCC-PHAT para los pares de mics, con interpolación de la respuesta de lag en los retardos de la LUT. — [Congvinh167/DOA-Estimation](https://github.com/Congvinh167/DOA-Estimation)
- La sugerencia genérica es medir con un tono de prueba de geometría conocida y restar el lag residual. Es una síntesis del buscador y no una fuente primaria. — [Davisbe](https://github.com/Davisbe/esp32_sound_localization)
- STM32 con GCC-PHAT y CMSIS-DSP para TDOA entre MEMS sincronizados: es una referencia de implementación. — mencionado en la [búsqueda de GitHub (topic sound-localization)](https://github.com/topics/sound-localization?l=c), no se verificó el repo.

### Inferences
- **Estimador recomendado:** H(f) = S_LR(f)/S_LL(f) con un chirp logarítmico o MLS de 100 Hz a 6 kHz, promediado por Welch. Se ajusta la pendiente de fase φ(f) = −2πf·Δ con mínimos cuadrados ponderados por coherencia. Da una resolución de µs (fracciones pequeñas de muestra) sin depender de interpolar un pico. Si φ(f) es lineal con ordenada ~0, es retardo puro (eléctrico o geométrico). Si tiene curvatura o una ordenada distinta de 0, hay desajuste de fase acústico o de mic.
- **Inversión endfire:** si se mide con la fuente en 0° (eje del arreglo) y en 180°, D(0°) = +τ0 + ε y D(180°) = −τ0 + ε. Entonces **ε = [D(0°)+D(180°)]/2** sin depender de la distancia ni de c, y τ0 = [D(0°)−D(180°)]/2 sirve de chequeo de geometría (≈0.98 muestras).

### Gaps
- No se encontraron publicaciones de makers con este método aplicado a INMP441. Es una práctica estándar de calibración de arreglos (inferencia propia).

## 3. Opciones de compensación

### Takeaway
Si el offset es eléctrico y determinístico (el mismo en todas las placas), se compensa con una constante en firmware. La forma más barata es absorberlo en el retardo que el beamformer diferencial ya aplica (T_d ≈ τ0), sin agregar un filtro. Para que el muestreo sea simultáneo por hardware, las alternativas son PDM estéreo (mismo clock y mismo decimador en el ESP32-S3) o TDM (ICS-52000).

### Cited Findings
- ESP32-S3 PDM RX: "each data line can be connected to two PDM MICs (left and right slots)"; hasta 4 líneas de datos en PDM RX (8 mics). TDM: hasta 16 slots (4 slots a 32 bits). — [ESP-IDF I2S, ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)
- En PDM, el pin LR del mic elige en qué flanco del clock maneja el dato. Los dos mics comparten la línea y no hay word clock. — [atomic14 blog](https://blog.atomic14.com/2025/10/10/prm-microphone-mystery)
- Hay reportes mixtos de stereo PDM en ESP32, con ruido o distorsión en algunos usuarios. — [ESP32 Forum PDM stereo](https://esp32.com/viewtopic.php?p=110131)
- ICS-52000 (TDM): la secuencia de WS en daisy-chain hace que cada mic "is properly configured to sample at the same time as all other mics in the array". Arreglos en distintos buses TDM con el mismo clock siguen sincronizados. — [AN-000099 TDK](https://invensense.tdk.com/wp-content/uploads/2017/08/AN-000099-v1.2.pdf)
- Un usuario preguntó si la propagación de WS por la cadena agrega retardo entre mics y no tuvo respuesta pública. — [TDK community](https://adm.invensense.tdk.com/comment/13104)
- Opciones de ESP-IDF STD que afectan el empaquetado de slots: `ws_pol` ("high level first"), `bit_shift` (Philips) y `left_align`. — [ESP-IDF I2S](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)

### Inferences
- **Parte entera:** se corrige corriendo el índice (gratis). Antes conviene revisar `ws_pol`/slot mask y descartar la Hipótesis B por configuración.
- **Parte fraccionaria, opción 1 (recomendada):** el DMA de 1er orden ya hace y = x_front(t) − x_back(t − T_d) con un retardo fraccionario T_d ≈ τ0 ≈ 0.98 muestra. Se usa **T_d,ef = T_d − ε** con el mismo filtro y costo cero. El signo se fija según qué mic es "front".
- **Opción 2:** se agrega un retardo fraccionario a un canal. Un allpass de Thiran de 1er orden es muy barato, tiene buena fase a baja frecuencia y error de group delay hacia fs/2. Un FIR de Lagrange/Farrow de 4 taps (cúbico) da un retardo casi plano hasta ~0.3–0.4 fs. Un rotador de fase e^{−jωε} cuesta casi nada si ya se trabaja en STFT. En un DMA, el error de fase a alta frecuencia importa menos que en baja, porque el diferencial pierde SNR en graves y ahí la precisión de fase es crítica.
- **Opción 3:** si se va a usar ESP-AFE para el beamforming, no se encontró si permite inyectar un offset por canal. Lo más seguro es corregir el offset antes de entregar el audio al AFE.
- **PDM estéreo en el S3:** los dos mics comparten el clock (~1–3 MHz) y el ESP32-S3 decima ambos con el mismo filtro. El desfase residual sería de medio período de clock PDM, ~0.5 µs ≈ 0.008 muestra (cálculo propio con clock de 1.024 MHz). Eso eliminaría el problema por diseño, pero cambia el mic (el INMP441 no es PDM) y el pinout. Ojo con el orden de slots ("right slot is received first").
- **Producción:** un offset eléctrico de medio frame se corrige con una constante de firmware sin calibración por unidad. Un desajuste acústico (puerto, junta, tolerancia de fase de la cápsula) es por unidad y exige calibrar en fin de línea o usar un DMA adaptativo (cardioides back-to-back con mezcla adaptativa) que lo tolere. Hay que decidirlo antes de la PCB, porque condiciona el test de fin de línea.

### Gaps
- No se verificó si ESP-AFE/ESP-SR expone parámetros de calibración de retardo o ganancia por mic.
- No se verificó el retardo real entre mics en la cadena ICS-52000.

## 4. Sensibilidad del DMA endfire de 1er orden (21 mm, 16 kHz) a 0.5 muestra de error

### Takeaway
El efecto es grave. Con τ0 ≈ 0.98 muestra, un error de 0.5 muestra (≈0.51·τ0) convierte un cardioide en otro patrón: el nulo se mueve de 180° a ~119° o desaparece, y el rechazo trasero en graves cae de idealmente infinito a ~9–14 dB. Para mantener ≥25 dB de rechazo trasero en baja frecuencia, el error residual tiene que ser ≲0.1 muestra (≈7 µs).

### Cited Findings
- Los DMA de 1er orden son sensibles a las imperfecciones del sensor (error de ganancia, de fase y autoruido). Yan, Huang, Kleijn y Abhayapala (2022) analizan errores de fase acotados y no acotados en LDMA de 1er orden y derivan umbrales de WNG para evitar que el lóbulo principal se desoriente. — [Phase Error Analysis for First-Order LDMAs (WGTN)](https://openaccess.wgtn.ac.nz/articles/conference_contribution/Phase_Error_Analysis_for_First-Order_Linear_Differential_Microphone_Arrays/22106507/1)
- Por la alta amplificación de ruido blanco, los DMA son susceptibles al desajuste de amplitud y fase. Se proponen la profundidad y el ancho del nulo como métricas. — [arXiv 2506.21043](https://www.arxiv.org/pdf/2506.21043)
- Benesty y Chen, *Study and Design of Differential Microphone Arrays*: diseño de DMA de 1er orden, WNG y robustez. — [Springer](https://www.springerprofessional.de/study-and-design-of-differential-microphone-arrays/3988454)
- El diseño robusto de peor caso con imperfecciones de sensor para DMA de 1er orden está en Tu y Chen (2022). — [búsqueda/IEICE](https://global.ieice.org/en_transactions/fundamentals/10.1587/transfun.E101.A.1687/_p) (no leído en detalle)
- Los arreglos endfire diferenciales son "very sensitive to mismatch" y la mejora de SNR casi desaparece con elementos desapareados; a ~20 mm la sensibilidad de señal cae. — [Patente "Vector noise cancellation" US9100735](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9100735); [UiO, apuntes end-fire array](https://www.mn.uio.no/fysikk/english/people/aca/sverre/lecturenotes/2024_end-fire_array.pdf)
- Introducción a la formulación del beamforming diferencial. — [MathWorks](https://it.mathworks.com/help/phased/ug/introduction-to-differential-beamforming.html)

### Inferences (cálculo propio, modelo ideal sin error de ganancia)
- y = x1(t) − x2(t − T_d) da |H| ∝ |1 − e^{−jω(T_d + τ0·cosθ)}|, con nulo en cos θ_null = −T_d/τ0. El error eléctrico ε se suma a T_d.
- Cardioide (T_d = τ0):
  - ε = −0.5 muestra (−0.51 τ0): cos θ = −0.49, con el nulo en **~119°** (patrón tipo hipercardioide).
  - ε = +0.5 muestra: cos θ = −1.51, **sin nulo real** (patrón subcardioide).
- Rechazo a 180° en baja frecuencia ≈ |ε|/(2τ0+ε): con ε = +0.51τ0 es −13.8 dB y con ε = −0.51τ0 es −9.3 dB. El ideal es −∞.
- Tolerancia: para un rechazo ≥ 25 dB, |ε| ≲ 0.056·2τ0 ≈ 6.9 µs ≈ 0.11 muestra. Para 30 dB, ≈ 3.9 µs ≈ 0.06 muestra. El desajuste de ganancia también limita el nulo, así que el presupuesto real de ε es todavía menor.
- Con 1 muestra de error sin compensar, una fuente broadside parece endfire (D ≈ τ0). Esto invalida cualquier DOA o TDOA con este par.
- Si se agranda la separación (por ejemplo a los 50 mm que el CLAUDE.md menciona para el beamforming con ESP-AFE), τ0 ≈ 2.3 muestras y el mismo ε pesa 2.3 veces menos. A cambio aparece aliasing espacial antes (d < λ/2 → f < 3.4 kHz a 50 mm). Hay que verificar qué separación es la vigente: los 21 mm del contexto no coinciden con los 50 mm del CLAUDE.md.

### Gaps
- No se obtuvieron los resultados numéricos de Elko ni de Benesty sobre la tolerancia de fase. Las cifras de arriba son cálculo propio y conviene validarlas con una simulación del beampattern.

## 5. Experimento de verificación (¿eléctrico o acústico?) y plan de compensación

### Takeaway
La matriz de 4 mediciones (intercambiar mics físicamente × intercambiar pines L/R) más la inversión endfire 0°/180° separa sin ambigüedad tres términos: el slot eléctrico (sigue al pin), la posición acústica (sigue al lugar) y el mic individual (sigue a la cápsula). Repetir con otra fs o con otros arranques distingue "medio frame" (constante en muestras) de un retardo analógico (constante en µs).

### Cited Findings
- Se recomienda verificar la asignación de canales con una fuente conocida antes de interpretar ángulos. — [Congvinh167/DOA-Estimation](https://github.com/Congvinh167/DOA-Estimation)
- La selección L/R se hace con el pin L/R: GND = izquierdo y VDD = derecho. — [Adafruit I2S MEMS mic guide](https://learn.adafruit.com/adafruit-i2s-mems-microphone-breakout?view=all)

### Inferences (protocolo propuesto)
1. **Preparación.** Grabar 32-bit/slot crudo sin AFE, a 16 kHz. Usar un chirp log de 100 Hz a 7 kHz de 2 s, repetido 10 veces, desde un parlante a ≥1 m en una sala tratada o al aire libre. Estimar Δ con la pendiente de fase de S_LR/S_LL, ponderada por coherencia, entre 200 Hz y 4 kHz. Reportar Δ en muestras y la ordenada de fase.
2. **Inversión endfire (geometría-independiente).** Medir con la fuente en 0° y en 180°: ε = [D(0°)+D(180°)]/2, y τ0 medido = [D(0°)−D(180°)]/2 ≈ 0.98 muestras. Este dato solo ya da el offset a compensar.
3. **Matriz 2×2.** La configuración base es mic A en posición 1 con slot L y mic B en posición 2 con slot R. Después:
   - (a) intercambiar físicamente A↔B sin tocar los pines;
   - (b) volver a la base e intercambiar solo los straps L/R (A→R, B→L);
   - (c) hacer las dos cosas.

   Con la fuente en broadside o con la inversión 0°/180° en cada caso:
   - la parte que **cambia de signo con el strap** es eléctrica (slot);
   - la que cambia con la posición es acústica (montaje o puerto);
   - la que sigue a la cápsula es intrínseca del mic.
4. **Constante en muestras o en tiempo.** Repetir a 32 kHz (o con otra fs válida para el INMP441). Si ε sigue ≈ 0.5 muestra, la causa es el medio frame. Si se mantiene en ~31 µs, la causa es analógica o acústica. Repetir en 5 o más arranques: si ε salta de a 1 muestra entre arranques, el problema es el emparejamiento de slots en el driver (Hipótesis B).
5. **Cavidad común (opcional).** Poner los dos puertos en un acoplador pequeño cerrado con un parlante (presión uniforme, dimensiones ≪ λ). El retardo acústico es ~0, así que lo que se mida es eléctrico más la diferencia entre mics.
6. **Varias unidades.** Repetir en 2 o 3 pares de INMP441 para estimar la dispersión entre unidades y decidir si alcanza con una constante de firmware o hace falta calibrar por unidad.

**Plan de compensación:**
- Si el término de slot es ≈0.5 muestra constante en todas las placas, fijarlo como constante en firmware (parte entera con corrimiento de índice, parte fraccionaria absorbida en T_d del DMA). Documentarlo en el vault como característica del bus I2S.
- Si el residuo por unidad es > ~0.1 muestra, agregar calibración en fin de línea (chirp más inversión, guardando ε y la ganancia en NVS), o pasar a PDM estéreo (mismo clock y decimador del S3) o a TDM.
- Corregir también la ganancia: el nulo depende de los dos desajustes.
- Validar con el beampattern medido: girar la fuente cada 15° y comprobar la profundidad del nulo ≥ 20–25 dB en 300 Hz–3 kHz.

### Gaps
- No se encontró un protocolo publicado de este tipo aplicado a INMP441. Es un diseño propio basado en principios estándar de calibración de arreglos.
