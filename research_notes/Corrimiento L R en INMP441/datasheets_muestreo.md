# Instante de muestreo L/R en micrófonos MEMS I2S (INMP441, ICS-43432, ICS-43434, ICS-52000) según la documentación del fabricante

Fuentes primarias leídas completas (texto extraído de los PDF):
- INMP441, DS-INMP441-00 Rev 1.1 (21/05/2014) — [copia CERN](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf) (original TDK: [invensense.tdk.com](https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf))
- ICS-43434, DS-000069 Rev 1.2 (29/08/2016) — [copia Farnell](https://www.farnell.com/datasheets/2632666.pdf), [copia chipsmall](https://datasheet.chipsmall.com/ICS-43434/395684.pdf)
- ICS-43432, DS-000038 Rev 1.3 — [copia Farnell](https://www.farnell.com/datasheets/2905839.pdf)
- ICS-52000, DS-000121 Rev 1.3 — [copia Farnell](https://www.farnell.com/datasheets/2632667.pdf)
- Knowles SPH0645LM4H-B Rev B — [copia Adafruit](https://cdn-shop.adafruit.com/product-files/3421/i2S+Datasheet.PDF)

No se pudieron obtener completos: AN-000099 (los links de TDK devolvieron 404/HTML) ni AN-1140 (idem). Se usó solo la vista previa de AN-000099.

## 1. ¿Los datasheets dicen cuándo se toma la muestra respecto de los flancos de WS? ¿Muestrean L y R en el mismo instante?

### Takeaway
Solo el **ICS-43434** lo dice explícitamente: ambos micrófonos de un par estéreo muestrean **simultáneamente al comienzo del frame I2S (flanco descendente de WS)**; L y R difieren solo en *cuándo transmiten* (L tras el flanco descendente, R tras el ascendente). El **INMP441** y el **ICS-43432** solo dicen que quedan "in sync" por compartir WS, **sin especificar el instante de muestreo**. Ningún datasheet de TDK menciona un desfase de medio frame entre canales.

### Cited Findings
- ICS-43434, sección "Synchronizing Microphones" (pág. 13 de 21): "Stereo ICS-43434 microphones are synchronized by the WS signal, so audio captured from two microphones sharing the same clock will be in sync. **The two microphones will synchronously sample the acoustic signals at the beginning of the I2S frame (WS falling edge).**" — [ICS-43434 DS-000069 v1.2](https://www.farnell.com/datasheets/2632666.pdf)
- ICS-43434, "I²S Data Interface" (pág. 13): "When set to the left channel, the data will be output following WS's falling edge and when set to output on the right channel, data will be output following WS's rising edge." — [ICS-43434](https://datasheet.chipsmall.com/ICS-43434/395684.pdf)
- INMP441, "Synchronizing Microphones" (pág. 13): "Stereo INMP441 microphones are synchronized by the WS signal, so audio captured from two microphones sharing the same clock will be in sync. If the mics are enabled separately, this synchronization may take up to 0.35 ms after the enable signal is asserted while internal data paths are flushed." — **no menciona el instante de muestreo ni el flanco de WS**. — [INMP441 DS-INMP441-00 r1.1](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf)
- ICS-43432, "Synchronizing Microphones" (pág. 10 de 20): texto idéntico al INMP441 ("…will be in sync. If the mics are enabled separately, this synchronization may take up to 0.35 ms…"); tampoco especifica el instante. Misma descripción de salida L tras flanco descendente / R tras flanco ascendente. — [ICS-43432 DS-000038 r1.3](https://www.farnell.com/datasheets/2905839.pdf)
- INMP441 (pág. 11): "There must be 64 SCK cycles in each WS stereo frame… The L/R control pin determines whether the INMP441 outputs data in the left or right channel"; "the MSB of each word is delayed by one SCK cycle from the start of each half-frame"; SD queda en tri-state fuera de su medio frame (Fig. 8–10). — [INMP441](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf)
- Ninguno de los datasheets describe que el modulador Σ-Δ/decimador de cada mic esté sincronizado de distinta forma según L/R; describen el ADC como "single-bit, high-order, sigma-delta (Σ-Δ) running at a high oversampling ratio" seguido de filtro de decimación cuyas especificaciones "scale with sampling frequency" (es decir, derivado del reloj SCK/WS). — [INMP441 pág. 13–14](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf); [ICS-43434](https://www.farnell.com/datasheets/2632666.pdf)

### Inferences
- Para ICS-43434 la hipótesis "cada mic muestrea en su propio medio frame" está **refutada por el fabricante**. Para INMP441 **no está ni confirmada ni refutada por escrito**: la frase "will be in sync" sugiere alineación, pero no excluye que el canal R latchee su palabra en el flanco ascendente de WS (medio frame después).
- El INMP441 es silicio más viejo (heredado de Analog Devices, línea ADMP441 — [ADMP441 en alldatasheet](https://www.alldatasheet.net/html-pdf/475921/AD/ADMP441/54/2/ADMP441.html)) que el ICS-4343x; la aclaración explícita "WS falling edge" aparece recién en el ICS-43434 (2016). No hay base documental para extrapolar el comportamiento del ICS-43434 al INMP441.
- Signo: si el mic R tomara su muestra en el flanco ascendente de WS (T/2 después que L) y el receptor empareja L[n] con R[n] del mismo frame, R[n] contendría la señal de un instante posterior ⇒ en la correlación cruzada **R aparecería adelantado ~0,5 muestra respecto de L**, independientemente de la posición de la fuente. Eso es **coherente** con lo medido (R adelante 0,5–1 muestra). Es consistente con la hipótesis, pero no la prueba (ver Gaps).
- Un desfase de exactamente ±1 muestra apuntaría más bien a un problema de alineación de slots en el receptor (ESP32), no al mic; uno de ~0,5 es la firma esperable de muestreo por medio frame. Lo de "0,5–1" es ambiguo entre ambos.

### Gaps
- No hay en ningún documento de TDK/InvenSense hallado una afirmación explícita sobre el instante de muestreo del INMP441 o ICS-43432.
- No se encontró nota de aplicación de TDK que trate la sincronización de pares estéreo I2S (sí la hay para TDM, AN-000099). AN-1140 no pudo descargarse (404).

## 2. Group delay / latencia y "phase matching" entre micrófonos

### Takeaway
INMP441: group delay del filtro = **17,2/fS** (1078 µs a 16 kHz). ICS-43432/43434/52000: **2/fS** "acoustic input to digital output – includes filter and I2S serial output". Son definiciones distintas (filtro solo vs. cadena completa). Ningún datasheet especifica tolerancia de group delay ni phase matching entre unidades; el único matching especificado es el de sensibilidad (±1 dB).

### Cited Findings
- INMP441, Tabla de especificaciones "Digital Filter" (pág. 4): "Group Delay 17.2/fS sec; fS = 48 kHz: 359 µs; fS = 16 kHz: 1078 µs"; ripple ±0,04 dB. — [INMP441](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf)
- ICS-43434, Tabla 4 "Digital Filter Characteristics" (pág. 6): "Group Delay — Acoustic input to digital output – includes filter and I2S serial output — 2/fS sec."; ripple ±0,3 dB; atenuación 58 dB. — [ICS-43434](https://datasheet.chipsmall.com/ICS-43434/395684.pdf)
- ICS-43432 (pág. 4): misma cifra 2/fS con la misma definición. — [ICS-43432](https://www.farnell.com/datasheets/2905839.pdf)
- ICS-52000 (pág. 4): "Group Delay — Acoustic input to digital output – includes filter and TDM serial output — 2/fS sec". — [ICS-52000](https://www.farnell.com/datasheets/2632667.pdf)
- Filtros: INMP441 banda pasante 0,423·fS, −6 dB en 0,5·fS, >60 dB de rechazo, especificaciones que escalan con fS (pág. 13–14); ICS-43434 banda pasante 0,417·fS. — [INMP441](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf); [ICS-43434](https://www.farnell.com/datasheets/2632666.pdf)
- ICS-43434 se publicita con sensibilidad ±1 dB "which enables high-performance microphone arrays without the need for system calibration" (descripción general). — [ICS-43434](https://www.farnell.com/datasheets/2632666.pdf)
- INMP441 y ICS-43432: "If the mics are enabled separately, this synchronization may take up to 0.35 ms… while internal data paths are flushed." — [INMP441](https://microcontroller-course.web.cern.ch/Project_Material/resources/INMP441.pdf)

### Inferences
- El group delay es una latencia común a ambos mics (mismo modelo, mismo reloj): se cancela en la diferencia L−R y **no explica** un desfase entre canales. Solo importaría si se mezclan modelos distintos (p. ej. INMP441 con ICS-43434: 17,2/fS vs 2/fS → ~15 muestras de diferencia, aunque las definiciones no son comparables).
- Que el group delay del INMP441 sea fraccionario (17,2 muestras) muestra que la cifra no está referida a un flanco de WS concreto; no aporta información sobre el instante L vs R.
- La frase "internal data paths are flushed" sugiere que la ruta interna (decimador) se re-alinea al WS tras el enable; no dice a qué flanco.

### Gaps
- No hay especificación de tolerancia de fase/group delay pieza a pieza en ningún datasheet consultado.

## 3. TDM (ICS-52000) y notas de aplicación: ¿TDK vende TDM por garantizar muestreo sincrónico?

### Takeaway
Sí: el ICS-52000 se anuncia explícitamente por "muestreo sincrónico" de hasta 16 canales, con una secuencia de sincronización al arranque (AN-000099). Pero los datasheets I2S de TDK también afirman sincronización por WS; TDK no presenta el TDM como solución a un desfase L/R de I2S, sino como forma de escalar >2 mics en un bus.

### Cited Findings
- ICS-52000, descripción general (pág. 1): "The TDM interface allows an array of up to 16 of [these microphones]… [to] sample their acoustic signals synchronously, enabling precise array processing"; feature: "Supports TDM arrays of up to 16 synchronously-sampled channels". — [ICS-52000](https://www.farnell.com/datasheets/2632667.pdf)
- ICS-52000, "Synchronizing Microphones" (pág. 11): "ICS-52000 microphones are synchronized by the WS signal, so audio captured from multiple microphones sharing the same clock will be sampled synchronously." WS se encadena (WSO → WS del siguiente); "The ICS-52000's WS clock input is sampled on the rising edge of SCK". — [ICS-52000](https://www.farnell.com/datasheets/2632667.pdf)
- ICS-52000, "Startup" (pág. 10): WS debe arrancar ≥10 ms después de SCK "to completely initialize before starting the synchronization sequence with other microphones in the TDM array"; datos válidos tras 262 144 ciclos de SCK (85 ms a 3,072 MHz). — [ICS-52000](https://www.farnell.com/datasheets/2632667.pdf)
- AN-000099 "Synchronous Sampling with an Array of ICS-52000 TDM Microphones" (v1.2, 2017): "all of the ICS-52000 microphones on a common bus will sample their acoustic signals simultaneously"; la sincronización integrada al arranque evita que los algoritmos DSP necesiten "a complicated series of delays and/or buffers to pre-process the signals". — [vista previa Sekorm de AN-000099](https://en.sekorm.com/doc/3941726.html); [PDF TDK](https://invensense.tdk.com/wp-content/uploads/2017/08/AN-000099-v1.2.pdf) (no accesible al momento de la consulta)
- Estado del ICS-52000: listado como discontinuado/last-time-buy por distribuidores. — [CDI](https://www.cdiweb.com/products/detail/ics52000-tdk-invensense/597439/)

### Inferences
- En TDM cada mic transmite en un slot distinto (hasta 16 slots en un frame), pero el fabricante afirma muestreo simultáneo: o sea, TDK separa explícitamente "instante de muestreo" (común) de "slot de transmisión" (distinto). El ICS-43434 aplica la misma lógica para estéreo I2S. Para el INMP441 esa separación no está escrita.
- Para la PCB de Órbita: si el desfase resulta inherente al INMP441, migrar a ICS-43434 (mismo pinout/función I2S, sincronía documentada) sería la corrección más barata; el ICS-52000 no se justifica para 2 mics y además está discontinuado.

### Gaps
- No se pudo leer AN-000099 completo ni AN-1140 (Microphone Array Beamforming, AN-1140-00 r1.0, 2013), AN-1124, AN-1165, AN-1112; no se verificó si contienen afirmaciones sobre pares I2S estéreo.

## 4. Otros fabricantes (Knowles SPH0645) y si el desfase de medio frame es un fenómeno conocido

### Takeaway
El datasheet del Knowles SPH0645LM4H-B describe solo la lógica de tri-state por WS/SELECT y tiempos de interfaz; **no dice cuándo muestrea** ni afirma sincronía entre L y R. No se encontró documentación de fabricante que reconozca un desfase de medio frame entre canales de un par I2S.

### Cited Findings
- SPH0645LM4H-B Rev B, "Interface Description" (pág. 6): "The SEL pin determines when the microphone drives the Data pin. When SEL=H the Data pin is driven when the WS=H, otherwise it is tri-stated"; "The Over Sampling Rate is fixed at 64 therefore the WS signal must be BCLK/64"; formato I2S 24 bit, "Data Precision is 18 bits". — [SPH0645 datasheet](https://cdn-shop.adafruit.com/product-files/3421/i2S+Datasheet.PDF)
- SPH0645, tabla de timing: "Data Delay from CLK Rising Edge tDC max 65.92 ns"; WS setup/hold 85,45 ns; no hay parámetro de group delay ni sección de sincronización de micrófonos. — [SPH0645](https://cdn-shop.adafruit.com/product-files/3421/i2S+Datasheet.PDF)
- Foros ESP32 con 2x INMP441 reportan problemas de canal (señal útil solo en un canal, datos aleatorios en el otro) atribuibles a configuración/cableado, no a desfase temporal de medio frame. — [esp32.com p=153122](https://esp32.com/viewtopic.php?p=153122); [esp32.com "I2S does not work in STEREO with INMP441"](https://esp32.com/viewtopic.php?p=123226)

### Inferences
- La ausencia de reportes documentados no descarta el fenómeno: con mics separados pocos cm, 0,5 muestra a 16 kHz (31 µs ≈ 10,7 mm de camino acústico) solo se nota en TDOA/beamforming, no en escucha.
- Para beamforming a 50 mm (frente Audio): 31 µs equivalen a ~73 % del retardo máximo endfire (50 mm/343 m/s ≈ 146 µs ≈ 2,3 muestras a 16 kHz). Si el desfase es real y fijo, es corregible con un retardo fraccional constante en software; pero hay que confirmarlo antes de diseñar el AFE alrededor de él.

### Gaps
- No se consultaron datasheets ST (PDM, otra arquitectura: el muestreo lo define el decimador del host) ni Vesper; no se hallaron notas de Knowles sobre timing L/R.
- Prueba empírica sugerida (no documentada por fabricante): intercambiar los pines L/R de los dos INMP441 (o intercambiar físicamente los mics). Si el adelanto sigue en el *canal* R → efecto de slot/instante de muestreo (mic o receptor); si sigue al *mic físico* → diferencia de fabricación/acústica. Alternativamente, medir con fuente equidistante (broadside) y ver si el desfase cambia con fS (un efecto de medio frame escala con 1/fS en tiempo y queda fijo en ~0,5 muestra).
