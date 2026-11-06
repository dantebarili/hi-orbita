#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    i2c_master_dev_handle_t device;
    bool ready;
    gpio_num_t irq_pin;
    SemaphoreHandle_t irq_signal;
} orbita_nfc_t;

typedef struct { uint8_t ic, major, minor, support; } orbita_nfc_version_t;
typedef struct { uint8_t bytes[10], length; } orbita_nfc_uid_t;

// Inicializar el contexto a cero antes de open. El llamador posee el bus.
// Un solo task debe usar cada lector; no llamar desde ISR/callback de red.
// Las operaciones bloquean con timeout; no se crea una tarea ni se decide login dentro del driver.
esp_err_t orbita_nfc_open(orbita_nfc_t *nfc, i2c_master_bus_handle_t bus, gpio_num_t irq_pin);
esp_err_t orbita_nfc_close(orbita_nfc_t *nfc);
esp_err_t orbita_nfc_begin(orbita_nfc_t *nfc, orbita_nfc_version_t *version);
// ESP_OK + present=false significa bus sano y sin tarjeta; un timeout es un fallo de comunicación.
esp_err_t orbita_nfc_read_uid(orbita_nfc_t *nfc, orbita_nfc_uid_t *uid, bool *present);
// Espera una tarjeta SIN polling desde el ESP (InAutoPoll, UM0701-02 7.3.13): el PN532 busca solo `polls` veces
// (1-254, cada vuelta 300 ms) y la tarea duerme hasta el IRQ. Bloquea hasta polls x 0,3 s + 2 s.
// ESP_OK + present=false: se agotaron los intentos sin tarjeta (sirve de chequeo de que el lector sigue vivo).
esp_err_t orbita_nfc_wait_card(orbita_nfc_t *nfc, uint8_t polls, orbita_nfc_uid_t *uid, bool *present);
