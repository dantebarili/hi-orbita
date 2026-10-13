/**
 * @file    audio_ring_out.h
 * @brief   Ring out: slots de audio mono con header interno.
 * @details Formato y politicas en orbita-obsidian/arquitectura.md §3.4. Por ahora solo el
 *          formato del slot; el ring (cursores, escritura, lectura) viene despues.
 */

#pragma once

#include <stdint.h>

// --- Constantes del ring ---
#define ORBITA_RING_VERSION      1                                    // Para comprobar que el sink y el ring comparten el header
#define ORBITA_SLOT_SAMPLES      512                                  // 32 ms mono a 16 kHz (= un fetch del AFE)
#define ORBITA_SLOT_AUDIO_BYTES  (ORBITA_SLOT_SAMPLES * 2)            // 1024 B
#define ORBITA_RING_SLOTS        313                                  // 10s aprox, 32ms por slot

// Bits de `flags`. Con los dos en 0 (sin VAD) la compuerta queda ABIERTA: se manda igual.
#define ORBITA_FLAG_VAD_OK       (1u << 0)   // el VAD midio este bloque (sin AFE no hay VAD)
#define ORBITA_FLAG_VOZ          (1u << 1)   // el VAD detecto voz (solo vale si VAD_OK)

/* HEADER: sirve para clasificar slots
 *         NO es el header del dato q se envia al sink, el sink lo arma a partir de este.
 *  seq:
 *  lost_samples:
 *  n_samples:
 *  flags:
 *  version: */
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
