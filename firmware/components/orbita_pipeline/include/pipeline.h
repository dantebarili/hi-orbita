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
 *          16 bit y llama al callback `on_crudo`. Quien consume (hoy el modulo de test de
 *          main/, manana el sink de sockets) se engancha ahi y no toca la captura.
 *          Opcional (CONFIG_ORBITA_PIPELINE_USA_AFE, en menuconfig): ring in -> tarea feed -> AFE -> tarea fetch,
 *          que entrega la salida procesada por `on_afe`. Hoy esta en pausa (ver plan).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

// --- Formato del audio (16 kHz, 16 bit) ---
#define ORBITA_CHANNELS          2                                   // mics L y R
#define ORBITA_BYTES_PER_SAMPLE  2
#define ORBITA_FRAME_BYTES       (ORBITA_CHANNELS * ORBITA_BYTES_PER_SAMPLE)   // un frame = una muestra por canal
#define ORBITA_CHUNK_FRAMES      1024                                // 64 ms; multiplo del feed del AFE (512 sin BSS, 1024 con BSS)
#define ORBITA_CHUNK_BYTES       (ORBITA_CHUNK_FRAMES * ORBITA_FRAME_BYTES)    // 4096 B
#define ORBITA_AFE_FETCH_FRAMES  512                                 // salida del AFE: 32 ms, mono
#define ORBITA_AFE_FETCH_BYTES   (ORBITA_AFE_FETCH_FRAMES * ORBITA_BYTES_PER_SAMPLE)

// 1: se crea el AFE y sus tareas feed/fetch; 0: sin AFE. Se elige con `idf.py menuconfig`
// (Orbita pipeline -> Usar el AFE): vale igual en todos los componentes.
#ifdef CONFIG_ORBITA_PIPELINE_USA_AFE
#define ORBITA_PIPELINE_USA_AFE  1
#else
#define ORBITA_PIPELINE_USA_AFE  0
#endif

// Audio crudo mono = promedio de los dos mics (fijo). Lo usan el ring mono (sin AFE), la wake word
// y el campo source del protocolo. Ojo: a 50 mm, una voz de costado pierde algo de agudos (~3,4 kHz).
#define ORBITA_RAW_MIC_SOURCE "mic_lr"

/** Muestra mono k de un buffer estereo L,R,L,R...: (L + R) / 2, sumado en 32 bit para no desbordar. */
static inline int16_t orbita_mono_lr(const int16_t *estereo, size_t k)
{
    return (int16_t)(((int32_t)estereo[2 * k] + estereo[2 * k + 1]) / 2);
}

/**
 * @brief   Callback por cada chunk capturado.
 *
 * @details Se llama desde la tarea de captura (core 1, prioridad 6, stack de 4 KB). Tiene que
 *          ser rapido y no bloquear: lo normal es copiar el chunk a un ring y volver. Nada de
 *          snprintf, red ni logs largos. `chunk_ini` apunta a un buffer que se reescribe en la
 *          vuelta siguiente: solo vale durante la llamada.
 *
 * @param [in]  chunk_ini   primer frame del chunk
 * @param [in]  frames      frames del chunk (siempre ORBITA_CHUNK_FRAMES)
 * @param [in]  hueco       true si antes de este chunk se perdio audio (lectura I2S fallida o
 *                          overflow del DMA): el audio NO es continuo con el chunk anterior
 */
typedef void (*orbita_crudo_cb_t)(const int16_t *chunk_ini, size_t frames, bool hueco);

/**
 * @brief   Callback por cada bloque que entrega el AFE.
 *
 * @details Se llama desde la tarea fetch. Mismas reglas que orbita_crudo_cb_t: rapido, sin
 *          bloquear (si fetch se atrasa, el AFE sobrescribe audio sin avisar). Aca no hay
 *          indicador de hueco: esa perdida no se puede detectar (ver pipeline.c, tarea_fetch).
 *
 * @param [in]  mono_ini    primer frame del bloque de salida del AFE
 * @param [in]  muestras    cantidad de muestras
 * @param [in]  voz         true si el VAD marca voz en este bloque
 */
typedef void (*orbita_afe_cb_t)(const int16_t *mono_ini, size_t muestras, bool voz);

typedef struct {
    orbita_crudo_cb_t on_crudo;      // puede ser NULL
    orbita_afe_cb_t   on_afe;        // puede ser NULL; solo se usa con el AFE
} orbita_pipeline_cfg_t;

/** Contadores acumulados desde el arranque: para medir una toma, restar dos lecturas (uint32_t sin signo). */
typedef struct {
    uint32_t i2s_errores;            // lecturas del I2S que fallaron o vinieron incompletas
    uint32_t dma_overflows;          // veces que el DMA del I2S pisó audio sin leer (hueco real en la captura)
    uint32_t overruns_ring_in;         // chunks que no entraron al ring in (feed no sigue el ritmo)
    uint32_t chunks_alimentados;     // chunks entregados al AFE
    uint32_t chunks_invalidos;       // chunks del ring in con tamano inesperado
    uint32_t bloques_mono;           // bloques mono de 512 armados por feed (sin AFE; 2 por chunk)
    uint32_t ring_out_errores;       // escrituras al ring out rechazadas (argumento invalido: no deberia pasar)
    uint32_t chunks_recibidos;       // fetch que devolvieron audio valido
    uint32_t fetch_vacios;           // fetch sin audio (vencio el tiempo)
    uint32_t fetch_invalidos;        // fetch con tamano inesperado
    uint32_t bloques_voz;            // fetch que el VAD marco como voz
    // Tiempos MAXIMOS desde el ultimo orbita_pipeline_reset_tiempos() (o el arranque), en us. NO se restan entre lecturas.
    uint32_t max_captura_us;         // captura: una vuelta sin la lectura I2S (presupuesto: < 56 000 = 120 - 64 ms)
    uint32_t max_ring_in_send_us;    // captura: envio del chunk al ring in
    uint32_t max_on_crudo_us;        // captura: callback on_crudo (ya cuenta dentro de max_captura_us)
    uint32_t ring_in_max_items;      // chunks esperando en el ring in, maximo (capacidad 8; si sube, feed se atrasa)
    uint32_t max_feed_us;            // feed: una vuelta sin la espera (presupuesto: < 64 000; con AFE incluye afe_feed ~20 000)
    uint32_t max_ring_out_write_us;  // ring_out_write, desde feed (sin AFE) o fetch (con AFE)
    uint32_t max_fetch_us;           // fetch: una vuelta sin la espera del AFE (solo AFE; presupuesto: < 32 000)
    uint32_t max_on_afe_us;          // fetch: callback on_afe (solo AFE)
    uint32_t stack_libre_captura;    // bytes minimos libres de stack (0 si la tarea no existe)
    uint32_t stack_libre_feed;
    uint32_t stack_libre_fetch;
} orbita_pipeline_stats_t;

/**
 * @brief   Arranca el pipeline: crea la tarea de captura (y/o el AFE con sus tareas si esta activado).
 *
 * @details Antes hay que haber llamado a orbita_audio_i2s_init(). Llamar UNA vez: una segunda
 *          llamada devuelve ESP_ERR_INVALID_STATE. Si start() falla a mitad, pueden quedar
 *          tareas y memoria creadas: el llamador debe reiniciar el equipo (no reintentar).
 *          La captura corre en el core 1, prioridad 6; feed y fetch, core 1, prioridad 5.
 *
 * @param [in]  cfg  callbacks (copiados); puede ser NULL
 *
 * @return  ESP_OK, ESP_ERR_INVALID_STATE si ya se arranco, ESP_ERR_INVALID_SIZE si el AFE pide
 *          otro chunk que el configurado, ESP_ERR_NO_MEM si falto memoria, o el error de
 *          orbita_afe_init().
 */
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg);

/** Pone en 0 los tiempos maximos (el arranque ensucia el maximo): llamar al empezar una medicion. */
void orbita_pipeline_reset_tiempos(void);

/** Copia los contadores actuales (ver orbita_pipeline_stats_t). */
void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out);
