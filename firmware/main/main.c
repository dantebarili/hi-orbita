#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_capture.h"

static const char *TAG = "orbita_main";

#define TEST_FRAME_COUNT 512

// Buffer estatico en RAM interna (no PSRAM todavia) para guardar las muestras que integran los frames 
// solicitados: 512 frames * 2 canales * 32 bits (int32_t) = 4KB.
static int32_t s_raw_buf[TEST_FRAME_COUNT * 2];

// Calcula RMS de un canal a partir del buffer entrelazado L/R.
// channel: 0 = izquierdo, 1 = derecho.
static double do_rms(const int32_t *data_buf, size_t frame_count, int channel)
{
    double sum_rms = 0.0;
    for (size_t i = 0; i < frame_count; i++) {
        // Se descartan los 8 bits menos significativos (relleno/basura del
        // slot de 32 bits; el mic solo entrega 24 bits utiles alineados a la
        // izquierda). El shift preserva el signo porque sample es int32_t.
        int32_t sample = data_buf[i * 2 + channel] >> 8;
        // Cast a double ANTES de multiplicar: sample al cuadrado puede superar
        // el rango de int32_t (overflow), asi que la multiplicacion tiene que
        // hacerse ya en double, no despues de sumarla.
        sum_rms += (double)sample * (double)sample;
    }

    return sqrt(sum_rms / frame_count);
}

void app_main(void)
{
    ESP_ERROR_CHECK(orbita_audio_i2s_init());

    ESP_LOGI(TAG, "Iniciando lectura de prueba (RMS por canal cada bloque)");

    while (1) {
      
        size_t frames_read = 0; // Cantidad de frames leidos y guardados realmente
        
      // Se guardan los ultimos TEST_FRAME_COUNT (idealmente) frames en s_raw_buf
      esp_err_t err = orbita_audio_i2s_read(s_raw_buf, TEST_FRAME_COUNT, &frames_read);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        } // Revisar sintaxis y que es esto

        double rms_l = do_rms(s_raw_buf, frames_read, 0);
        double rms_r = do_rms(s_raw_buf, frames_read, 1);

        ESP_LOGI(TAG, "frames=%d  RMS_L=%.1f  RMS_R=%.1f", (int)frames_read, rms_l, rms_r);
    }
}
