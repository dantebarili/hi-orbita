#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_capture.h"
#include "pipeline.h"
#include "test.h"

// Falla de arranque: en laboratorio el equipo NO se reinicia (un reinicio en bucle tapa el
// motivo). Queda parado repitiendo el error, para leerlo en el monitor. En producto esto
// cambia: ahi si corresponde reiniciar (ver A10 del plan).
static void arranque_fallo(const char *que, esp_err_t err)
{
    while (1) {
        ESP_LOGE("main", "ARRANQUE DETENIDO: %s fallo con %s", que, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    // Drivers y rings del modulo de laboratorio (se borra cuando exista el sink de sockets).
    test_init();

    // --- Audio: I2S y pipeline de captura ---
    esp_err_t err = orbita_audio_i2s_init();
    if (err != ESP_OK) {
        arranque_fallo("orbita_audio_i2s_init", err);
    }

    err = orbita_pipeline_start(test_pipeline_cfg());     // Se carga el callback del test (test_on_crudo) en la configuracion
    if (err != ESP_OK) {
        arranque_fallo("orbita_pipeline_start", err);
    }

    // Loop de comandos de laboratorio ('g' graba una toma, 's' pide el resumen). No vuelve.
    test_loop();
}
