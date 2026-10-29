/**
 * @file    audio_ring_out.h
 * @brief   Ring out: slots de audio mono con header interno.
 * @details Formato y politicas en orbita-obsidian/arquitectura.md §3.4. v1: cola FIFO de
 *          slots en PSRAM, sin pre-roll ni tabla de segmentos (A6).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// --- Constantes del ring ---
#define ORBITA_RING_VERSION      1                                    // Para comprobar que el sink y el ring comparten el header
#define ORBITA_SLOT_SAMPLES      512                                  // 32 ms mono a 16 kHz (= un fetch del AFE)
#define ORBITA_SLOT_AUDIO_BYTES  (ORBITA_SLOT_SAMPLES * 2)            // 1024 B
#define ORBITA_RING_SLOTS        1024                                 // ~32,8 s (32 ms por slot), 1,04 MB en PSRAM

// Bits de `flags`. Con los dos en 0 (sin VAD) la compuerta queda ABIERTA: se manda igual.
#define ORBITA_FLAG_VAD_OK       (1u << 0)   // el VAD midio este bloque (sin AFE no hay VAD)
#define ORBITA_FLAG_VOZ          (1u << 1)   // el VAD detecto voz (solo vale si VAD_OK)

/* HEADER: sirve para clasificar slots
 *         NO es el header del dato q se envia al sink, el sink lo arma a partir de este.
 *  seq:          nº de slot, lo asigna el ring (sube de a 1 en cada write)
 *  lost_samples: muestras perdidas justo antes de este slot (captura + descartes del ring)
 *  n_samples:    muestras de audio validas (<= ORBITA_SLOT_SAMPLES)
 *  flags:        ORBITA_FLAG_*
 *  version:      ORBITA_RING_VERSION */
typedef struct {
    uint32_t seq;            // nº de slot, sube de a 1 (comparar con (int32_t)(a - b))
    uint32_t lost_samples;   // muestras perdidas justo antes de este slot (0 = ninguna)
    uint16_t n_samples;      // muestras de audio validas en este slot
    uint8_t  flags;          // ORBITA_FLAG_*
    uint8_t  version;        // ORBITA_RING_VERSION
    uint32_t reservado;      // futuro
} orbita_slot_hdr_t;

_Static_assert(sizeof(orbita_slot_hdr_t) == 16, "el header del slot debe medir 16 B");

/* SLOT: porcion de audio lista para ser enviada, con un header que describe su contenido.
 *       1 slot = 512 muestras mono = 32ms */
typedef struct {
    orbita_slot_hdr_t hdr;
    int16_t           audio[ORBITA_SLOT_SAMPLES];
} orbita_slot_t;

_Static_assert(sizeof(orbita_slot_t) == 16 + ORBITA_SLOT_AUDIO_BYTES, "el slot debe medir 1040 B");

/** Estado del ring para medir (A12). `max_ocupados` sirve para dimensionar ORBITA_RING_SLOTS. */
typedef struct {
    uint16_t ocupados;       // slots esperando al sink ahora
    uint16_t max_ocupados;   // maximo desde el arranque
    uint32_t escritos;       // slots escritos con exito
    uint32_t descartados;    // slots pisados por ring lleno (el sink no vacia a tiempo)
} orbita_ring_out_stats_t;

/**
 * @brief   Reserva los ORBITA_RING_SLOTS slots en PSRAM (~1 MB). Llamar UNA vez.
 *
 * @details ORDEN DE ARRANQUE: (1) antes de crear las tareas que usan el ring (productor y sink): no
 *          esta pensado para que `init` corra en paralelo con `write`/`read`; (2) antes de iniciar el AFE:
 *          el bloque es contiguo y, si `esp-sr` ya fragmento la PSRAM, puede fallar.
 *
 * @return  ESP_OK; ESP_ERR_INVALID_STATE si ya estaba iniciado; ESP_ERR_NO_MEM si no hay PSRAM.
 */
esp_err_t orbita_ring_out_init(void);

/**
 * @brief   Agrega un slot al final. No espera a nadie: ~20-50 us con interrupciones apagadas (spinlock).
 *
 * @details Lo llama el productor (feed o fetch, core 1, prio 5). El ring completa `seq`, `version`
 *          y copia el audio. Si el ring esta lleno descarta el slot MAS VIEJO (sube `descartados`) y
 *          suma sus muestras a `lost_samples` del que pasa a ser el mas viejo, para que el sink vea el hueco
 *          (tambien por el salto de `seq`).
 *
 * @param [in]  audio         muestras mono 16 bit; se copian, el buffer puede reusarse al volver
 * @param [in]  n_samples     1..ORBITA_SLOT_SAMPLES (el resto del slot queda sin definir)
 * @param [in]  flags         ORBITA_FLAG_* (0 = sin VAD, compuerta abierta)
 * @param [in]  lost_samples  muestras perdidas justo antes de este bloque (contadores de la captura)
 *
 * @return  ESP_OK; ESP_ERR_INVALID_STATE sin init; ESP_ERR_INVALID_ARG.
 */
esp_err_t orbita_ring_out_write(const int16_t *audio, uint16_t n_samples, uint8_t flags, uint32_t lost_samples);

/**
 * @brief   Saca el slot MAS VIEJO y lo copia a `dst` (el ring lo libera: no se puede releer).
 *
 * @details No bloquea: si esta vacio devuelve ESP_ERR_NOT_FOUND y el sink espera (p. ej. `vTaskDelay`)
 *          y reintenta. Copia SOLO el header y `hdr.n_samples` muestras: el resto de `dst` queda como
 *          estaba (es audio viejo), asi que el sink debe mandar siempre `2 * hdr.n_samples` bytes de audio,
 *          nunca 1024. `dst` es del sink (1 KB: memoria estatica o heap, no un stack chico). Para detectar
 *          perdidas el sink mira `hdr.lost_samples` y los saltos de `hdr.seq`.
 *
 * @param [out]  dst  slot de destino (1040 B)
 *
 * @return  ESP_OK; ESP_ERR_NOT_FOUND vacio; ESP_ERR_INVALID_STATE sin init; ESP_ERR_INVALID_ARG.
 */
esp_err_t orbita_ring_out_read(orbita_slot_t *dst);

/**
 * @brief   Descarta los slots viejos y deja solo los ultimos `n` (el pre-roll de una conversacion).
 *
 * @details El descarte es a proposito, no una perdida: el primer slot que queda arranca con
 *          `lost_samples` = 0. No cuenta en `descartados`.
 *
 * @return  slots descartados.
 */
uint16_t orbita_ring_out_quedarse_con(uint16_t n);

/** Copia el estado actual del ring a `out`. */
void orbita_ring_out_get_stats(orbita_ring_out_stats_t *out);
