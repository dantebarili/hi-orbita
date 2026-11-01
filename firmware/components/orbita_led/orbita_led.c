// Ver el cartel en orbita_led.h. Una sola tarea toca el RMT; el resto del firmware solo deja pedidos en el buzon.

#include "orbita_led.h"
#include <math.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "led";

#define LED_TAREA_STACK  3072
#define LED_TAREA_PRIO   2
#define LED_TAREA_CORE   0
#define LED_CUADRO_MS    20
#define LED_GAMMA        2.2f
#define LED_MA_CHIP      1.0f      // consumo propio de cada WS2812 con los LEDs apagados (estimado)

typedef struct {
    orbita_rgb_t color;
    uint8_t brillo;
    orbita_led_patron_t patron;
} pedido_t;

static led_strip_handle_t s_tira;
static QueueHandle_t s_buzon;      // cola de 1 lugar: xQueueOverwrite pisa el pedido anterior si no se leyo

/* Factor 0..1 que multiplica el brillo del estado. t_ms cuenta desde que empezo el estado, asi la respiracion
 * arranca abajo y el latido arranca con su primer pulso. Mismas curvas que la simulacion. */
static float patron_factor(orbita_led_patron_t patron, int64_t t_ms)
{
    if (patron == ORBITA_LED_FIJO) return 1.0f;
    const float piso = ORBITA_LED_PISO / 100.0f;
    const float x = (float)(t_ms % ORBITA_LED_PERIODO_MS) / ORBITA_LED_PERIODO_MS;   // fase 0..1
    float onda;
    if (patron == ORBITA_LED_RESPIRA) {
        onda = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * x);
    } else {
        // Dos campanas (gaussianas): un pulso a 10 % del periodo y otro mas chico a 30 %.
        const float a = (x - 0.10f) / 0.045f, b = (x - 0.30f) / 0.05f;
        const float lub = expf(-0.5f * a * a), dub = 0.62f * expf(-0.5f * b * b);
        onda = lub > dub ? lub : dub;
    }
    return piso + (1.0f - piso) * onda;
}

/* Intensidad percibida 0..1 de cada canal para un pedido, en el instante t_ms. */
static void objetivo(const pedido_t *p, int64_t t_ms, float out[3])
{
    const float k = (p->brillo / 255.0f) * (ORBITA_LED_TOPE / 255.0f) * patron_factor(p->patron, t_ms);
    out[0] = p->color.r / 255.0f * k;
    out[1] = p->color.g / 255.0f * k;
    out[2] = p->color.b / 255.0f * k;
}

/* Percibido -> ciclo de trabajo (gamma), tope de corriente y escritura al aro. */
static void mostrar(const float percibido[3])
{
    float duty[3], suma = 0.0f;
    for (int c = 0; c < 3; c++) {
        float v = percibido[c] < 0.0f ? 0.0f : (percibido[c] > 1.0f ? 1.0f : percibido[c]);
        duty[c] = powf(v, LED_GAMMA);
        suma += duty[c];
    }
    // Corriente estimada de los canales; el consumo propio de los chips no se puede bajar.
    const float ma_canales = ORBITA_LED_CANTIDAD * suma * ORBITA_LED_MA_CANAL;
    const float ma_libre = ORBITA_LED_MAX_MA - ORBITA_LED_CANTIDAD * LED_MA_CHIP;
    const float escala = ma_canales > ma_libre ? ma_libre / ma_canales : 1.0f;

    uint8_t rgb[3];
    for (int c = 0; c < 3; c++) rgb[c] = (uint8_t)lrintf(duty[c] * escala * 255.0f);
    for (int i = 0; i < ORBITA_LED_CANTIDAD; i++) led_strip_set_pixel(s_tira, i, rgb[0], rgb[1], rgb[2]);
    esp_err_t err = led_strip_refresh(s_tira);
    if (err != ESP_OK) ESP_LOGW(TAG, "refresh fallo: %s", esp_err_to_name(err));
}

static void tarea_led(void *arg)
{
    (void)arg;
    pedido_t actual = { .brillo = 0, .patron = ORBITA_LED_FIJO };
    float desde[3] = {0}, ahora[3] = {0};
    int64_t inicio_us = esp_timer_get_time();
    bool animando = false;

    for (;;) {
        // Animando: un cuadro cada 20 ms. Quieto (fijo y fundido terminado): espera sin gastar CPU.
        pedido_t nuevo;
        TickType_t espera = animando ? pdMS_TO_TICKS(LED_CUADRO_MS) : portMAX_DELAY;
        if (xQueueReceive(s_buzon, &nuevo, espera) == pdTRUE) {
            for (int c = 0; c < 3; c++) desde[c] = ahora[c];    // el fundido parte de lo que se ve ahora
            actual = nuevo;
            inicio_us = esp_timer_get_time();
        }

        const int64_t t_ms = (esp_timer_get_time() - inicio_us) / 1000;
        float meta[3];
        objetivo(&actual, t_ms, meta);
        const float f = t_ms >= ORBITA_LED_FUNDIDO_MS ? 1.0f : (float)t_ms / ORBITA_LED_FUNDIDO_MS;
        for (int c = 0; c < 3; c++) ahora[c] = desde[c] + (meta[c] - desde[c]) * f;

        mostrar(ahora);
        animando = actual.patron != ORBITA_LED_FIJO || f < 1.0f;
    }
}

esp_err_t orbita_led_init(void)
{
    led_strip_config_t tira_cfg = {
        .strip_gpio_num = ORBITA_LED_GPIO,
        .max_leds = ORBITA_LED_CANTIDAD,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,   // el WS2812 recibe verde, rojo, azul
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,    // 10 MHz: 0,1 us por tick, sobra para los tiempos del WS2812
        // Con DMA, este es el tamano del buffer: la trama entera (24 x 24 bits = 576 simbolos) entra de una y no
        // depende de recargas por interrupcion mientras el Wi-Fi esta activo. Cuesta ~4 KB de RAM interna.
        .mem_block_symbols = 1024,
        .flags.with_dma = true,
    };
    esp_err_t err = led_strip_new_rmt_device(&tira_cfg, &rmt_cfg, &s_tira);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device fallo: %s", esp_err_to_name(err));
        return err;
    }
    (void)led_strip_clear(s_tira);

    s_buzon = xQueueCreate(1, sizeof(pedido_t));
    if (s_buzon == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(tarea_led, "led", LED_TAREA_STACK, NULL, LED_TAREA_PRIO, NULL, LED_TAREA_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Aro de %d LEDs en GPIO%d, tope %d mA", ORBITA_LED_CANTIDAD, ORBITA_LED_GPIO, ORBITA_LED_MAX_MA);
    return ESP_OK;
}

void orbita_led_set(orbita_rgb_t color, uint8_t brillo, orbita_led_patron_t patron)
{
    if (s_buzon == NULL) return;   // sin init (por ejemplo, el aro fallo al arrancar): no hace nada
    const pedido_t p = { .color = color, .brillo = brillo, .patron = patron };
    (void)xQueueOverwrite(s_buzon, &p);
}
