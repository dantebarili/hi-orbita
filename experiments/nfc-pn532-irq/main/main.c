#include <stdio.h>
#include <string.h>
#include "orbita_nfc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Solo banco: asignación acordada en este chat; todavía no integrada al pinout del repo.
#define NFC_SDA GPIO_NUM_10
#define NFC_SCL GPIO_NUM_11
#define NFC_IRQ GPIO_NUM_12

static void stop_lab(const char *step, esp_err_t err)
{
    ESP_LOGE("nfc_lab", "%s: %s. Revisar cables, 1 ON/2 OFF y reiniciar alimentacion.", step, esp_err_to_name(err));
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    ESP_LOGI("nfc_lab", "NFC IRQ: SDA=10 SCL=11 IRQ=12 activo bajo, I2C 100 kHz, direccion 0x24");
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = NFC_SDA, .scl_io_num = NFC_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, // usar las pull-up de la placa comprobadas en banco
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) stop_lab("Crear bus", err);
    orbita_nfc_t nfc = {0};
    err = orbita_nfc_open(&nfc, bus, NFC_IRQ);
    if (err != ESP_OK) { i2c_del_master_bus(bus); stop_lab("Agregar PN532", err); }
    vTaskDelay(pdMS_TO_TICKS(150));
    orbita_nfc_version_t version;
    err = orbita_nfc_begin(&nfc, &version);
    if (err != ESP_OK) stop_lab("Iniciar PN532", err);
    ESP_LOGI("nfc_lab", "PN532 confirmado: firmware %u.%u, soporte=0x%02X", version.major, version.minor, version.support);
    ESP_LOGI("nfc_lab", "Acercar una tarjeta ISO14443A. Lecturas UID; no escribe ni autentica usuarios.");
    orbita_nfc_uid_t last = {0};
    unsigned absent = 0;
    while (1) {
        orbita_nfc_uid_t uid; bool present;
        err = orbita_nfc_read_uid(&nfc, &uid, &present);
        if (err != ESP_OK) stop_lab("Leer tarjeta", err);
        if (present) {
            absent = 0;
            if (last.length != uid.length || memcmp(last.bytes, uid.bytes, uid.length)) {
                char hex[31] = {0};
                for (unsigned i = 0; i < uid.length; i++) snprintf(hex + 3*i, sizeof(hex) - 3*i, "%02X%s", uid.bytes[i], i+1 == uid.length ? "" : ":");
                ESP_LOGI("nfc_lab", "TARJETA uid=%s bytes=%u", hex, uid.length);
                last = uid;
            }
        } else if (++absent >= 3) {
            if (last.length) ESP_LOGI("nfc_lab", "Tarjeta retirada (3 sondeos sin deteccion)");
            memset(&last, 0, sizeof(last)); absent = 3;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
