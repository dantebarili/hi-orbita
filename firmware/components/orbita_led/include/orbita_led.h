/**
 * @file    orbita_led.h
 * @brief   Aro de 24 LEDs WS2812: un color, un brillo y un patron (fijo, respiracion o latido) para todo el aro.
 * @details Este modulo no sabe de conversaciones: quien decide QUE mostrar es audio_ws_led.c (la tabla de estados).
 *          orbita_led_set() no bloquea y se puede llamar desde cualquier tarea (no desde una ISR).
 *          Simulacion con estos mismos numeros: artifact "Aro LED de Orbita".
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

/* ARO LED (datos clave del modulo)
 *  - Pin:        GPIO17 (pad 10 del WROOM-1), con pin-map-check el 31/10. 330 ohm en serie; en la PCB, buffer
 *                a 5 V (74AHCT1G125): el WS2812 a 5 V espera un "1" de ~3,5 V y el S3 da 3,3 V (sin verificar
 *                contra el datasheet del LED).
 *  - Senal:      RMT a 10 MHz con DMA, buffer de 1024 simbolos (~4 KB de RAM interna): la trama entera sale de
 *                una vez, sin depender de que el Wi-Fi deje correr una interrupcion. Pila de la tarea: 3 KB.
 *  - Tarea:      "led", core 0, prioridad 2. Un cuadro cada 20 ms mientras anima; quieta si el estado es fijo.
 *  - Corriente:  estimada en cada cuadro como 24 x (suma de canales x 20 mA + 1 mA). Si pasa el tope, baja todo
 *                el aro en proporcion. Blanco pleno sin tope = ~1,4 A. */
#define ORBITA_LED_GPIO        17
#define ORBITA_LED_CANTIDAD    24
#define ORBITA_LED_MAX_MA      350   // tope de corriente del aro (orbita-pcb: presupuesto del 5 V)
#define ORBITA_LED_MA_CANAL    20    // por canal a pleno; dato de la publicacion del aro, sin datasheet
#define ORBITA_LED_TOPE        153   // brillo maximo percibido, 0-255 (153 = 60 %); escala a todos los estados
#define ORBITA_LED_PERIODO_MS  1300  // respiracion y latido
#define ORBITA_LED_PISO        35    // % del brillo del estado en el punto mas bajo de la respiracion/latido
#define ORBITA_LED_FUNDIDO_MS  300   // transicion entre estados

typedef struct {
    uint8_t r, g, b;
} orbita_rgb_t;

typedef enum {
    ORBITA_LED_FIJO,
    ORBITA_LED_RESPIRA,   // sube y baja suave (coseno)
    ORBITA_LED_LATIDO,    // dos pulsos cortos ("lub-dub") y pausa
} orbita_led_patron_t;

/** @brief Crea el canal RMT y la tarea "led". Arranca apagado. */
esp_err_t orbita_led_init(void);

/**
 * @brief  Pide un estado nuevo: el aro funde hacia el en ORBITA_LED_FUNDIDO_MS.
 * @param  color   color del aro
 * @param  brillo  0-255, brillo percibido (antes del tope y de la correccion gamma)
 * @param  patron  fijo, respiracion o latido
 * @note   Si llegan dos pedidos antes de que la tarea los vea, gana el ultimo.
 */
void orbita_led_set(orbita_rgb_t color, uint8_t brillo, orbita_led_patron_t patron);
