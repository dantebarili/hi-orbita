/**
 * @file    orbita_tiempo.h
 * @brief   Cronometro para medir cuanto tarda un tramo de codigo (A15). Guarda el MAXIMO, no el promedio.
 * @details Uso:  uint32_t t0 = orbita_t_us();  ...tramo...;  orbita_max_us(&s_max_algo_us, t0);
 *          El contador es de 32 bit (da la vuelta a los ~71 min), pero la resta sin signo sirve igual
 *          para tramos cortos. Cada variable `max` la escribe UNA sola tarea.
 */

#pragma once

#include <stdint.h>
#include "esp_timer.h"

static inline uint32_t orbita_t_us(void)
{
    return (uint32_t)esp_timer_get_time();     // microsegundos desde el arranque
}

// Guarda en *max lo que paso desde t0, si es mayor que lo que ya habia.
static inline void orbita_max_us(volatile uint32_t *max, uint32_t t0)
{
    uint32_t dt = orbita_t_us() - t0;
    if (dt > *max) {
        *max = dt;
    }
}
