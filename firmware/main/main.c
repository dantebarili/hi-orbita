#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_capture.h"
#include "audio_sink.h"
#include "pipeline.h"
#include "sdkconfig.h"
#include "orbita_wifi.h"
#include "orbita_ws.h"
#include "audio_ws_sink.h"
#include "audio_ws_playback.h"
#include "audio_ws_conversacion.h"
#include "audio_ws_led.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "wakeword.h"

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

// Medir en placa: el ring va a PSRAM, pero DMA y pilas consumen RAM interna.
static void log_memoria(const char *etapa)
{
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI("main", "RAM %s: interna_libre=%u bloque_max=%u psram_libre=%u",
             etapa, (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void app_main(void)
{
    // S2: la captura va PRIMERO y no espera a la red. Wi-Fi y WebSocket se levantan despues y
    // se reconectan solos (Wi-Fi cada 5 s, WebSocket con backoff 1-2-4-8 s); mientras tanto el
    // sink retiene el audio en el ring (S1).
    if (!CONFIG_ORBITA_WIFI_SSID[0] || !CONFIG_ORBITA_WS_URI[0])
        arranque_fallo("Configurar Wi-Fi y URI en menuconfig -> Orbita WiFi", ESP_ERR_INVALID_ARG);
    esp_err_t err;
    // La wake word va primero: su arena (~50 KB) se reserva en RAM interna antes que el resto.
    log_memoria("antes de wakeword");
    // Cada deteccion abre una conversacion: recien ahi se manda audio al servidor (pre-roll incluido).
    err = orbita_wakeword_start(audio_ws_conversacion_on_wake);
    if (err != ESP_OK) arranque_fallo("wakeword", err);
    // Aro LED despues de la wake word (su arena va primero en RAM interna). Si falla, sigue sin indicador.
    (void)audio_ws_led_init();
    err = orbita_audio_i2s_init();
    if (err != ESP_OK) arranque_fallo("I2S", err);
    // El ring del parlante (PSRAM) se reserva antes de que el pipeline asigne sus buffers.
    log_memoria("antes de playback");
    err = audio_ws_playback_init();
    if (err != ESP_OK) arranque_fallo("playback", err);
    log_memoria("despues de playback");
    // No enviar desde on_crudo. La wake word recibe el mic de adelante (omnidireccional): su on_crudo solo
    // lo copia a su ring y vuelve. Lo que va al servidor sale del beamformer (pipeline, tarea feed).
    const orbita_pipeline_cfg_t pipe_cfg = { .on_crudo = orbita_wakeword_on_crudo };
    err = orbita_pipeline_start(&pipe_cfg);
    if (err != ESP_OK) arranque_fallo("pipeline", err);
    log_memoria("despues de pipeline");
    // Unico consumidor del ring out: arma los mensajes y los manda por el WebSocket, pero solo durante
    // una conversacion (compuerta cerrada hasta la wake word; el ring guarda los ultimos ~32 s).
    err = audio_ws_conversacion_init();
    if (err != ESP_OK) arranque_fallo("conversacion", err);
    err = orbita_sink_start(orbita_audio_ws_sink());
    if (err != ESP_OK) arranque_fallo("sink WebSocket", err);
    err = nvs_flash_init();
    if (err != ESP_OK) arranque_fallo("NVS", err);
    err = esp_netif_init();
    if (err != ESP_OK) arranque_fallo("esp_netif", err);
    err = esp_event_loop_create_default();
    if (err != ESP_OK) arranque_fallo("event loop", err);
    err = orbita_wifi_start(CONFIG_ORBITA_WIFI_SSID, CONFIG_ORBITA_WIFI_PASSWORD);
    if (err != ESP_OK) arranque_fallo("Wi-Fi", err);
    // Sin esperar IP: el cliente WebSocket reintenta solo hasta que haya red y servidor.
    // on_downlink: lo que manda el servidor (play, PLAY, play_end, play_stop...) va al parlante.
    const orbita_ws_config_t ws = {
        .uri = CONFIG_ORBITA_WS_URI,
        .on_downlink = audio_ws_playback_on_downlink,
    };
    err = orbita_ws_start(&ws);
    if (err != ESP_OK) arranque_fallo("WebSocket", err);
    log_memoria("arranque completo");
    ESP_LOGI("main", "Listo: decir \"Orbita\" para empezar a mandar audio al servidor");
}
