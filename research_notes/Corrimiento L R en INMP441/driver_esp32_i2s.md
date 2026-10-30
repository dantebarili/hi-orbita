# Driver / periférico I2S del ESP32-S3 y corrimiento L/R

Contexto verificado en el repo: `firmware/components/orbita_audio/audio_capture.c:92` usa `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)` (master RX). Fuentes de código leídas: ESP-IDF `release/v5.4` (`i2s_std.h`, `i2s_hal.c`, `hal/esp32s3/i2s_ll.h`, `hal/esp32/i2s_ll.h`). TRM ESP32-S3 v1.8, cap. 28.

## 1. Cómo arma el ESP32-S3 los slots RX en el buffer DMA (TRM + driver)

### Takeaway
En el S3 el modo STD es TDM de 2 slots: el frame arranca en el flanco de WS que define `ws_pol` (default `false` → WS bajo = slot 0 = izquierdo) y los dos slots de un mismo período de WS se escriben consecutivos al FIFO/DMA (L, R). Con la config del repo no hay mecanismo documentado que empareje R del frame n con L del frame n+1; eso solo pasaría con WS invertido (`ws_pol=true` o `invert_flags.ws_inv=true`), y además aparecería como canales intercambiados.

### Cited Findings
- En el S3 (HW v2), `i2s_ll_rx_enable_std()` simplemente llama a `i2s_ll_rx_enable_tdm()`: el modo STD es TDM — [i2s_ll.h esp32s3, v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/esp32s3/include/hal/i2s_ll.h)
- `i2s_ll_rx_select_std_slot()` fija `rx_tdm_tot_chan_num = 1` (2 slots) y habilita canal 0 (LEFT=0x01), canal 1 (RIGHT=0x02) o ambos — [mismo archivo](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/esp32s3/include/hal/i2s_ll.h)
- `i2s_hal_std_set_rx_slot()` en HW v2 aplica: `rx_msb_shift = bit_shift`, `ws_width`, `rx_half_sample_bits = slot_bit_width`, `rx_ws_idle_pol = ws_pol`, `bit_order_lsb`, `left_align`, `big_endian`. No hay en HW v2 campo `msb_right` ni `right_first` (esos son solo ESP32/S2) — [i2s_hal.c v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/i2s_hal.c); [i2s_std.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/esp_driver_i2s/include/driver/i2s_std.h)
- Defaults de `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` (HW v2): `ws_width = bits_per_sample`, `ws_pol = false`, `bit_shift = true`, `left_align = true`, `slot_mask = BOTH` en estéreo. MSB: igual pero `bit_shift = false` — [i2s_std.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/esp_driver_i2s/include/driver/i2s_std.h)
- TRM §28.10.1.1: en TDM RX, `I2S_RX_TDM_TOT_CHAN_NUM` fija cuántos canales; `I2S_RX_TDM_CHANn_EN=1` → "this channel data is valid and will be stored into RX FIFO"; en master, WS lo controlan `I2S_RX_WS_IDLE_POL` ("default level of WS") y `I2S_RX_TDM_WS_WIDTH`; "I2S_RX_HALF_SAMPLE_BITS x 2 is equal to the BCK cycles in one WS period" — [ESP32-S3 TRM v1.8, cap. 28, p. 1054–1055](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
- TRM, registro `I2S_RX_CONF_REG`: `I2S_RX_WS_IDLE_POL` "0: WS remains low when receiving left channel data, and remains high when receiving right channel data. 1: WS remains high when receiving left channel data…" — [TRM v1.8, Reg. 28.5, p. 1062](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
- TRM §28.5: `I2S_RX_MSB_SHIFT` 0 → WS y SD cambian a la vez (MSB); 1 → "WS signal changes one BCK clock cycle earlier than SD signal" (Philips/PCM). El corrimiento de Philips es de **1 BCK**, no de un slot — [TRM v1.8 §28.5, p. 1042–1043](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
- TRM §28.10.2: el dato se guarda MSB primero (`I2S_RX_BIT_ORDER`); ancho de almacenamiento por `I2S_RX_BITS_MOD`/`I2S_RX_24_FILL_EN`; si el ancho recibido (`I2S_RX_TDM_CHAN_BITS`) es mayor que el almacenado, `I2S_RX_LEFT_ALIGN` elige si se guardan los bits altos (1) o bajos (0) — [TRM v1.8 §28.10.2.2–28.10.2.3, p. 1056](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf)
- Guía IDF (ESP32-S3): en RX estéreo las muestras llenan el buffer en secuencia; en mono-left caen en posiciones alternas; si se cambia `slot_bit_width` a mano hay que actualizar `ws_width`. No hay nota de swap L/R en modo STD; la única nota de swap es para **PDM RX** (el slot derecho llega primero, se corrige invirtiendo el clock) — [ESP-IDF I2S, esp32s3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html)
- TRM §28.8.2: en master RX, "RX unit keeps outputting clock signal and sampling input data" desde `I2S_RX_START`; `i2s_ll_rx_start()` hace `rx_update` y luego `rx_start` — [TRM v1.8 p. 1047](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf); [i2s_ll.h esp32s3](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/esp32s3/include/hal/i2s_ll.h)

### Inferences
- Con `ws_pol=false` el frame TDM = [WS bajo → slot 0 (mic con L/R=GND)] + [WS alto → slot 1 (mic con L/R=VDD)], ambos del mismo período de WS → buffer `[L_n, R_n, L_n+1, R_n+1…]`. El emparejamiento R(n-1)+L(n) no surge de la config del repo.
- Si WS estuviera invertido (`ws_pol=true` o `gpio_cfg.invert_flags.ws_inv=true`), el frame arrancaría en WS alto: slot 0 = mic R, slot 1 = mic L de la **mitad siguiente** → pares `(R_n, L_n+1)`. Se vería como swap L/R; si alguien "lo arregla" intercambiando índices en software, queda un corrimiento de medio frame (L atrasado ½ muestra respecto de R → R "adelantado"). Vale verificar que `ws_inv` esté en `false`.
- El bit-shift de Philips mal elegido (Philips vs MSB contra el mic) corre el dato **1 bit** (factor ×2 de amplitud / pérdida del LSB o del signo), no desplaza en el tiempo entre canales. INMP441 es formato I2S (Philips), así que el default es el correcto (ver gap sobre datasheet).
- Un error estructural de slot daría un corrimiento **entero** (1 muestra = 62,5 µs a 16 kHz) y exactamente constante; un valor medido de "~0,5–1 muestra" fraccional apunta más a mitad de frame (ver punto anterior), a instantes de muestreo distintos dentro del mic, o al método de estimación, que a un bug de DMA.
- En master RX el periférico genera WS desde cero, así que no debería arrancar a mitad de frame (no verificado en TRM explícitamente). En **slave** sí podría engancharse a mitad de frame → swap de slot entero.

### Gaps
- El TRM no tiene una frase explícita "slot 0 se escribe antes que slot 1 en memoria"; se infiere de §28.10.1.1 + doc IDF ("in sequence").
- No se pudo leer el datasheet INMP441 (el link de TDK devolvió HTML). Falta confirmar: (a) en qué instante muestrea cada mic respecto del flanco de WS — si cada mic toma su muestra al inicio de **su** mitad, el mic R muestrea T/2 = 31,25 µs después que el L y aparecería ~0,5 muestra "adelantado" en el índice (exactamente el síntoma); (b) group delay del decimador. Esto es la hipótesis más fuerte y no está verificada.

## 2. Issues / hilos conocidos

### Takeaway
No encontré ningún issue de esp-idf que reporte un corrimiento de una muestra (o fraccional) constante entre L y R en modo STD en el S3. Sí hay issues de swaps de slot entero, inserciones de 4 bytes y quirks de orden en el ESP32 original, que producen síntomas distintos (salto de canal entero o corrupción), no un desfase sub-muestra estable.

### Cited Findings
- esp-idf #15275 "I2s read is flaky" (abierto, v5.5-dev, ESP32 y S3, enlace ESP↔ESP): en ESP32 master con MSB 16-bit aparecen **inserciones de 4 bytes** (0x00/0x80) que corren todo el stream; en Philips 16-bit, 4 bytes en cero reemplazan datos; en PCM entre ESP32 y S3 "looks like left and right are swapped". En S3↔S3 Philips/MSB 16-bit a 10 kHz master RX: "No issues"; Philips 32-bit a 1 kHz: "Frequent random data", a 10 kHz "No issues" — [#15275](https://github.com/espressif/esp-idf/issues/15275)
- esp-idf #10630 (S3, TDM PCM 16-bit mono slave): aparece una muestra 0/-1 cada dos, i.e. el slot inactivo se cuela en el buffer — [#10630](https://github.com/espressif/esp-idf/issues/10630)
- esp-idf #3686 "I2S ADC channel order is unexpected/doubled up" (ESP32 original, cerrado) — [#3686](https://github.com/espressif/esp-idf/issues/3686) (no leído en detalle)
- esp-idf #7192 y #14415: problemas con SPH0645 (formato/alineación, no desfase L/R) — [#7192](https://github.com/espressif/esp-idf/issues/7192), [#14415](https://github.com/espressif/esp-idf/issues/14415) (solo títulos)
- esp-idf #19305 (ESP32 clásico, full-duplex): el ISR compartido RX/TX limpia el EOF del otro sentido → saltos de descriptores TX; aplica a DMA dedicado (no GDMA). El S3 usa GDMA, no le aplica según el reporte — [#19305](https://github.com/espressif/esp-idf/issues/19305)
- esp-idf #15776 (feature request): no hay forma de arrancar RX y TX sincronizados; el offset RX/TX es desconocido (relevante para AEC, no para L/R) — [#15776](https://github.com/espressif/esp-idf/issues/15776)
- Foro esp32.com: hilo "I2S does not work in STEREO with INMP441" (legacy, `RIGHT_LEFT`): señal útil solo en índices pares y basura en impares — [esp32.com p=123226](https://esp32.com/viewtopic.php?p=123226) (contenido vía buscador; la página pide challenge anti-bot, no se pudo leer completa). Hilo "L&R Microphones on I2S using ESP32 S3 DevKitC-1" — [esp32.com p=116048](https://esp32.com/viewtopic.php?p=116048) (no accesible, mismo motivo).
- Proyectos de TDOA con 2 mics I2S en un bus (SPH0645, L/R a GND/VDD) estiman el retardo por correlación sin mencionar corrección de offset entre slots — [alanfellerv/Audio-Source-Localisationbot](https://github.com/alanfellerv/Audio-Source-Localisationbot); [EkinUgurr/sound-localization-esp32](https://github.com/EkinUgurr/sound-localization-esp32) (resumen del buscador)

### Inferences
- Las fallas reportadas (inserción de 4 bytes, swap) cambiarían de golpe el alineamiento y serían visibles como saltos/ruido; un offset estable y fraccional no encaja con ellas.
- #15275 indica que los casos 32-bit a fs muy baja son frágiles en enlaces ESP↔ESP; a 16 kHz/32-bit master RX con mic no hay reporte de problema.

### Gaps
- Búsqueda GitHub limitada (semántica, mayoría abiertos); no se encontraron issues cerrados tipo "one sample delay between channels". El foro esp32.com bloqueó la lectura directa. Arduino-ESP32 y esp-skainet no se revisaron por límite de tiempo.

## 3. ESP32 original vs ESP32-S3

### Takeaway
El ESP32 (HW v1) tiene registros propios de orden (`rx_right_first`, `rx_msb_right`, `rx_chan_mod`) con comportamiento documentado como confuso (el propio HAL dice que el TRM estaría mal en TX); el S3 (HW v2) elimina eso y usa TDM genérico con `ws_idle_pol`. Los quirks de swap L/R clásicos son del ESP32 original.

### Cited Findings
- HW v1 (`#if SOC_I2S_HW_VERSION_1`, esp32/esp32-s2): `i2s_std_slot_config_t` tiene `msb_right` ("place right channel data at the MSB in the FIFO"); HW v2 tiene `left_align`, `big_endian`, `bit_order_lsb` en su lugar — [i2s_std.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/esp_driver_i2s/include/driver/i2s_std.h)
- En ESP32 los defaults Philips/MSB ponen `msb_right = true` solo si `bits_per_sample <= 16`; `ws_pol` se mapea a `rx_right_first` — [i2s_std.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/esp_driver_i2s/include/driver/i2s_std.h); [i2s_hal.c v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/i2s_hal.c)
- ESP32 `i2s_ll_rx_select_std_slot(hw, mask, is_msb_right)`: "the slot sequence is affected by msb_right according to TRM"; LEFT→`rx_chan_mod = msb_right ? 2 : 1`, RIGHT→`msb_right ? 1 : 2` — [hal/esp32/i2s_ll.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/esp32/include/hal/i2s_ll.h)
- Comentario en el HAL: "According to the test, the behavior of tx_msb_right is opposite with TRM, TRM is wrong?" — [i2s_hal.c v5.4, l.158](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/i2s_hal.c)

### Inferences
- Los consejos de foros "en ESP32 los canales salen invertidos / usar RIGHT_LEFT" vienen del HW v1 + driver legacy y no se trasladan al S3.

### Gaps
- No se revisó el TRM del ESP32 original.

## 4. Cómo probarlo

### Takeaway
La prueba más decisiva es intercambiar los straps L/R de los dos mics sin moverlos: si el adelanto sigue al **slot** (R siempre adelante) es del sistema (WS/slot/muestreo del mic); si sigue al **mic físico**, es acústico/de montaje. Complementar con loopback eléctrico de patrón conocido para descartar el driver.

### Cited Findings
- El pin L/R del INMP441 elige la mitad del frame: bajo → canal izquierdo; alto → derecho — [electrodragon INMP441](https://w2.electrodragon.com/Sensor-dat/sensor-microphone-dat/sensor-microphone-I2S-dat/sensor-microphone-I2S-dat.md)
- El S3 permite compartir BCK/WS entre TX y RX (`i2s_ll_share_bck_ws` → `tx_conf.sig_loopback`) y full-duplex en un mismo controlador — [hal/esp32s3/i2s_ll.h v5.4](https://github.com/espressif/esp-idf/blob/release/v5.4/components/hal/esp32s3/include/hal/i2s_ll.h)

### Inferences (procedimientos propuestos, no tomados de una fuente)
1. **Swap de straps:** mic A L/R=GND, mic B L/R=VDD → medir; invertir straps → medir. Comparar el signo del retardo.
2. **Loopback de patrón:** con mics desconectados, TX (otro puerto o full-duplex compartiendo BCK/WS) manda estéreo con L=contador k, R=k|0x8000_0000 (32-bit). Leer RX y verificar que cada par `[i, i+1]` tenga el mismo k. Si R trae k-1 o k+1 → pairing corrido en el driver.
3. **Palabras crudas:** loguear los primeros N `int32` del primer `i2s_channel_read` sin conversión; con un solo mic conectado verificar que el slot vacío (SD en alta impedancia / pull-down) quede en índices impares o pares según el strap.
4. **Analizador lógico** en BCLK/WS/SD: confirmar que el mic L habla con WS bajo, Philips (1 BCK de corrimiento) y que `ws_inv` no está activo.
5. **Estimador:** usar GCC-PHAT con interpolación parabólica y una fuente broadside (equidistante); un offset ≈0,50 muestra estable apunta a medio frame (T/2); ≈1,00 a un slot/frame entero.
