#pragma once
#include <stddef.h>
#include <stdint.h>

// Protocolo de laboratorio v1, no layout de una struct de C. Todo little-endian.
#define ORBITA_WIRE_HEADER 40u
#define ORBITA_WIRE_MAX_SAMPLES 2048u
typedef struct {
    uint64_t stream_id;
    uint32_t index, seq, lost;
    uint16_t slots, samples, voice, vad;
} orbita_wire_meta_t;

uint32_t orbita_wire_crc(uint32_t previous, const uint8_t *data, size_t size);
size_t orbita_wire_encode(uint8_t *dst, size_t capacity,
                         const orbita_wire_meta_t *meta, const int16_t *audio);
