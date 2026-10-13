#include "audio_ws_wire.h"
#include <string.h>

static void le(uint8_t *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(value >> (8*i));
}

// Mismo CRC32 que zlib.crc32: sirve para comparar el PCM enviado y guardado.
uint32_t orbita_wire_crc(uint32_t previous, const uint8_t *data, size_t size)
{
    uint32_t crc = ~previous;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

size_t orbita_wire_encode(uint8_t *dst, size_t capacity,
                         const orbita_wire_meta_t *m, const int16_t *audio)
{
    if (!dst || !m || !audio || !m->samples || m->samples > ORBITA_WIRE_MAX_SAMPLES ||
        !m->slots || m->slots > 4 || m->samples > m->slots * 512 ||
        m->voice > m->vad || m->vad > m->slots ||
        capacity < ORBITA_WIRE_HEADER + 2u*m->samples) return 0;
    memset(dst, 0, ORBITA_WIRE_HEADER);
    memcpy(dst, "ORBA", 4);
    dst[4] = 1;
    dst[5] = ORBITA_WIRE_HEADER;
    le(dst+8, m->stream_id, 8);
    le(dst+16, m->index, 4);
    le(dst+20, m->seq, 4);
    le(dst+24, m->lost, 4);
    le(dst+28, m->slots, 2);
    le(dst+30, m->samples, 2);
    le(dst+32, m->voice, 2);
    le(dst+34, m->vad, 2);
    for (unsigned i = 0; i < m->samples; ++i)
        le(dst+ORBITA_WIRE_HEADER+2*i, (uint16_t)audio[i], 2);
    le(dst+36, orbita_wire_crc(0, dst+ORBITA_WIRE_HEADER, 2u*m->samples), 4);
    return ORBITA_WIRE_HEADER + 2u*m->samples;
}
