#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_capture.h"
#include "audio_sink.h"
#include "pipeline.h"
#include "sdkconfig.h"
#if CONFIG_ORBITA_LAB_WEBSOCKET
#include "orbita_wifi.h"
#include "orbita_ws.h"
#include "audio_ws_sink.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#else
#include "test.h"
#endif

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
#if CONFIG_ORBITA_LAB_WEBSOCKET
    // Inicializamos red ANTES de encender I2S: no dejar el DMA sin lector
    // durante una espera larga por el router. La captura arranca sin esperar al servidor.
    if (!CONFIG_ORBITA_WIFI_SSID[0] || !CONFIG_ORBITA_WS_URI[0])
        arranque_fallo("Configurar Wi-Fi y URI en Orbita integration", ESP_ERR_INVALID_ARG);
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) arranque_fallo("NVS", err);
    err = esp_netif_init();
    if (err != ESP_OK) arranque_fallo("esp_netif", err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK) arranque_fallo("event loop", err);
    err = orbita_wifi_start(CONFIG_ORBITA_WIFI_SSID, CONFIG_ORBITA_WIFI_PASSWORD);
    if (err != ESP_OK) arranque_fallo("Wi-Fi", err);
    do {
        err = orbita_wifi_wait_for_ip(10000);
        if (err == ESP_ERR_TIMEOUT) ESP_LOGW("main", "Esperando IP antes de iniciar captura");
        else if (err != ESP_OK) arranque_fallo("esperar IP", err);
    } while (err == ESP_ERR_TIMEOUT);
    const orbita_ws_config_t ws = { .uri = CONFIG_ORBITA_WS_URI };
    err = orbita_ws_start(&ws);
    if (err != ESP_OK) arranque_fallo("WebSocket", err);
    err = orbita_audio_i2s_init();
    if (err != ESP_OK) arranque_fallo("I2S", err);
    // NULL: sin copias/callbacks del test UART. No enviar desde on_crudo ni on_afe.
    err = orbita_pipeline_start(NULL);
    if (err != ESP_OK) arranque_fallo("pipeline", err);
    // Único consumidor del ring out. El adaptador usa la tarea sink ya existente.
    err = orbita_sink_start(orbita_audio_ws_sink());
    if (err != ESP_OK) arranque_fallo("sink WebSocket", err);
    ESP_LOGI("main", "Audio WebSocket activo; fin/errores se informan en audio_ws");
#else
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
#endif
}
