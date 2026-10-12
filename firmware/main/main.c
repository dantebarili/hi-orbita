#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_capture.h"
#include "pipeline.h"
#include "test.h"

void app_main(void)
{
    // Drivers y rings del modulo de laboratorio (se borra cuando exista el sink de sockets).
    test_init();

    // --- Audio: I2S y pipeline de captura ---
    // Cualquier falla de arranque reinicia el equipo.
    ESP_ERROR_CHECK(orbita_audio_i2s_init());
    esp_err_t err = orbita_pipeline_start(test_pipeline_cfg());     // Se carga el callback del test (test_on_crudo) en la configuracion
    if (err != ESP_OK) {
        // Con el driver de UART instalado los logs quedan en un buffer: si se reinicia enseguida
        // se pierden. La espera deja que salgan antes del reinicio de ESP_ERROR_CHECK.
        ESP_LOGE("main", "orbita_pipeline_start fallo: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(err);
    }

    // Loop de comandos de laboratorio ('g' graba una toma, 's' pide el resumen). No vuelve.
    test_loop();
}
