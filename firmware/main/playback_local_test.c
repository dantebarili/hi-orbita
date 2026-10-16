// Diagnostico independiente del servidor: solo salida I2S y parlante.
#include "orbita_playback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "tono_local";
static QueueHandle_t events;
static int16_t chunk[800];

typedef struct {
    orbita_play_event_t event;
    uint32_t id;
} local_event_t;

// Corre en playback: solo copia el evento. El log y la espera se hacen en main.
static void on_event(orbita_play_event_t event, uint32_t id, void *ctx)
{
    (void)ctx;
    const local_event_t item = {.event = event, .id = id};
    xQueueSend(events, &item, 0);
}

esp_err_t playback_local_test_run(void)
{
    static const int16_t cycle[16] = {
        0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
        0, -6123, -11314, -14782, -16000, -14782, -11314, -6123,
    };
    _Static_assert(sizeof(chunk) <= 4096, "push admite hasta 4 KB");
    for (unsigned i = 0; i < 800; ++i) chunk[i] = cycle[i % 16];
    events = xQueueCreate(8, sizeof(local_event_t));
    if (events == NULL) return ESP_ERR_NO_MEM;
    const orbita_playback_cfg_t cfg = {
        .gpio_bclk = ORBITA_PLAY_BCLK_GPIO,
        .gpio_ws = ORBITA_PLAY_WS_GPIO,
        .gpio_dout = ORBITA_PLAY_DOUT_GPIO,
        .prebuffer_ms = 400,
        .cb = on_event,
    };
    esp_err_t err = orbita_playback_init(&cfg);
    if (err != ESP_OK) return err;
    ESP_LOGW(TAG, "SOLO PARLANTE: sin microfonos, AFE, Wi-Fi ni WebSocket");
    ESP_LOGI(TAG, "BCLK=%d WS=%d DOUT=%d; ganancia Q15=%d",
             cfg.gpio_bclk, cfg.gpio_ws, cfg.gpio_dout, ORBITA_PLAY_GAIN_Q15);
    for (uint32_t id = 1; ; ++id) {
        ESP_LOGI(TAG, "Tono local id=%lu: 1 kHz durante 1 s", (unsigned long)id);
        err = orbita_playback_begin(id);
        if (err != ESP_OK) return err;
        for (unsigned i = 0; i < 20; ++i) {
            err = orbita_playback_push((const uint8_t *)chunk, sizeof(chunk));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "push %u: %s", i, esp_err_to_name(err));
                (void)orbita_playback_stop();
                return err;
            }
        }
        err = orbita_playback_end();
        if (err != ESP_OK) return err;
        const TickType_t start = xTaskGetTickCount();
        bool done = false;
        while (xTaskGetTickCount() - start < pdMS_TO_TICKS(5000)) {
            local_event_t item;
            if (xQueueReceive(events, &item, pdMS_TO_TICKS(100)) != pdTRUE) continue;
            if (item.id != id) continue;
            ESP_LOGI(TAG, "id=%lu evento=%s", (unsigned long)id,
                     item.event == ORBITA_PLAY_EV_STARTED ? "STARTED" :
                     item.event == ORBITA_PLAY_EV_DONE ? "DONE" :
                     item.event == ORBITA_PLAY_EV_STOPPED ? "STOPPED" : "UNDERRUN");
            if (item.event == ORBITA_PLAY_EV_DONE) { done = true; break; }
            if (item.event == ORBITA_PLAY_EV_STOPPED) return ESP_FAIL;
        }
        if (!done) {
            (void)orbita_playback_stop();
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "Fin; silencio 3 s. Pila main libre=%u B",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
