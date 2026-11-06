#include "orbita_nfc.h"
#include "pn532_frame.h"
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// NXP UM0701-02: 6.1.1.3 (0x48 en formato de 8 bits => 0x24 en IDF),
// 6.2.4 (status RDY + tramas), 7.2.10 (SAM), 7.3.1 (reintentos), 7.3.5 (Type A).
static const uint8_t ack[] = {0, 0, 0xff, 0, 0xff, 0};
static const char *TAG = "pn532";

static void irq_handler(void *arg)
{
    orbita_nfc_t *nfc = arg;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(nfc->irq_signal, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static esp_err_t wait_ready(orbita_nfc_t *nfc)
{
    int64_t end = esp_timer_get_time() + 1000000; // límite de laboratorio: 1 s
    // Nivel antes de dormir: también funciona si el flanco ocurrió antes.
    // Semáforos viejos no autorizan leer: siempre comprobar IRQ activa.
    while (gpio_get_level(nfc->irq_pin) != 0) {
        int64_t remaining = end - esp_timer_get_time();
        if (remaining <= 0) {
            ESP_LOGE(TAG, "Timeout esperando IRQ GPIO%d", nfc->irq_pin);
            return ESP_ERR_TIMEOUT;
        }
        TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
        xSemaphoreTake(nfc->irq_signal, ticks ? ticks : 1);
    }
    return ESP_OK;
}

static esp_err_t command(orbita_nfc_t *nfc, const uint8_t *cmd, size_t cmd_len,
                         uint8_t *out, size_t cap, size_t *out_len)
{
    uint8_t tx[40], rx[65];
    size_t n = pn532_encode(cmd, cmd_len, tx, sizeof(tx));
    if (!n || !out || !out_len) return ESP_ERR_INVALID_ARG;
    *out_len = 0;
    esp_err_t err = i2c_master_transmit(nfc->device, tx, n, 100);
    vTaskDelay(pdMS_TO_TICKS(10)); // separación entre transacciones, también al despertar
    if (err == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(10)); err = wait_ready(nfc); }
    if (err == ESP_OK) err = i2c_master_receive(nfc->device, rx, sizeof(ack) + 1, 100);
    if (err == ESP_OK && (!(rx[0] & 1) || memcmp(rx + 1, ack, sizeof(ack)))) {
        ESP_LOGE(TAG, "Comando 0x%02X: ACK invalido (status + 6 bytes):", cmd[0]);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, sizeof(ack) + 1, ESP_LOG_ERROR);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) err = wait_ready(nfc);
    if (err == ESP_OK) err = i2c_master_receive(nfc->device, rx, sizeof(rx), 100);
    const uint8_t *payload = NULL;
    size_t length = 0;
    if (err == ESP_OK && (!(rx[0] & 1) || !pn532_decode(rx + 1, sizeof(rx) - 1, cmd[0], &payload, &length))) {
        ESP_LOGE(TAG, "Comando 0x%02X: respuesta invalida (status + trama):", cmd[0]);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, rx, sizeof(rx), ESP_LOG_ERROR);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && length > cap) err = ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) {
        // Sin RSTPD_N: no encadenar comandos tras respuesta incierta. Reiniciar alimentación para recuperar.
        nfc->ready = false;
        ESP_LOGE(TAG, "Comando 0x%02X fallo: %s", cmd[0], esp_err_to_name(err));
        return err;
    }
    memcpy(out, payload, length); *out_len = length;
    return ESP_OK;
}

esp_err_t orbita_nfc_open(orbita_nfc_t *nfc, i2c_master_bus_handle_t bus, gpio_num_t irq_pin)
{
    if (!nfc || !bus || !GPIO_IS_VALID_GPIO(irq_pin)) return ESP_ERR_INVALID_ARG;
    if (nfc->device) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x24,
        .scl_speed_hz = 100000, .scl_wait_us = 20000,
    };
    nfc->ready = false;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &nfc->device);
    if (err != ESP_OK) return err;
    nfc->irq_pin = irq_pin;
    nfc->irq_signal = xSemaphoreCreateBinary();
    if (!nfc->irq_signal) { err = ESP_ERR_NO_MEM; goto fail; }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << irq_pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&io);
    if (err != ESP_OK) goto fail;
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = gpio_isr_handler_add(irq_pin, irq_handler, nfc);
    if (err == ESP_OK) return ESP_OK;
    gpio_intr_disable(irq_pin);
fail:
    if (nfc->irq_signal) { vSemaphoreDelete(nfc->irq_signal); nfc->irq_signal = NULL; }
    i2c_master_bus_rm_device(nfc->device); nfc->device = NULL;
    return err;
}

esp_err_t orbita_nfc_close(orbita_nfc_t *nfc)
{
    if (!nfc || !nfc->device) return ESP_ERR_INVALID_STATE;
    gpio_intr_disable(nfc->irq_pin);
    gpio_isr_handler_remove(nfc->irq_pin);
    esp_err_t err = i2c_master_bus_rm_device(nfc->device);
    if (err == ESP_OK) {
        nfc->device = NULL; nfc->ready = false;
        vSemaphoreDelete(nfc->irq_signal); nfc->irq_signal = NULL;
    }
    return err;
}

esp_err_t orbita_nfc_begin(orbita_nfc_t *nfc, orbita_nfc_version_t *version)
{
    if (!nfc || !nfc->device || !version) return ESP_ERR_INVALID_ARG;
    nfc->ready = false;
    uint8_t out[32]; size_t n;
    // SAM primero: despierta/configura el lector. Timeout de SAM no rige el sondeo de tarjetas.
    const uint8_t sam[] = {0x14, 0x01, 0x14, 0x01}; // normal, IRQ habilitada
    esp_err_t err = command(nfc, sam, sizeof(sam), out, sizeof(out), &n);
    if (err != ESP_OK) return err;
    if (n != 0) {
        ESP_LOGE(TAG, "SAM: payload inesperado (%u bytes)", (unsigned)n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, out, n, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t fw[] = {0x02};
    err = command(nfc, fw, sizeof(fw), out, sizeof(out), &n);
    if (err != ESP_OK) return err;
    if (n != 4 || out[0] != 0x32 || !(out[3] & 1)) {
        ESP_LOGE(TAG, "Version: payload inesperado (%u bytes)", (unsigned)n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, out, n, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *version = (orbita_nfc_version_t){out[0], out[1], out[2], out[3]};
    const uint8_t retries[] = {0x32, 0x05, 0xff, 0x01, 0x00}; // búsqueda Type A finita
    err = command(nfc, retries, sizeof(retries), out, sizeof(out), &n);
    if (err != ESP_OK) return err;
    if (n != 0) {
        ESP_LOGE(TAG, "RFConfiguration: payload inesperado (%u bytes)", (unsigned)n);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, out, n, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_RESPONSE;
    }
    nfc->ready = true;
    return ESP_OK;
}

esp_err_t orbita_nfc_read_uid(orbita_nfc_t *nfc, orbita_nfc_uid_t *uid, bool *present)
{
    if (!nfc || !uid || !present) return ESP_ERR_INVALID_ARG;
    *present = false; memset(uid, 0, sizeof(*uid));
    if (!nfc->device || !nfc->ready) return ESP_ERR_INVALID_STATE;
    const uint8_t scan[] = {0x4a, 0x01, 0x00}; // una tarjeta, ISO14443A a 106 kbps
    uint8_t out[55]; size_t n; // incluye el ATS opcional dentro del límite de trama de 64 bytes
    esp_err_t err = command(nfc, scan, sizeof(scan), out, sizeof(out), &n);
    if (err != ESP_OK) return err;
    if (n == 1 && out[0] == 0) return ESP_OK;
    if (!pn532_uid(out, n, uid->bytes, &uid->length)) {
        nfc->ready = false;
        return ESP_ERR_INVALID_RESPONSE;
    }
    *present = true;
    return ESP_OK;
}
