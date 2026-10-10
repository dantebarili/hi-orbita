#pragma once

#include <stdint.h>
#include <stddef.h>

// Tamaño fijo del header WAV (RIFF/WAVE/fmt/data), en bytes.
#define WAV_HEADER_SIZE 44
#define WAV_CHANNEL_DATA_SIZE 2 // en bytes, cuando ya haya recortado al final

/**
 * Llena header_out (buffer de WAV_HEADER_SIZE bytes, ya reservado por quien
 * llama) con un header WAV valido para PCM sin comprimir.
 *
 * data_size: cantidad total de bytes de audio YA EMPAQUETADO (24-bit, sin el
 * byte de relleno) que va a ir despues del header — no el tamaño del buffer
 * original de 32-bit.
 */
void wav_build_header(uint8_t *header_out, uint32_t sample_rate,
                       uint16_t num_channels, uint16_t bits_per_sample,
                       uint32_t data_size);

/**
 * Empaqueta un bloque de audio de 32-bit-contenedor (formato en el que sale
 * el I2S/PSRAM, entrelazado L/R) a 24-bit real (3 bytes por muestra,
 * little-endian, sin el byte de relleno bajo).
 *
 * in_buf: bloque de entrada, frame_count frames x 2 canales (int32_t).
 * out_buf: buffer de salida ya reservado por quien llama; tiene que tener
 * espacio para frame_count * 2 * 3 bytes.
 *
 * Devuelve la cantidad de bytes escritos en out_buf.
 */
size_t wav_pack_block_24bit(const int32_t *in_buf, size_t frame_count,
                             uint8_t *out_buf);

/**
 * Igual que wav_pack_block_24bit pero a 16-bit (2 bytes por muestra,
 * little-endian): se queda con los 16 bits mas altos de cada muestra de 32.
 *
 * out_buf: tiene que tener espacio para frame_count * 2 * 2 bytes.
 *
 * Devuelve la cantidad de bytes escritos en out_buf.
 */
size_t wav_pack_block_16bit(const int32_t *in_buf, size_t frame_count,
                             uint8_t *out_buf);
