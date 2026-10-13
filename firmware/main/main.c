#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_capture.h"
#include "audio_sink.h"
#include "pipeline.h"
#include "test.h"

// Falla de arranque: en laboratorio el equipo NO se reinicia (un reinicio en bucle tapa el
// motivo). Queda parado repitiendo el error, para leerlo en el monitor. En producto esto
// cambia: ahi si corresponde reiniciar (ver A10 del plan).
static void arranque_fallo(const char *que, esp_err_t err)
{
    while (1) {
        ESP_LOGE("main", "ARRANQUE DETENIDO: %s fallo con %s", que, esp_err_to_name(err));
        // Mismo aviso por la ROM: sale aunque los logs normales no se esten viendo.
        esp_rom_printf("ARRANQUE DETENIDO: %s fallo con %s\n", que, esp_err_to_name(err));
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

    // Sink de laboratorio: vacia el ring out y cuenta mensajes (no envia nada). Despues del
    // pipeline, que es quien hace orbita_ring_out_init().
    err = orbita_sink_start(test_sink());
    if (err != ESP_OK) {
        arranque_fallo("orbita_sink_start", err);
    }

    // Loop de comandos de laboratorio ('g' graba una toma, 's' pide el resumen). No vuelve.
    test_loop();
}
