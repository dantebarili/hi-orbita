#include "sdkconfig.h"
#if CONFIG_ORBITA_LAB_WEBSOCKET
// Ver el cartel en audio_ws_conversacion.h. Lo llaman tres tareas distintas: wakeword (on_wake),
// la del WebSocket (fin, actividad) y la de esp_timer (timeout). El estado va protegido con un spinlock.

#include "audio_ws_conversacion.h"
#include "audio_sink.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>

static const char *TAG = "conversacion";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_en_conversacion;
static esp_timer_handle_t s_timer;
static uint32_t s_conversaciones;

static void reiniciar_timeout(void)
{
    (void)esp_timer_stop(s_timer);                  // si no estaba corriendo devuelve error: no importa
    (void)esp_timer_start_once(s_timer, (uint64_t)AUDIO_WS_CONV_TIMEOUT_S * 1000000);
}

static void cerrar(const char *motivo)
{
    portENTER_CRITICAL(&s_lock);
    bool estaba = s_en_conversacion;
    s_en_conversacion = false;
    portEXIT_CRITICAL(&s_lock);
    if (!estaba) return;
    orbita_sink_cerrar();
    (void)esp_timer_stop(s_timer);
    ESP_LOGI(TAG, "Fin de la conversacion %u (%s): se deja de mandar audio", (unsigned)s_conversaciones, motivo);
}

static void on_timeout(void *arg)
{
    (void)arg;
    cerrar("timeout local, el servidor no la cerro");
}

esp_err_t audio_ws_conversacion_init(void)
{
    orbita_sink_cerrar();
    const esp_timer_create_args_t args = { .callback = on_timeout, .name = "conv_timeout" };
    return esp_timer_create(&args, &s_timer);
}

void audio_ws_conversacion_on_wake(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ya = s_en_conversacion;
    s_en_conversacion = true;
    portEXIT_CRITICAL(&s_lock);
    if (ya) {
        ESP_LOGI(TAG, "Wake word durante la conversacion: ya se esta mandando audio");
        return;
    }
    s_conversaciones++;
    orbita_sink_abrir(AUDIO_WS_PREROLL_SLOTS);
    reiniciar_timeout();
    ESP_LOGI(TAG, "Conversacion %u: se manda el pre-roll y despues en tiempo real", (unsigned)s_conversaciones);
}

void audio_ws_conversacion_actividad(void)
{
    if (s_en_conversacion) reiniciar_timeout();
}

void audio_ws_conversacion_fin(void)
{
    cerrar("conversation_end del servidor");
}

#endif
