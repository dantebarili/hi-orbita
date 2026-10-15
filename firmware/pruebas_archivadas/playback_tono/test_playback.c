/* PRUEBA PARLANTE (borrar cuando funcione). Ver test_playback.h. */

#include "test_playback.h"
#include "orbita_playback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "test_play";

#define PREBUFFER_MS      400
#define TONO_PERIODO       16                       // 1 kHz a 16 kHz = 16 muestras por ciclo
#define TROZO_MUESTRAS    800                       // 50 ciclos exactos: el tono no se corta entre pushes
#define TROZO_BYTES       (TROZO_MUESTRAS * 2)      // 1600 B (<= 4 KB que acepta push)
#define TROZOS_3S          60                       // 60 * 50 ms = 3 s
#define TROZOS_PREBUFFER    8                       // 8 * 1600 B = 12800 B = 400 ms: justo el prebuffer

// VOLUMEN: la tabla esta a 16000 (-6 dBFS) y orbita_playback le aplica su tope (ORBITA_PLAY_GAIN_Q15 = 0,25):
// sale a 4000 (-18 dBFS), el nivel que ya se probo en el banco. VOLUMEN_DIV queda en 1 (sin atenuar de mas);
// subir el volumen real = subir ORBITA_PLAY_GAIN_Q15 en orbita_playback.h, de a poco.
#define VOLUMEN_DIV 1

// Un ciclo de seno de amplitud 16000 (sin dividir).
static const int16_t CICLO[TONO_PERIODO] = {
    0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
    0, -6123, -11314, -14782, -16000, -14782, -11314, -6123,
};

static int16_t trozo_tono[TROZO_MUESTRAS];
static QueueHandle_t cola_ev;                       // el callback solo encola: loguea la tarea principal

typedef struct {
    orbita_play_event_t ev;
    uint32_t            id;
} evento_t;

// Corre en la tarea de reproduccion: rapido y sin bloquear (xQueueSend con espera 0).
static void on_evento(orbita_play_event_t ev, uint32_t id, void *ctx)
{
    (void)ctx;
    evento_t e = { .ev = ev, .id = id };
    xQueueSend(cola_ev, &e, 0);
}

static const char *nombre(orbita_play_event_t ev)
{
    switch (ev) {
    case ORBITA_PLAY_EV_STARTED:  return "STARTED";
    case ORBITA_PLAY_EV_DONE:     return "DONE";
    case ORBITA_PLAY_EV_STOPPED:  return "STOPPED";
    case ORBITA_PLAY_EV_UNDERRUN: return "UNDERRUN";
    }
    return "?";
}

// Espera hasta `ms` el evento `buscado` y loguea todos los que lleguen. true si llego.
static bool esperar(orbita_play_event_t buscado, uint32_t ms)
{
    TickType_t fin = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < fin) {
        evento_t e;
        if (xQueueReceive(cola_ev, &e, pdMS_TO_TICKS(50)) == pdTRUE) {
            ESP_LOGI(TAG, "  evento %s (id=%u)", nombre(e.ev), (unsigned)e.id);
            if (e.ev == buscado) {
                return true;
            }
        }
    }
    ESP_LOGW(TAG, "  NO llego el evento %s en %u ms", nombre(buscado), (unsigned)ms);
    return false;
}

// Empuja `n` trozos de tono. false si el ring los rechaza.
static bool empujar(int n)
{
    for (int i = 0; i < n; i++) {
        esp_err_t err = orbita_playback_push((const uint8_t *)trozo_tono, TROZO_BYTES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  push %d fallo: %s", i, esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

static void prueba_tono_completo(void)
{
    ESP_LOGI(TAG, "A) Tono de 3 s completo: esperado STARTED y DONE");
    if (orbita_playback_begin(1) != ESP_OK) { ESP_LOGE(TAG, "  begin rechazado"); return; }
    if (empujar(TROZOS_3S)) {
        orbita_playback_end();
        esperar(ORBITA_PLAY_EV_DONE, 6000);
    } else {
        orbita_playback_stop();
        esperar(ORBITA_PLAY_EV_STOPPED, 2000);
    }
}

static void prueba_stop(void)
{
    ESP_LOGI(TAG, "B) Stop a los 0,5 s: esperado STARTED y STOPPED, el tono corta");
    if (orbita_playback_begin(2) != ESP_OK) { ESP_LOGE(TAG, "  begin rechazado"); return; }
    empujar(TROZOS_3S);
    esperar(ORBITA_PLAY_EV_STARTED, 2000);
    vTaskDelay(pdMS_TO_TICKS(500));
    orbita_playback_stop();
    esperar(ORBITA_PLAY_EV_STOPPED, 2000);
}

static void prueba_underrun(void)
{
    ESP_LOGI(TAG, "C) Underrun: 400 ms, silencio de ~0,5 s, 2,6 s mas: esperado STARTED, UNDERRUN y DONE (corte audible)");
    if (orbita_playback_begin(3) != ESP_OK) { ESP_LOGE(TAG, "  begin rechazado"); return; }
    empujar(TROZOS_PREBUFFER);
    esperar(ORBITA_PLAY_EV_UNDERRUN, 3000);       // el ring se vacia a los 400 ms de sonar
    vTaskDelay(pdMS_TO_TICKS(500));
    empujar(TROZOS_3S - TROZOS_PREBUFFER);
    orbita_playback_end();
    esperar(ORBITA_PLAY_EV_DONE, 6000);
}

esp_err_t test_playback_run(void)
{
    for (int i = 0; i < TROZO_MUESTRAS; i++) {
        trozo_tono[i] = CICLO[i % TONO_PERIODO] / VOLUMEN_DIV;
    }
    cola_ev = xQueueCreate(16, sizeof(evento_t));
    if (cola_ev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const orbita_playback_cfg_t cfg = {
        .gpio_bclk    = ORBITA_PLAY_BCLK_GPIO,
        .gpio_ws      = ORBITA_PLAY_WS_GPIO,
        .gpio_dout    = ORBITA_PLAY_DOUT_GPIO,
        .prebuffer_ms = PREBUFFER_MS,
        .cb           = on_evento,
        .cb_ctx       = NULL,
    };
    esp_err_t err = orbita_playback_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Parlante listo (BCLK=%d WS=%d DOUT=%d). Las 3 pruebas se repiten cada 5 s",
             ORBITA_PLAY_BCLK_GPIO, ORBITA_PLAY_WS_GPIO, ORBITA_PLAY_DOUT_GPIO);

    while (1) {
        prueba_tono_completo();
        vTaskDelay(pdMS_TO_TICKS(2000));
        prueba_stop();
        vTaskDelay(pdMS_TO_TICKS(2000));
        prueba_underrun();
        ESP_LOGI(TAG, "Fin del ciclo; pila libre de main=%u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
