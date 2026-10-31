# Pinout del prototipo (ESP32-S3 N16R8)

Un solo lugar con todos los pines y dónde están en el código. Es la fuente de verdad de los pines y sus reglas (`CLAUDE.md` y `AGENTS.md` ya no los listan): al cambiar un pin, actualizar solo esta nota. Estado: **En uso** (probado) · **Propuesto** (elegido, sin probar en la placa).

## Asignados

| GPIO | Pad WROOM-1 | Señal | Estado | Definido en | Va a |
|---|---|---|---|---|---|
| 4 | 4 | I2S mics: BCLK | En uso | `ORBITA_I2S_BCLK_GPIO`, `audio_capture.h` | BCLK de ambos mics |
| 5 | 5 | I2S mics: WS | En uso | `ORBITA_I2S_WS_GPIO`, `audio_capture.h` | WS de ambos mics |
| 6 | 6 | I2S mics: DIN | En uso | `ORBITA_I2S_DIN_GPIO`, `audio_capture.h` | DOUT de ambos mics (cada uno en su slot con el pin `L/R`) |
| 15 | 8 | I2S parlante: BCLK | En uso (PCB, 16/10) | `ORBITA_PLAY_BCLK_GPIO`, `orbita_playback.h` | BCLK del MAX98357A |
| 7 | 7 | I2S parlante: WS | En uso (PCB, 16/10) | `ORBITA_PLAY_WS_GPIO`, `orbita_playback.h` | LRC del MAX98357A |
| 16 | 9 | I2S parlante: DOUT | En uso (PCB, 16/10) | `ORBITA_PLAY_DOUT_GPIO`, `orbita_playback.h` | DIN del MAX98357A |
| 17 | 10 | LED aro WS2812: DIN | Propuesto | — | DIN del primer LED (330 Ω en serie; DO del último al aire) |
| 19 / 20 | 13 / 14 | USB-Serial-JTAG (DM / DP) | Reservado | — | Comandos y datos (`tools/orbita_serial.py`) |
| 43 / 44 | 37 / 36 | UART0 (TX / RX) | Reservado | — | Logs (`idf.py monitor`) |

Controladores I2S: mics en `I2S_NUM_0`, parlante en `I2S_NUM_1` (el S3 tiene 2). Si se pasa a mics PDM, tienen que seguir en `I2S_NUM_0`: el driver solo acepta PDM ahí (`i2s_pdm.c`, IDF v5.5.5). Pads según el datasheet WROOM-1 v1.3, Tabla 3; strapping: GPIO0 = pad 27, 3 = 15, 45 = 26, 46 = 16.

## No usar

| GPIO | Por qué | Verificado en |
|---|---|---|
| 26–32 | Flash (bus MSPI: CS1, HD, WP, CS0, CLK, MISO, MOSI) | `soc/esp32s3/include/soc/spi_pins.h` |
| 33–37 | PSRAM octal (D4–D7, DQS). En el WROOM-1 los pads de 35–37 existen igual: dejarlos sin conectar | `spi_pins.h`; WROOM-1 Tabla 3, nota b |
| 22–25 | No existen en el ESP32-S3 | `SOC_GPIO_VALID_GPIO_MASK`, `soc_caps.h` |
| 0, 3, 45, 46 | Strapping: 0 y 46 eligen el modo de arranque, 45 la tensión de VDD_SPI | ESP HW Design Guidelines (S3), Schematic Checklist: Strapping Pins |

## Sin pin todavía

NFC (interfaz sin definir: SPI / I2C / UART), botón de mute, microSD (SPI), botones reset/boot, `SD_MODE` del MAX98357A (GPIO directo, sin pull-up al reset; ver `orbita-pcb/orbita_pcb_ruteo.md` §1.8).

## Notas

- **GPIO 15 y 16 son XTAL_32K** (y CTS/RTS por defecto de la UART0; verificado en `uart_pins.h`). Sirven como GPIO mientras no haya cristal de 32 kHz ni control de flujo por hardware en la UART0. Si la PCB lleva ese cristal: usar 7, 8 y 9 para el parlante.
- MAX98357A: **alimentación a 5 V externos** (desde el 3V3 del DevKit no sonaba, 15/10; causa sin medir). Datasheet: VDD 2,5–5,5 V, acepta LRCLK de 16 kHz y datos de 16 bit; `SD_MODE` en bajo = apagado, `GAIN_SLOT` elige la ganancia (Tablas 5 y 8). Según la hoja, 3,3 V tendría que andar: la falla del 15/10 seguramente fue el 3V3 del DevKit o el cableado.
- LED aro (WS2812, 5 V): el "1" pide ~0,7·VDD ≈ 3,5 V y el S3 da 3,3 V → buffer de nivel en la PCB (ej. 74AHCT1G125). Sin verificar contra el datasheet del LED. Evitar 38/48 (LED del DevKit) y 39–42 (JTAG).
- Pines del DevKit (LED RGB de la placa, botones) no cuentan: no van a existir en la PCB.
