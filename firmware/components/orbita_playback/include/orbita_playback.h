/**
 * @file    orbita_playback.h
 * @brief   Parlante: recibe PCM por partes, lo guarda en un ring en PSRAM y lo reproduce por I2S.
 * @details Interfaz acordada en el Paso 0 (plan_prototipo_alfa.md, INTEGRACION_WEBSOCKET.md "Downlink").
 *          Este modulo no sabe de sockets: quien reciba los frames PLAY llama a begin/push/end/stop.
 *          Formato de entrada: PCM s16le, 16 kHz, mono. Tarea de salida: core 0, prioridad 6
 *          (arquitectura.md §2).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* PINES del I2S de salida hacia el amplificador MAX98357A. Es el UNICO lugar donde se definen:
 * quien llame a orbita_playback_init() los pasa en la config (gpio_bclk, gpio_ws, gpio_dout).
 * Elegidos con pin-map-check el 14/10 (fuera de flash, PSRAM, USB-JTAG, UART0, strapping y mics 4/5/6).
 *   ORBITA_PLAY_BCLK_GPIO  -> BCLK del amplificador
 *   ORBITA_PLAY_WS_GPIO    -> LRC (word select) del amplificador
 *   ORBITA_PLAY_DOUT_GPIO  -> DIN del amplificador
 * Ojo: GPIO 15 y 16 son XTAL_32K en el ESP32-S3: si la PCB lleva cristal de 32 kHz, cambiarlos.
 * Alternativa sin ese compromiso: 7, 8, 9. Pines de alimentacion y control del MAX98357A (VIN, SD, GAIN):
 * sin verificar contra su datasheet. */
#define ORBITA_PLAY_BCLK_GPIO   7
#define ORBITA_PLAY_WS_GPIO    15
#define ORBITA_PLAY_DOUT_GPIO  16

/* EVENTOS: lo que el modulo avisa hacia afuera por el callback (los HECHOS que pasaron; que se hace
 * con ellos lo decide quien escucha). Ojo con el contexto:
 *  - Pueden llegar desde la tarea de red (begin/push/end) o desde la tarea de reproduccion
 *    (STARTED, DONE, STOPPED, UNDERRUN), y NO hay orden garantizado entre dos eventos emitidos desde
 *    tareas distintas (por ejemplo, DONE puede cruzarse con STARTED en una respuesta cortisima).
 *  - El callback tiene que ser reentrante, NO bloquear ni hacer trabajo largo (corre en la pila de
 *    4 KB de la tarea de reproduccion): copiar el dato y salir.
 *  - Puede llamar a orbita_playback_is_active(); NO conviene que llame a begin/stop desde adentro. */
typedef enum {
    ORBITA_PLAY_EV_STARTED,    // se lleno el prebuffer y empezo a sonar
    ORBITA_PLAY_EV_DONE,       // termino la respuesta (llego play_end y se vacio el ring)
    ORBITA_PLAY_EV_STOPPED,    // se corto por orbita_playback_stop()
    ORBITA_PLAY_EV_UNDERRUN,   // el ring se vacio antes de play_end: se escucho un corte
    ORBITA_PLAY_EV_REJECTED,   // begin/push rechazado (ocupado, o no entra en el ring)
} orbita_play_event_t;

typedef void (*orbita_play_cb_t)(orbita_play_event_t ev, void *ctx);

/* CONFIG: los pines NO estan fijos en el modulo (siguen pendientes, se validan con pin-map-check).
 *  gpio_bclk, gpio_ws, gpio_dout:  pines del I2S de salida hacia el amplificador clase D
 *  prebuffer_ms:                   audio acumulado antes de empezar a sonar (acordado: 400)
 *  cb, cb_ctx:                     callback de eventos (puede ser NULL) */
typedef struct {
    int              gpio_bclk;
    int              gpio_ws;
    int              gpio_dout;
    uint32_t         prebuffer_ms;
    orbita_play_cb_t cb;
    void            *cb_ctx;
} orbita_playback_cfg_t;

// Funciones que llama quien recibe el audio (órdenes hacia playback.c).
// playback.c las implementa; el resultado vuelve por el callback.

/**
 * @brief  Crea el ring en PSRAM, el canal I2S de salida y la tarea. Llamar UNA vez.
 * @return ESP_OK, o el error del driver / ESP_ERR_NO_MEM.
 */
esp_err_t orbita_playback_init(const orbita_playback_cfg_t *cfg);

/** @brief  Abre una respuesta. ESP_ERR_INVALID_STATE (+ evento REJECTED) si ya hay una en curso. */
esp_err_t orbita_playback_begin(void);

/** @brief  Agrega `len` bytes de PCM (<= 4 KB por llamada). Copia al ring; no espera si esta lleno. */
esp_err_t orbita_playback_push(const uint8_t *data, size_t len);

/** @brief  Avisa que no llegan mas datos; el evento DONE sale cuando el ring se vacia. */
esp_err_t orbita_playback_end(void);

/** @brief  Corta ya: descarta el ring y silencia el I2S. Evento STOPPED. */
esp_err_t orbita_playback_stop(void);

/** @brief  true entre begin y DONE/STOPPED. 
 * Sirve para marcar el flag "mic durante el parlante" (S8). */
bool orbita_playback_is_active(void);
