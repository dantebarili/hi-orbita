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
#define ORBITA_PLAY_BCLK_GPIO  15   // igual que la PCB impresa (BCLK y LRC estaban al reves en el firmware anterior)
#define ORBITA_PLAY_WS_GPIO     7
#define ORBITA_PLAY_DOUT_GPIO  16

/* VOLUMEN LIMITADO POR SOFTWARE: este es el unico lugar donde se cambia. Toda muestra se multiplica por esta
 * ganancia (Q15: 32768 = 1,0; 8192 = 0,25 = -12 dB) justo antes de i2s_channel_write, en aplicar_ganancia() de
 * orbita_playback.c. Es el unico camino al I2S de salida: afecta a todo lo que suene, venga de donde venga.
 * Protege parlante y alimentacion (hoy el ampli va a 5 V externos). Subirlo de a poco. Rango 0 a 32768. */
#define ORBITA_PLAY_GAIN_Q15  8192

/* EVENTOS: hechos ASINCRONOS que el modulo avisa hacia afuera por el callback (que se hace con ellos lo
 * decide quien escucha). Cada evento trae el `play_id` que se paso a orbita_playback_begin() para la
 * respuesta a la que pertenece: un DONE atrasado lleva el id de la respuesta que termino, no el de una nueva.
 *  - TODOS salen desde la tarea de reproduccion (un solo contexto), asi que llegan EN ORDEN:
 *    STARTED -> (UNDERRUN)* -> DONE o STOPPED. Nunca se cruzan.
 *  - CONTRATO: cada begin() que devuelve ESP_OK termina en EXACTAMENTE un evento terminal, DONE o STOPPED.
 *    Si se corta la conexion a mitad de una respuesta, quien recibe el audio debe llamar a stop().
 *  - Los rechazos (begin/push/end/stop) NO son eventos: se informan SOLO por el valor de retorno, en la tarea
 *    de quien llamo. Asi no hay dos avisos (retorno + callback) del mismo rechazo.
 *  - El callback tiene que ser rapido y NO bloquear (corre en la pila de 4 KB de la tarea de reproduccion):
 *    copiar el dato y salir. Puede llamar a orbita_playback_is_active(); NO a begin/stop desde adentro. */
typedef enum {
    ORBITA_PLAY_EV_STARTED,    // empezo a sonar (prebuffer lleno, o respuesta corta cerrada por end)
    ORBITA_PLAY_EV_DONE,       // termino la respuesta (llego end y se vacio el ring y el DMA)
    ORBITA_PLAY_EV_STOPPED,    // se corto por orbita_playback_stop()
    ORBITA_PLAY_EV_UNDERRUN,   // el ring se vacio antes de end: se escucho un corte
} orbita_play_event_t;

/* `play_id`: el valor opaco que se paso a begin() (el modulo no lo interpreta). */
typedef void (*orbita_play_cb_t)(orbita_play_event_t ev, uint32_t play_id, void *ctx);

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

/**
 * @brief  Abre una respuesta y le pone un identificador.
 * @param  play_id  Valor opaco que el modulo devuelve en cada evento de ESTA respuesta (p. ej. un indice a
 *                  una tabla de quien recibe el audio con el play_id del servidor y la conexion).
 * @return ESP_OK; ESP_ERR_INVALID_STATE si ya hay una respuesta en curso (o un stop que no termino en 100 ms)
 *         o el modulo no esta iniciado. Sin evento: el rechazo se informa solo por el retorno.
 */
esp_err_t orbita_playback_begin(uint32_t play_id);

/**
 * @brief  Agrega `len` bytes de PCM (par, <= 4 KB por llamada). Copia al ring; no espera si esta lleno.
 * @return ESP_OK; ESP_ERR_INVALID_ARG (puntero nulo, largo 0, impar o > 4 KB); ESP_ERR_INVALID_STATE (no hay
 *         respuesta abierta, o se esta cortando); ESP_ERR_NO_MEM (ring lleno: el frame NO entro y la respuesta
 *         quedo con un hueco; quien llama deberia mandar play_reject y llamar a stop()).
 */
esp_err_t orbita_playback_push(const uint8_t *data, size_t len);

/** @brief  Avisa que no llegan mas datos; DONE sale cuando se vacian el ring y el DMA. ESP_ERR_INVALID_STATE si no hay respuesta abierta. */
esp_err_t orbita_playback_end(void);

/** @brief  Pide cortar ya: se descarta lo que falta y sale STOPPED desde la tarea de reproduccion.
 *          Lo que ya esta en el DMA (hasta 120 ms) suena igual. ESP_ERR_INVALID_STATE si no hay nada que cortar. */
esp_err_t orbita_playback_stop(void);

/** @brief  true entre begin y DONE/STOPPED. 
 * Sirve para marcar el flag "mic durante el parlante" (S8). */
bool orbita_playback_is_active(void);
