# Pinout del prototipo (ESP32-S3 N16R8)

Un solo lugar con todos los pines y dónde están en el código. Es la fuente de verdad de los pines y sus reglas (`CLAUDE.md` y `AGENTS.md` ya no los listan): al cambiar un pin, actualizar solo esta nota. Estado: **En uso** (probado) · **Propuesto** (elegido, sin probar en la placa).

## Asignados

| GPIO | Señal | Estado | Definido en | Va a |
|---|---|---|---|---|
| 4 | I2S mics: BCLK | En uso | `ORBITA_I2S_BCLK_GPIO`, `audio_capture.h` | BCLK de ambos mics |
| 5 | I2S mics: WS | En uso | `ORBITA_I2S_WS_GPIO`, `audio_capture.h` | WS de ambos mics |
| 6 | I2S mics: DIN | En uso | `ORBITA_I2S_DIN_GPIO`, `audio_capture.h` | DOUT de ambos mics (cada uno en su slot con el pin `L/R`) |
| 15 | I2S parlante: BCLK | En uso (PCB, 16/10) | `ORBITA_PLAY_BCLK_GPIO`, `orbita_playback.h` | BCLK del MAX98357A |
| 7 | I2S parlante: WS | En uso (PCB, 16/10) | `ORBITA_PLAY_WS_GPIO`, `orbita_playback.h` | LRC del MAX98357A |
| 16 | I2S parlante: DOUT | En uso (PCB, 16/10) | `ORBITA_PLAY_DOUT_GPIO`, `orbita_playback.h` | DIN del MAX98357A |
| 19 / 20 | USB-Serial-JTAG (DM / DP) | Reservado | — | Comandos y datos (`tools/orbita_serial.py`) |
| 43 / 44 | UART0 (TX / RX) | Reservado | — | Logs (`idf.py monitor`) |

Controladores I2S: mics en `I2S_NUM_0`, parlante en `I2S_NUM_1` (el S3 tiene 2).

## No usar

| GPIO | Por qué | Verificado en |
|---|---|---|
| 26–32 | Flash (bus MSPI: CS1, HD, WP, CS0, CLK, MISO, MOSI) | `soc/esp32s3/include/soc/spi_pins.h` |
| 33–37 | PSRAM octal (D4–D7, DQS) | `spi_pins.h` |
| 22–25 | No existen en el ESP32-S3 | `SOC_GPIO_VALID_GPIO_MASK`, `soc_caps.h` |
| 0, 3, 45, 46 | Strapping (evitar) | `CLAUDE.md`; **sin verificar en el datasheet** |

## Sin pin todavía

NFC (interfaz sin definir: SPI / I2C / UART), LED indicador, botón de mute, microSD (SPI), botones reset/boot, pines de control del MAX98357A (`SD`, `GAIN`) y programación/debug en la PCB.

## Notas

- **GPIO 15 y 16 son XTAL_32K** (y CTS/RTS por defecto de la UART0; verificado en `uart_pins.h`). Sirven como GPIO mientras no haya cristal de 32 kHz ni control de flujo por hardware en la UART0. Si la PCB lleva ese cristal: usar 7, 8 y 9 para el parlante.
- MAX98357A: **alimentación a 5 V externos** (desde el 3V3 del DevKit no sonaba, 15/10; causa sin medir). Sin verificar contra su datasheet: 16 kHz / 16 bit, función de `SD` y `GAIN`.
- Pines del DevKit (LED RGB de la placa, botones) no cuentan: no van a existir en la PCB.
