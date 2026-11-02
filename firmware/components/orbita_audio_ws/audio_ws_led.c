#include "sdkconfig.h"
// Ver el cartel en audio_ws_led.h. Lo llaman wakeword, WebSocket, playback y esp_timer (todas tareas, ninguna
// ISR). Un mutex cubre los tres datos Y el pedido al aro: si dos tareas recalculan a la vez, el ultimo pedido
// que llega al aro es siempre el del estado mas nuevo. (Un spinlock no sirve: adentro no se puede usar la cola.)

#include "audio_ws_led.h"
#include "orbita_led.h"
#include "orbita_ws.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "led_estado";

#define LED_RED_PERIODO_MS  500

typedef enum { REPOSO, ESCUCHANDO, HABLANDO, SIN_RED, N_ESTADOS } estado_t;

typedef struct {
    orbita_rgb_t color;
    uint8_t brillo;                // 0-255, antes del tope global ORBITA_LED_TOPE (60 %)
    orbita_led_patron_t patron;
} estilo_t;

// TABLA DE COLORES (31/10, elegida con la simulacion "Aro LED de Orbita"; brillos subidos el 01/11).
// Con la gamma 2,2 de orbita_led, el reposo al 12 % salia RGB (0,0,1) = apagado, y el ambar al 45 % casi no se veia a
// traves de la pieza impresa. Valores al aro con TOPE 60 % y 200 mA: reposo (1,5,10); ambar pulso (66,26,1);
// escuchando hasta (8,29,57); hablando (19,21,53).
static const estilo_t ESTILOS[N_ESTADOS] = {
    [REPOSO]     = { {0x64, 0xB5, 0xF6}, 100, ORBITA_LED_FIJO    },   // celeste, 39 % (era 12 %): tenue pero visible
    [ESCUCHANDO] = { {0x64, 0xB5, 0xF6}, 255, ORBITA_LED_RESPIRA },   // el mismo celeste, pleno
    [HABLANDO]   = { {0x9F, 0xA8, 0xFF}, 255, ORBITA_LED_FIJO    },   // lavanda
    [SIN_RED]    = { {0xFF, 0xA7, 0x26}, 255, ORBITA_LED_LATIDO  },   // ambar, pleno (era 45 %)
};
static const char *NOMBRES[N_ESTADOS] = { "reposo", "escuchando", "hablando", "sin red" };

static SemaphoreHandle_t s_mutex;
static bool s_conversacion, s_hablando, s_red_ok;
static estado_t s_mostrado = N_ESTADOS;     // N_ESTADOS = todavia no se mostro nada
static esp_timer_handle_t s_timer_red;

/* Cambia UN dato (con el mutex tomado) y, si el estado resultante es otro, se lo pide al aro.
 * `dato` apunta a s_conversacion, s_hablando o s_red_ok: asi las tres entradas comparten el mismo camino. */
static void actualizar(bool *dato, bool valor)
{
    if (s_mutex == NULL) return;            // antes de init: no hay aro
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *dato = valor;
    estado_t e = s_hablando ? HABLANDO : !s_red_ok ? SIN_RED : s_conversacion ? ESCUCHANDO : REPOSO;
    if (e != s_mostrado) {                  // mismo estado: no reiniciar la animacion
        s_mostrado = e;
        orbita_led_set(ESTILOS[e].color, ESTILOS[e].brillo, ESTILOS[e].patron);   // no bloquea
        ESP_LOGI(TAG, "LED: %s", NOMBRES[e]);
    }
    xSemaphoreGive(s_mutex);
}

static void on_timer_red(void *arg)
{
    (void)arg;
    actualizar(&s_red_ok, orbita_ws_is_connected());
}

esp_err_t audio_ws_led_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = orbita_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sin aro LED (%s): el equipo sigue sin indicador", esp_err_to_name(err));
        return err;
    }
    actualizar(&s_red_ok, false);           // al arrancar todavia no hay red: SIN RED hasta que conecte
    const esp_timer_create_args_t args = { .callback = on_timer_red, .name = "led_red" };
    err = esp_timer_create(&args, &s_timer_red);
    if (err == ESP_OK) err = esp_timer_start_periodic(s_timer_red, (uint64_t)LED_RED_PERIODO_MS * 1000);
    return err;
}

void audio_ws_led_conversacion(bool abierta) { actualizar(&s_conversacion, abierta); }

void audio_ws_led_hablando(bool hablando) { actualizar(&s_hablando, hablando); }

