/**
 * @file    audio_ring.h
 * @brief   Ring 2: slots de audio mono con header interno (A5 v1).
 * @details Formato y politicas en orbita-obsidian/arquitectura.md §3.4. Por ahora solo el
 *          formato del slot; el ring (cursores, escritura, lectura) viene despues.
 */

#pragma once

#include <stdint.h>

#define ORBITA_RING_VERSION      1
#define ORBITA_SLOT_SAMPLES      512                                  // 32 ms mono a 16 kHz (= un fetch del AFE)
#define ORBITA_SLOT_AUDIO_BYTES  (ORBITA_SLOT_SAMPLES * 2)            // 1024 B

// Bits de `flags`. Con los dos en 0 (sin VAD) la compuerta queda ABIERTA: se manda igual.
#define ORBITA_FLAG_VAD_OK       (1u << 0)   // el VAD midio este bloque (sin AFE no hay VAD)
#define ORBITA_FLAG_VOZ          (1u << 1)   // el VAD detecto voz (solo vale si VAD_OK)

// Header interno de cada slot. NO viaja por el cable: el sink arma el suyo a partir de este.
typedef struct {
    uint32_t seq;            // nº de slot, sube de a 1 (comparar con (int32_t)(a - b))
    uint32_t lost_samples;   // muestras perdidas justo antes de este slot (0 = ninguna)
    uint16_t n_samples;      // muestras de audio validas en este slot
    uint8_t  flags;          // ORBITA_FLAG_*
    uint8_t  version;        // ORBITA_RING_VERSION
    uint32_t reservado;      // futuro
} orbita_slot_hdr_t;

_Static_assert(sizeof(orbita_slot_hdr_t) == 16, "el header del slot debe medir 16 B");

// Un slot completo en memoria: header seguido del audio.
typedef struct {
    orbita_slot_hdr_t hdr;
    int16_t           audio[ORBITA_SLOT_SAMPLES];
} orbita_slot_t;

_Static_assert(sizeof(orbita_slot_t) == 16 + ORBITA_SLOT_AUDIO_BYTES, "el slot debe medir 1040 B");
