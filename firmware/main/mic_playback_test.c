// Prueba local de los dos I2S: captura cruda continua + tonos. No inicia red ni AFE.
#include "audio_capture.h"
#include "orbita_playback.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define MIC_TEST_FRAMES 1024
#define MIC_TEST_PERIOD_MS 5000
static const char *TAG = "mics_parlante";
// Un solo bloque reutilizado; el audio se descarta despues de medir sus picos.
static int32_t raw[MIC_TEST_FRAMES * 2];
static int16_t tone[800];
static const int16_t silence[800];
static QueueHandle_t events;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    uint32_t frames, errors, peak_l, peak_r;
} capture_stats_t;
static capture_stats_t stats;

typedef struct {
    orbita_play_event_t event;
    uint32_t id;
} playback_event_t;

static void capture_discard_task(void *ctx)
{
    (void)ctx;
    for (;;) {
        size_t frames = 0;
        esp_err_t err = orbita_audio_i2s_read(raw, MIC_TEST_FRAMES, &frames);
        uint32_t peak_l = 0, peak_r = 0;
        if (err == ESP_OK && frames <= MIC_TEST_FRAMES) {
            // Mismo paso de 32 a 16 bit que usa el pipeline; aqui solo se mide.
            for (size_t i = 0; i < frames; ++i) {
                int32_t l = raw[2 * i] >> 16;
                int32_t r = raw[2 * i + 1] >> 16;
                uint32_t al = l < 0 ? (uint32_t)-l : (uint32_t)l;
                uint32_t ar = r < 0 ? (uint32_t)-r : (uint32_t)r;
                if (al > peak_l) peak_l = al;
                if (ar > peak_r) peak_r = ar;
            }
        }
        bool failed = err != ESP_OK || frames != MIC_TEST_FRAMES;
        portENTER_CRITICAL(&stats_lock);
        if (failed) stats.errors++;
        else stats.frames += (uint32_t)frames;
        if (peak_l > stats.peak_l) stats.peak_l = peak_l;
        if (peak_r > stats.peak_r) stats.peak_r = peak_r;
        portEXIT_CRITICAL(&stats_lock);
        // No se conserva ni se envia raw; la proxima lectura lo reemplaza.
        if (failed) vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Corre en playback: copiar el evento y salir, sin logs ni espera.
static void on_playback(orbita_play_event_t event, uint32_t id, void *ctx)
{
    (void)ctx;
    const playback_event_t item = {.event = event, .id = id};
    xQueueSend(events, &item, 0);
}

static esp_err_t three_beeps(uint32_t id)
{
    esp_err_t err = orbita_playback_begin(id);
    if (err != ESP_OK) return err;
    // 250 ms de tono, 250 ms de silencio, repetido tres veces (sin silencio final).
    for (unsigned part = 0; part < 5; ++part) {
        const int16_t *data = part % 2 == 0 ? tone : silence;
        for (unsigned chunk = 0; chunk < 5; ++chunk) {
            err = orbita_playback_push((const uint8_t *)data, sizeof(tone));
            if (err != ESP_OK) {
                (void)orbita_playback_stop();
                return err;
            }
        }
    }
    err = orbita_playback_end();
    if (err != ESP_OK) (void)orbita_playback_stop();
    return err;
}

esp_err_t mic_playback_test_run(void)
{
    static const int16_t cycle[16] = {
        0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
        0, -6123, -11314, -14782, -16000, -14782, -11314, -6123,
    };
    _Static_assert(sizeof(tone) <= 4096, "push admite hasta 4 KB");
    for (unsigned i = 0; i < 800; ++i) tone[i] = cycle[i % 16];
    events = xQueueCreate(8, sizeof(playback_event_t));
    if (events == NULL) return ESP_ERR_NO_MEM;

    // Mismo orden de inicializacion que el integrado: entrada y despues salida.
    esp_err_t err = orbita_audio_i2s_init();
    if (err != ESP_OK) return err;
    const orbita_playback_cfg_t cfg = {
        .gpio_bclk = ORBITA_PLAY_BCLK_GPIO,
        .gpio_ws = ORBITA_PLAY_WS_GPIO,
        .gpio_dout = ORBITA_PLAY_DOUT_GPIO,
        .prebuffer_ms = 400,
        .cb = on_playback,
    };
    err = orbita_playback_init(&cfg);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(capture_discard_task, "mic_test", 4096, NULL,
                               6, NULL, 1) != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGW(TAG, "CAPTURA ACTIVA + PARLANTE; sin Wi-Fi, WebSocket, pipeline ni AFE");
    ESP_LOGI(TAG, "Buffer crudo fijo=%u B; audio leido y descartado continuamente",
             (unsigned)sizeof(raw));
    TickType_t last_start = xTaskGetTickCount();
    for (uint32_t id = 1; ; ++id) {
        ESP_LOGI(TAG, "Tres pitidos LOCALES id=%lu (inicio cada 5 s)", (unsigned long)id);
        err = three_beeps(id);
        if (err != ESP_OK) return err;
        TickType_t start = xTaskGetTickCount();
        bool done = false;
        while (xTaskGetTickCount() - start < pdMS_TO_TICKS(4000)) {
            playback_event_t item;
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
        capture_stats_t snapshot;
        portENTER_CRITICAL(&stats_lock);
        snapshot = stats;
        stats.peak_l = stats.peak_r = 0;
        portEXIT_CRITICAL(&stats_lock);
        ESP_LOGI(TAG, "MIC frames=%lu errores=%lu dma=%lu picoL=%lu picoR=%lu RAM_int=%u PSRAM=%u",
                 (unsigned long)snapshot.frames, (unsigned long)snapshot.errors,
                 (unsigned long)orbita_audio_get_dma_overflows(),
                 (unsigned long)snapshot.peak_l, (unsigned long)snapshot.peak_r,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        vTaskDelayUntil(&last_start, pdMS_TO_TICKS(MIC_TEST_PERIOD_MS));
    }
}
