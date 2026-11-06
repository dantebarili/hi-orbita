/**
 * @file    pipeline.h
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Orbita
 * @brief   Pipeline de audio: captura I2S en tarea propia, beamformer y entrega al ring out.
 * @version 0.2.0
 * @date    2026-11-01
 *
 * @copyright Copyright (c) 2026 - Proyecto Orbita
 *
 * @details captura (I2S -> pasa-altos -> 16 bit) -> on_crudo (la wake word, con el mic de adelante)
 *                                                -> ring in -> feed -> beamformer -> ring out (al servidor)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"
#include "beamforming.h"   // ORBITA_BF_ADELANTE_ES_L (orbita_mic_adelante)

// --- Formato del audio (16 kHz, 16 bit) ---
#define ORBITA_CHANNELS          2                                   // mics L y R
#define ORBITA_BYTES_PER_SAMPLE  2
#define ORBITA_FRAME_BYTES       (ORBITA_CHANNELS * ORBITA_BYTES_PER_SAMPLE)   // un frame = una muestra por canal
#define ORBITA_CHUNK_FRAMES      1024                                // 64 ms
#define ORBITA_CHUNK_BYTES       (ORBITA_CHUNK_FRAMES * ORBITA_FRAME_BYTES)    // 4096 B

// Audio que va al servidor: salida del beamformer (hipercardioide, ver beamforming.h). Va en el campo
// `source` del hello. La wake word, en cambio, usa el mic de adelante solo (omnidireccional).
#define ORBITA_RAW_MIC_SOURCE "bf"

/** Muestra k del mic de adelante en un buffer estereo L,R,L,R... El slot sale de ORBITA_BF_ADELANTE_ES_L:
 *  si se monta al reves, la wake word y el beamformer cambian juntos. No se usa (L+R)/2: con los mics a
 *  21,4 mm el promedio hace un hueco en los agudos que depende de la direccion (atras: 4,3 kHz). */
static inline int16_t orbita_mic_adelante(const int16_t *estereo, size_t k)
{
    return estereo[2 * k + (ORBITA_BF_ADELANTE_ES_L ? 0 : 1)];
}

/**
 * @brief   Callback por cada chunk capturado.
 *
 * @details Se llama desde la tarea de captura (core 1, prioridad 6, stack de 4 KB). Tiene que
 *          ser rapido y no bloquear: lo normal es copiar el chunk a un ring y volver. Nada de
 *          snprintf, red ni logs largos. `chunk_ini` apunta a un buffer que se reescribe en la
 *          vuelta siguiente: solo vale durante la llamada.
 *
 * @param [in]  chunk_ini   primer frame del chunk (estereo L,R,L,R...)
 * @param [in]  frames      frames del chunk (siempre ORBITA_CHUNK_FRAMES)
 * @param [in]  hueco       true si antes de este chunk se perdio audio (lectura I2S fallida o
 *                          overflow del DMA): el audio NO es continuo con el chunk anterior
 */
typedef void (*orbita_crudo_cb_t)(const int16_t *chunk_ini, size_t frames, bool hueco);

typedef struct {
    orbita_crudo_cb_t on_crudo;      // puede ser NULL
} orbita_pipeline_cfg_t;

/** Contadores acumulados desde el arranque: para medir una toma, restar dos lecturas (uint32_t sin signo). */
typedef struct {
    uint32_t i2s_errores;            // lecturas del I2S que fallaron o vinieron incompletas
    uint32_t chunks_pausados;        // chunks no entregados por la pausa (parlante sonando + cola)
    uint32_t dma_overflows;          // veces que el DMA del I2S piso audio sin leer (hueco real en la captura)
    uint32_t overruns_ring_in;       // chunks que no entraron al ring in (feed no sigue el ritmo)
    uint32_t chunks_invalidos;       // chunks del ring in con tamano inesperado
    uint32_t bloques_mono;           // bloques mono de 512 armados por feed (2 por chunk)
    uint32_t ring_out_errores;       // escrituras al ring out rechazadas (argumento invalido: no deberia pasar)
    // Tiempos MAXIMOS desde el ultimo orbita_pipeline_reset_tiempos() (o el arranque), en us. NO se restan entre lecturas.
    uint32_t max_captura_us;         // captura: una vuelta sin la lectura I2S (presupuesto: < 56 000 = 120 - 64 ms)
    uint32_t max_ring_in_send_us;    // captura: envio del chunk al ring in
    uint32_t max_on_crudo_us;        // captura: callback on_crudo (ya cuenta dentro de max_captura_us)
    uint32_t ring_in_max_items;      // chunks esperando en el ring in, maximo (capacidad 8; si sube, feed se atrasa)
    uint32_t max_feed_us;            // feed: una vuelta sin la espera, beamformer incluido (presupuesto: < 64 000)
    uint32_t max_ring_out_write_us;  // feed: ring_out_write
    uint32_t stack_libre_captura;    // bytes minimos libres de stack (0 si la tarea no existe)
    uint32_t stack_libre_feed;
} orbita_pipeline_stats_t;

/**
 * @brief   Arranca el pipeline: reserva el ring out y crea las tareas de captura y feed.
 *
 * @details Antes hay que haber llamado a orbita_audio_i2s_init(). Llamar UNA vez: una segunda
 *          llamada devuelve ESP_ERR_INVALID_STATE. Si start() falla a mitad, pueden quedar
 *          tareas y memoria creadas: el llamador debe reiniciar el equipo (no reintentar).
 *          La captura corre en el core 1, prioridad 6; feed, core 1, prioridad 5.
 *
 * @param [in]  cfg  callbacks (copiados); puede ser NULL
 *
 * @return  ESP_OK, ESP_ERR_INVALID_STATE si ya se arranco, ESP_ERR_NO_MEM si falto memoria, o el
 *          error de orbita_ring_out_init().
 */
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg);

/**
 * @brief   Pausa la entrega de audio: mientras `pausa` es true, la captura sigue leyendo el I2S pero
 *          no pone nada en ningun ring ni llama a on_crudo. Al soltarla sigue pausada ~320 ms mas
 *          (cola del parlante) y el primer chunk llega a on_crudo marcado como hueco.
 *
 * @details Para el parlante sin AEC: lo llama el playback al empezar y al terminar de sonar. Es
 *          rapida y no bloquea (solo escribe una variable): se puede llamar desde un callback.
 */
void orbita_pipeline_pausar(bool pausa);

/** Pone en 0 los tiempos maximos (el arranque ensucia el maximo): llamar al empezar una medicion. */
void orbita_pipeline_reset_tiempos(void);

/** Copia los contadores actuales (ver orbita_pipeline_stats_t). */
void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out);
