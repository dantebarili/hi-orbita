/**
 * @file    audio_capture.c
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Órbita
 * @brief   Inicializo el canal I2S para leer los mics INMP441/ICS-43434 y proveo una funcion para leer N frames de audio.
 * @version 1.0.0
 * @date    2026-09-03
 * 
 * @copyright Copyright (c) 2026 - Proyecto Órbita
 * 
 * @details Buffer DMA del I2S: 6 descriptores de 240 frames cada uno = 1440 frames (~90ms @16kHz). Ese es el margen maximo 
 *          entre llamadas a orbita_audio_i2s_read() antes de arriesgar overrun (perder muestras).
 *          
 *          
 */

#include "audio_capture.h"
#include "freertos/FreeRTOS.h"
#if __has_include("driver/i2s_std.h")
#include "driver/i2s_std.h"
#elif __has_include("esp_driver_i2s/i2s_std.h")
#include "esp_driver_i2s/i2s_std.h"
#else
#error "I2S standard-mode header not found; install/configure the ESP-IDF I2S driver component"
#endif
#include "esp_log.h"

static const char *TAG = "orbita_audio";
static i2s_chan_handle_t s_rx_chan = NULL; // Handle para identidicar el canal a utilizar

esp_err_t orbita_audio_i2s_init(void)
{
    /*  - Se crea la configuracion del canal con la ESP como el Master
    *   - Buffer interno DMA del I2S: dma_desc_num(6) * dma_frame_num(240) = 1440
    *      frames (~90ms @16kHz). Ese es el margen maximo entre llamadas a
    *      orbita_audio_i2s_read() antes de arriesgar overrun (perder muestras).
    */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

  
    // Creamos el canal con la config anterior creada y en modo RX
    // Y lo guardamos en s_rx_chan
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel fallo: %s", esp_err_to_name(err));
        return err;
    }

    // Configuracion del formato de audio
  
    i2s_std_config_t std_cfg = {
        // Reloj: fija el sample rate a 16 kHz. Este macro calcula solo los
        // divisores de reloj internos necesarios para llegar a esa frecuencia.
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ORBITA_SAMPLE_RATE_HZ),

        // Formato de cada slot (canal) dentro del frame I2S:
        // - 32 bits de ancho: aunque el dato util del mic son 24 bits, el
        //   protocolo reserva un contenedor de 32 bits por slot (los 8 bits
        //   bajos quedan en cero/relleno). Hay que leer en 32 bits para que
        //   los datos queden alineados correctamente.
        // - STEREO: 2 slots por frame (L y R), uno por cada mic. Los 2 mics
        //   comparten la misma linea fisica de datos (DIN) y el mismo reloj
        //   (BCLK/WS); cada uno "habla" solo en su slot, segun tenga su pin
        //   L/R del mic atado a GND (izquierdo) o a VDD (derecho).
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),

        // Mapeo de las señales logicas del I2S a los GPIO fisicos del ESP32-S3.
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,       // No usamos master clock separado.
            .bclk = ORBITA_I2S_BCLK_GPIO,  // Reloj de bit, generado por el ESP32 (master).
            .ws   = ORBITA_I2S_WS_GPIO,    // Word select: indica de que canal (L/R) es cada bit.
            .dout = I2S_GPIO_UNUSED,       // No transmitimos nada, solo leemos (RX).
            .din  = ORBITA_I2S_DIN_GPIO,   // Linea de datos entrante, compartida por ambos mics.
            .invert_flags = {
                // Ninguna señal necesita invertirse: es la polaridad estandar
                // que esperan los mics INMP441/ICS-43434.
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
  
// Aplico la config
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode fallo: %s", esp_err_to_name(err));
        return err;
    }
  
// Prendo el canal con toda la configuracion anterioremente creada
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable fallo: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S inicializado: %d Hz, estereo, 32 bits/slot", ORBITA_SAMPLE_RATE_HZ);
    return ESP_OK; // Return OK final si el canal I2S fue correctamente creado
}

esp_err_t orbita_audio_i2s_read(int32_t *out_buf, size_t frame_count, size_t *frames_read)
{
    size_t bytes_to_read = frame_count * 2 * sizeof(int32_t); // bytes totales a leer, 2 canales * frame (L/R)
    size_t bytes_read = 0; // Inicializamos en 0 por las dudas

    /*
    * Se lee el canal, se guardan N bytes_to_read en out_buf, te carga los M bytes leidos en bytes_read 
    * Se le indica al FreeRTOS, mediante portMAX_DELAY que no se active la accion hasta que haya un evento 
        es decir que se tengan en buffer al menos N bytes_to_read. 
    * La LATENCIA DE LECTURA de esta accion es de frames_count / sample_frec
    */
    esp_err_t err = i2s_channel_read(s_rx_chan, out_buf, bytes_to_read, &bytes_read, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_read fallo: %s", esp_err_to_name(err));
        return err;
    }

    if (frames_read) {
        *frames_read = bytes_read / (2 * sizeof(int32_t)); // devuelvo la cantidad de FRAMES leidos
    }
    return ESP_OK;
}
