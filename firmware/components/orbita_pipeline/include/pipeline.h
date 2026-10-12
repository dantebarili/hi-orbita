/**
 * @file    pipeline.h
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Orbita
 * @brief   Pipeline de audio: captura I2S en tarea propia y entrega de chunks a quien los consuma.
 * @version 0.1.0
 * @date    2026-10-12
 *
 * @copyright Copyright (c) 2026 - Proyecto Orbita
 *
 * @details La tarea de captura lee el I2S de a ORBITA_CHUNK_FRAMES frames (64 ms), los pasa a
 *          16 bit y llama al callback `on_chunk`. Quien consume (hoy el modulo de test de
 *          main/, manana el sink de sockets) se engancha ahi y no toca la captura.
 *          Opcional (ORBITA_PIPELINE_USA_AFE=1): ring 1 -> tarea feed -> AFE -> tarea fetch,
 *          que entrega la salida procesada por `on_afe_block`. Hoy esta en pausa (ver plan).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// --- Formato del audio (16 kHz, 16 bit) ---
#define ORBITA_CHANNELS          2                                   // mics L y R
#define ORBITA_BYTES_PER_SAMPLE  2
#define ORBITA_FRAME_BYTES       (ORBITA_CHANNELS * ORBITA_BYTES_PER_SAMPLE)   // un frame = una muestra por canal
#define ORBITA_CHUNK_FRAMES      1024                                // 64 ms; es el chunk de feed del AFE (con 2 mics + BSS)
#define ORBITA_CHUNK_BYTES       (ORBITA_CHUNK_FRAMES * ORBITA_FRAME_BYTES)    // 4096 B
#define ORBITA_AFE_FETCH_FRAMES  512                                 // salida del AFE: 32 ms, mono
#define ORBITA_AFE_FETCH_BYTES   (ORBITA_AFE_FETCH_FRAMES * ORBITA_BYTES_PER_SAMPLE)

// 1: se crea el AFE y sus tareas feed/fetch; 0: sin AFE (hoy). Se puede pisar con -D.
#ifndef ORBITA_PIPELINE_USA_AFE
#define ORBITA_PIPELINE_USA_AFE  0
#endif

/**
 * @brief  Callback por cada chunk capturado.
 *
 * @details Se llama desde la tarea de captura (core 1, prioridad alta): tiene que ser rapido y
 *          NO bloquear (nada de esperas largas, logs ni UART). Si hay que conservar el audio, se
 *          copia aca y sin esperar. `estereo` vale solo durante la llamada.
 *
 * @param [in]  estereo  ORBITA_CHUNK_FRAMES frames int16 entrelazados (L R L R ...)
 * @param [in]  frames   frames del chunk (siempre ORBITA_CHUNK_FRAMES)
 */
typedef void (*orbita_chunk_cb_t)(const int16_t *estereo, size_t frames);

/**
 * @brief  Callback por cada bloque que entrega el AFE (solo con ORBITA_PIPELINE_USA_AFE=1).
 *
 * @details Se llama desde la tarea fetch. Mismas reglas que orbita_chunk_cb_t: rapido, sin
 *          bloquear (si fetch se atrasa, el AFE sobrescribe audio sin avisar).
 *
 * @param [in]  mono      ORBITA_AFE_FETCH_FRAMES muestras int16; memoria del AFE, vale solo durante la llamada
 * @param [in]  muestras  cantidad de muestras
 * @param [in]  voz       true si el VAD marca voz en este bloque
 */
typedef void (*orbita_afe_cb_t)(const int16_t *mono, size_t muestras, bool voz);

typedef struct {
    orbita_chunk_cb_t on_chunk;      // puede ser NULL
    orbita_afe_cb_t   on_afe_block;  // puede ser NULL; solo se usa con el AFE
} orbita_pipeline_cfg_t;

/** Contadores acumulados desde el arranque: para medir una toma, restar dos lecturas (uint32_t sin signo). */
typedef struct {
    uint32_t i2s_errores;            // lecturas del I2S que fallaron o vinieron incompletas
    uint32_t overruns_ring1;         // chunks que no entraron al ring 1 (feed no sigue el ritmo)
    uint32_t chunks_alimentados;     // chunks entregados al AFE
    uint32_t chunks_invalidos;       // chunks del ring 1 con tamano inesperado
    uint32_t chunks_recibidos;       // fetch que devolvieron audio valido
    uint32_t fetch_vacios;           // fetch sin audio (vencio el tiempo)
    uint32_t fetch_invalidos;        // fetch con tamano inesperado
    uint32_t bloques_voz;            // fetch que el VAD marco como voz
    uint32_t stack_libre_captura;    // bytes minimos libres de stack (0 si la tarea no existe)
    uint32_t stack_libre_feed;
    uint32_t stack_libre_fetch;
} orbita_pipeline_stats_t;

/**
 * @brief  Arranca el pipeline: crea la tarea de captura (y el AFE con sus tareas si esta activado).
 *
 * @details Antes hay que haber llamado a orbita_audio_i2s_init(). Llamar UNA vez.
 *          La captura corre en el core 1, prioridad 6; feed y fetch, core 1, prioridad 5.
 *
 * @param [in]  cfg  callbacks (copiados); puede ser NULL
 *
 * @return  ESP_OK, ESP_ERR_INVALID_SIZE si el AFE pide otro chunk que el configurado,
 *          ESP_ERR_NO_MEM si falto memoria, o el error de orbita_afe_init().
 */
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg);

/** Copia los contadores actuales (ver orbita_pipeline_stats_t). */
void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out);
