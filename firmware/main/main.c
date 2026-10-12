#include "esp_err.h"
#include "audio_capture.h"
#include "pipeline.h"
#include "test.h"

void app_main(void)
{
    // Drivers y rings del modulo de laboratorio (se borra cuando exista el sink de sockets).
    test_init();

    // Audio: I2S y pipeline de captura. Cualquier falla de arranque reinicia el equipo.
    ESP_ERROR_CHECK(orbita_audio_i2s_init());
    ESP_ERROR_CHECK(orbita_pipeline_start(test_pipeline_cfg()));

    // Loop de comandos de laboratorio ('g' graba una toma, 's' pide el resumen). No vuelve.
    test_loop();
}
