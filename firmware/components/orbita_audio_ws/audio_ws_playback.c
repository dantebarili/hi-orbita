#include "sdkconfig.h"
#if CONFIG_ORBITA_LAB_WEBSOCKET

#include "audio_ws_playback.h"
#include "audio_ws_wire.h"
#include "orbita_playback.h"
#include "orbita_ws.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <limits.h>
#include <inttypes.h>

#define PLAY_MAP_CAPACITY 4
#define PLAY_PREBUFFER_MS 400

typedef struct {
    bool used;
    bool suppress_terminal_report;
    bool started;
    bool had_underrun;
    uint32_t token;
    uint32_t connection_id;
    uint64_t server_play_id;
    int64_t started_us;
} play_map_t;

static const char *TAG = "audio_ws_playback";
static portMUX_TYPE s_map_lock = portMUX_INITIALIZER_UNLOCKED;
static play_map_t s_play_map[PLAY_MAP_CAPACITY];
static uint32_t s_next_token = 1;
// Solo lo modifica el callback downlink serializado; nunca el callback de playback.
// CRC sobre PCM recibido antes de la copia al ring y de la ganancia del parlante.
static uint32_t s_rx_crc, s_rx_bytes;

static play_map_t *find_token_locked(uint32_t token)
{
    for (size_t i = 0; i < PLAY_MAP_CAPACITY; ++i) {
        if (s_play_map[i].used && s_play_map[i].token == token) return &s_play_map[i];
    }
    return NULL;
}

static play_map_t *find_server_id_locked(uint32_t connection_id, uint64_t server_id)
{
    for (size_t i = 0; i < PLAY_MAP_CAPACITY; ++i) {
        if (s_play_map[i].used && s_play_map[i].connection_id == connection_id &&
            s_play_map[i].server_play_id == server_id) return &s_play_map[i];
    }
    return NULL;
}

// El modulo playback usa un token uint32; el protocolo del servidor usa un id uint64.
// Guardamos la asociacion hasta recibir DONE/STOPPED, incluso si ya empezo otro PLAY.
static bool reserve_play(uint32_t connection_id, uint64_t server_id, uint32_t *token_out)
{
    bool reserved = false;
    portENTER_CRITICAL(&s_map_lock);
    play_map_t *slot = NULL;
    for (size_t i = 0; i < PLAY_MAP_CAPACITY; ++i) {
        if (!s_play_map[i].used) { slot = &s_play_map[i]; break; }
    }
    if (slot != NULL) {
        for (unsigned attempt = 0; attempt <= PLAY_MAP_CAPACITY; ++attempt) {
            uint32_t candidate = s_next_token++;
            if (candidate == 0) candidate = s_next_token++;
            if (candidate != 0 && find_token_locked(candidate) == NULL) {
                *slot = (play_map_t){
                    .used = true,
                    .token = candidate,
                    .connection_id = connection_id,
                    .server_play_id = server_id,
                };
                *token_out = candidate;
                reserved = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_map_lock);
    return reserved;
}

static void remove_token(uint32_t token)
{
    portENTER_CRITICAL(&s_map_lock);
    play_map_t *entry = find_token_locked(token);
    if (entry != NULL) *entry = (play_map_t){0};
    portEXIT_CRITICAL(&s_map_lock);
}

static bool suppress_report(uint32_t connection_id, uint64_t server_id)
{
    bool found = false;
    portENTER_CRITICAL(&s_map_lock);
    play_map_t *entry = find_server_id_locked(connection_id, server_id);
    if (entry != NULL) {
        entry->suppress_terminal_report = true;
        found = true;
    }
    portEXIT_CRITICAL(&s_map_lock);
    return found;
}

static void suppress_connection(uint32_t connection_id)
{
    portENTER_CRITICAL(&s_map_lock);
    for (size_t i = 0; i < PLAY_MAP_CAPACITY; ++i) {
        if (s_play_map[i].used && s_play_map[i].connection_id == connection_id)
            s_play_map[i].suppress_terminal_report = true;
    }
    portEXIT_CRITICAL(&s_map_lock);
}

static void on_playback_event(orbita_play_event_t event, uint32_t token, void *ctx)
{
    (void)ctx;
    if (event == ORBITA_PLAY_EV_STARTED) {
        int64_t now_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_map_lock);
        play_map_t *entry = find_token_locked(token);
        if (entry != NULL) {
            entry->started = true;
            entry->started_us = now_us;
        }
        portEXIT_CRITICAL(&s_map_lock);
        return;
    }

    if (event == ORBITA_PLAY_EV_UNDERRUN) {
        portENTER_CRITICAL(&s_map_lock);
        play_map_t *entry = find_token_locked(token);
        if (entry != NULL) entry->had_underrun = true;
        portEXIT_CRITICAL(&s_map_lock);
        return;
    }

    if (event != ORBITA_PLAY_EV_DONE && event != ORBITA_PLAY_EV_STOPPED) return;

    int64_t now_us = esp_timer_get_time();
    play_map_t completed = {0};
    portENTER_CRITICAL(&s_map_lock);
    play_map_t *entry = find_token_locked(token);
    if (entry != NULL) {
        completed = *entry;
        *entry = (play_map_t){0};
    }
    portEXIT_CRITICAL(&s_map_lock);
    if (!completed.used || completed.suppress_terminal_report) return;

    orbita_ws_play_reason_t reason = event == ORBITA_PLAY_EV_STOPPED
        ? ORBITA_WS_PLAY_STOPPED
        : (completed.had_underrun ? ORBITA_WS_PLAY_UNDERRUN : ORBITA_WS_PLAY_FINISHED);
    uint32_t played_ms = 0;
    if (completed.started && now_us >= completed.started_us) {
        uint64_t elapsed_ms = (uint64_t)(now_us - completed.started_us) / 1000u;
        played_ms = elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
    }

    // Esta API solo encola: el callback de playback no espera a la red.
    esp_err_t err = orbita_ws_play_done(completed.connection_id, completed.server_play_id,
                                        reason, played_ms);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "No se pudo encolar play_done (%s)", esp_err_to_name(err));
}

esp_err_t audio_ws_playback_init(void)
{
    const orbita_playback_cfg_t config = {
        .gpio_bclk = ORBITA_PLAY_BCLK_GPIO,
        .gpio_ws = ORBITA_PLAY_WS_GPIO,
        .gpio_dout = ORBITA_PLAY_DOUT_GPIO,
        .prebuffer_ms = PLAY_PREBUFFER_MS,
        .cb = on_playback_event,
        .cb_ctx = NULL,
    };
    return orbita_playback_init(&config);
}

bool audio_ws_playback_on_downlink(void *ctx, const orbita_ws_downlink_event_t *event)
{
    (void)ctx;
    if (event == NULL) return false;

    switch (event->kind) {
    case ORBITA_WS_PLAY_BEGIN: {
        // begin() puede esperar hasta 100 ms si un stop sigue drenando. Si aun esta activo,
        // rechazamos este PLAY sin bloquear la tarea que recibe el WebSocket.
        if (orbita_playback_is_active()) return false;
        uint32_t token;
        if (!reserve_play(event->connection_id, event->id, &token)) return false;
        esp_err_t err = orbita_playback_begin(token);
        if (err != ESP_OK) {
            remove_token(token);
            return false; // orbita_ws encola el unico play_reject
        }
        s_rx_crc = s_rx_bytes = 0;
        return true;
    }
    case ORBITA_WS_PLAY_DATA: {
        uint32_t token = 0;
        portENTER_CRITICAL(&s_map_lock);
        play_map_t *entry = find_server_id_locked(event->connection_id, event->id);
        if (entry != NULL) token = entry->token;
        portEXIT_CRITICAL(&s_map_lock);
        if (token == 0) return false;
        // push copia el PCM al ring antes de que el buffer WebSocket se reutilice.
        // En caso de ring lleno, false hace que orbita_ws envie reject y despache STOP.
        uint32_t crc = orbita_wire_crc(s_rx_crc, event->pcm, event->pcm_bytes);
        if (orbita_playback_push(event->pcm, event->pcm_bytes) != ESP_OK) return false;
        s_rx_crc = crc;
        s_rx_bytes += event->pcm_bytes;
        return true;
    }
    case ORBITA_WS_PLAY_END: {
        portENTER_CRITICAL(&s_map_lock);
        bool known = find_server_id_locked(event->connection_id, event->id) != NULL;
        portEXIT_CRITICAL(&s_map_lock);
        if (!known || orbita_playback_end() != ESP_OK) return false;
        ESP_LOGI(TAG, "PLAY RX: id=%016" PRIx64 " bytes=%" PRIu32 " crc32=%08" PRIx32,
                 event->id, s_rx_bytes, s_rx_crc);
        return true;
    }
    case ORBITA_WS_PLAY_STOP:
        if (event->rejected) suppress_report(event->connection_id, event->id);
        if (orbita_playback_is_active()) (void)orbita_playback_stop();
        return true;
    case ORBITA_WS_DOWNLINK_RESET:
        suppress_connection(event->connection_id);
        if (orbita_playback_is_active()) (void)orbita_playback_stop();
        return true;
    case ORBITA_WS_CONVERSATION_BEGIN:
    case ORBITA_WS_CONVERSATION_END:
        // El indicador LED de conversación todavía no tiene consumidor conectado.
        return true;
    }
    return false;
}

bool audio_ws_playback_is_active(void)
{
    return orbita_playback_is_active();
}

#endif
