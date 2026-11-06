#include "sdkconfig.h"
// Ver el cartel en audio_ws_nfc.h. La sesion (s_uid) la escribe solo la tarea nfc y la lee el sink: spinlock.

#include "audio_ws_nfc.h"
#include "audio_ws_led.h"
#include "orbita_nfc.h"
#include "orbita_ws.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "nfc";

#define NFC_AUTOPOLL_VUELTAS  100     // InAutoPoll: el PN532 busca solo 100 x 300 ms = 30 s y vuelve (chequeo de vida)
#define NFC_PERIODO_MS        150     // con la tarjeta apoyada: cada cuanto se mira si la sacaron
#define NFC_AUSENTE_TOQUE     3       // busquedas vacias para dar la tarjeta por retirada
#define NFC_REBOTE_MS         1500    // la misma tarjeta que reaparece antes de esto no es un toque nuevo
#define NFC_REINTENTO_MIN_MS  10000   // lector que no contesta: primer reintento...
#define NFC_REINTENTO_MAX_MS  60000   // ...y la espera crece hasta esto (no llenar el monitor de errores)
#define NFC_SEND_TIMEOUT_MS   1000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_uid[AUDIO_WS_NFC_UID_CHARS];      // "" = sin sesion
static orbita_nfc_t s_nfc;                      // contexto del driver: vive mientras exista la tarea

void audio_ws_nfc_uid(char out[AUDIO_WS_NFC_UID_CHARS])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(out, s_uid, AUDIO_WS_NFC_UID_CHARS);
    portEXIT_CRITICAL(&s_lock);
}

// Avisa el cambio al servidor. Sin conexion no se reintenta: el uid igual viaja en el proximo hello/wake.
static void avisar(const char *evento, const char *uid)
{
    if (!orbita_ws_is_connected()) {
        ESP_LOGI(TAG, "Sin conexion: el %s viaja en el proximo hello", evento);
        return;
    }
    char msg[80];
    int n = snprintf(msg, sizeof(msg), "{\"type\":\"nfc\",\"event\":\"%s\",\"uid\":\"%s\"}", evento, uid);
    if (n <= 0 || (size_t)n >= sizeof(msg) ||
        orbita_ws_send(ORBITA_WS_TEXT, msg, (size_t)n, NFC_SEND_TIMEOUT_MS) != ESP_OK)
        ESP_LOGW(TAG, "No se pudo avisar el %s (va en el proximo hello/wake)", evento);
}

static void toque(const orbita_nfc_uid_t *uid)
{
    char hex[AUDIO_WS_NFC_UID_CHARS] = "";
    for (unsigned i = 0; i < uid->length; i++) snprintf(hex + 2 * i, 3, "%02X", uid->bytes[i]);

    char antes[AUDIO_WS_NFC_UID_CHARS];
    portENTER_CRITICAL(&s_lock);
    memcpy(antes, s_uid, sizeof(antes));
    bool misma = strcmp(antes, hex) == 0;
    if (misma) s_uid[0] = '\0';                 // misma tarjeta: cierra la sesion
    else memcpy(s_uid, hex, sizeof(hex));       // sin sesion u otra tarjeta: esta pasa a ser la sesion
    portEXIT_CRITICAL(&s_lock);

    audio_ws_led_nfc(!misma);                   // latido verde (login) o blanco (logout): el medico ve que anduvo
    if (misma) {
        ESP_LOGI(TAG, "Logout %s", hex);
        avisar("logout", hex);
    } else {
        if (antes[0]) ESP_LOGI(TAG, "Cambio de tarjeta: %s -> %s", antes, hex);
        else ESP_LOGI(TAG, "Login %s", hex);
        avisar("login", hex);
    }
}

static bool mismo_uid(const orbita_nfc_uid_t *a, const orbita_nfc_uid_t *b)
{
    return a->length == b->length && memcmp(a->bytes, b->bytes, a->length) == 0;
}

/* Dos fases, para no hacer polling desde el ESP mientras no hay tarjeta:
 *  ESPERA:  InAutoPoll. El PN532 busca solo y la tarea DUERME hasta el IRQ (sin I2C). Cada 30 s vuelve sin
 *           tarjeta y se rearma: si el lector se colgo, ahi se nota.
 *  APOYADA: hay que saber cuando la sacan, y el PN532 no avisa eso: se mira cada 150 ms (InListPassiveTarget)
 *           hasta 3 vacias seguidas. Si en vez de sacarla la cambian por otra, es un toque de la nueva. */
static void tarea_nfc(void *arg)
{
    (void)arg;
    bool listo = false, apoyada = false;
    orbita_nfc_uid_t actual = {0}, retirada = {0};
    int64_t t_retirada = 0;
    unsigned ausente = 0;
    uint32_t espera_ms = NFC_REINTENTO_MIN_MS;
    while (1) {
        if (!listo) {
            orbita_nfc_version_t v;
            if (orbita_nfc_begin(&s_nfc, &v) != ESP_OK) {
                ESP_LOGW(TAG, "Lector NFC sin respuesta: se sigue sin login, reintento en %u s",
                         (unsigned)(espera_ms / 1000));
                vTaskDelay(pdMS_TO_TICKS(espera_ms));
                espera_ms = espera_ms * 2 > NFC_REINTENTO_MAX_MS ? NFC_REINTENTO_MAX_MS : espera_ms * 2;
                continue;
            }
            listo = true;
            espera_ms = NFC_REINTENTO_MIN_MS;
            ESP_LOGI(TAG, "PN532 listo (firmware %u.%u): acercar la tarjeta del medico", v.major, v.minor);
        }
        orbita_nfc_uid_t uid;
        bool presente;
        esp_err_t err = apoyada ? orbita_nfc_read_uid(&s_nfc, &uid, &presente)
                                : orbita_nfc_wait_card(&s_nfc, NFC_AUTOPOLL_VUELTAS, &uid, &presente);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Fallo el lector: se reinicia (la sesion se mantiene)");
            listo = apoyada = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!apoyada) {                                 // ESPERA: volvio el InAutoPoll
            if (!presente) continue;                    // 30 s sin tarjeta: se rearma
            apoyada = true;
            ausente = 0;
            bool rebote = mismo_uid(&uid, &retirada) &&
                          esp_timer_get_time() - t_retirada < (int64_t)NFC_REBOTE_MS * 1000;
            if (!rebote) toque(&uid);
            actual = uid;
        } else if (presente) {                          // APOYADA: sigue ahi, o la cambiaron por otra
            ausente = 0;
            if (!mismo_uid(&uid, &actual)) {
                toque(&uid);
                actual = uid;
            }
        } else if (++ausente >= NFC_AUSENTE_TOQUE) {    // la sacaron: vuelve a esperar sin polling
            apoyada = false;
            retirada = actual;
            t_retirada = esp_timer_get_time();
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(NFC_PERIODO_MS));
    }
}

esp_err_t audio_ws_nfc_start(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = AUDIO_WS_NFC_SDA_GPIO,
        .scl_io_num = AUDIO_WS_NFC_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,  // las del modulo (probadas en el banco)
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) return err;
    err = orbita_nfc_open(&s_nfc, bus, AUDIO_WS_NFC_IRQ_GPIO);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus);
        return err;
    }
    if (xTaskCreatePinnedToCore(tarea_nfc, "nfc", 4096, NULL, 2, NULL, 0) != pdPASS) {
        orbita_nfc_close(&s_nfc);
        i2c_del_master_bus(bus);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "NFC: SDA=%d SCL=%d IRQ=%d", AUDIO_WS_NFC_SDA_GPIO, AUDIO_WS_NFC_SCL_GPIO, AUDIO_WS_NFC_IRQ_GPIO);
    return ESP_OK;
}
