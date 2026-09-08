#include <string.h>
#include "wav_writer.h"

void wav_build_header(uint8_t *header_out, uint32_t sample_rate,
                       uint16_t num_channels, uint16_t bits_per_sample,
                       uint32_t data_size)
{
    // --- Chunk RIFF ---
    memcpy(&header_out[0], "RIFF", 4);

    uint32_t chunk_size = 36 + data_size;
    header_out[4] = (uint8_t)(chunk_size & 0xFF);
    header_out[5] = (uint8_t)((chunk_size >> 8) & 0xFF);
    header_out[6] = (uint8_t)((chunk_size >> 16) & 0xFF);
    header_out[7] = (uint8_t)((chunk_size >> 24) & 0xFF);

    memcpy(&header_out[8], "WAVE", 4);

    // --- Subchunk1: fmt ---
    memcpy(&header_out[12], "fmt ", 4);

    uint32_t subchunk1_size = 16;
    header_out[16] = (uint8_t)(subchunk1_size & 0xFF);
    header_out[17] = (uint8_t)((subchunk1_size >> 8) & 0xFF);
    header_out[18] = (uint8_t)((subchunk1_size >> 16) & 0xFF);
    header_out[19] = (uint8_t)((subchunk1_size >> 24) & 0xFF);

    uint16_t audio_format = 1; // PCM
    header_out[20] = (uint8_t)(audio_format & 0xFF);
    header_out[21] = (uint8_t)((audio_format >> 8) & 0xFF);

    header_out[22] = (uint8_t)(num_channels & 0xFF);
    header_out[23] = (uint8_t)((num_channels >> 8) & 0xFF);

    header_out[24] = (uint8_t)(sample_rate & 0xFF);
    header_out[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    header_out[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    header_out[27] = (uint8_t)((sample_rate >> 24) & 0xFF);

    uint16_t block_align = num_channels * (bits_per_sample / 8);
    uint32_t byte_rate = sample_rate * block_align;
    header_out[28] = (uint8_t)(byte_rate & 0xFF);
    header_out[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    header_out[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    header_out[31] = (uint8_t)((byte_rate >> 24) & 0xFF);

    header_out[32] = (uint8_t)(block_align & 0xFF);
    header_out[33] = (uint8_t)((block_align >> 8) & 0xFF);

    header_out[34] = (uint8_t)(bits_per_sample & 0xFF);
    header_out[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);

    // --- Subchunk2: data ---
    memcpy(&header_out[36], "data", 4);

    header_out[40] = (uint8_t)(data_size & 0xFF);
    header_out[41] = (uint8_t)((data_size >> 8) & 0xFF);
    header_out[42] = (uint8_t)((data_size >> 16) & 0xFF);
    header_out[43] = (uint8_t)((data_size >> 24) & 0xFF);
}

size_t wav_pack_block_24bit(const int32_t *in_buf, size_t frame_count,
                             uint8_t *out_buf)
{
    size_t frames_read = 0;
    size_t offset_in = 0;
    size_t offset_out = 0;

    while (frames_read < frame_count)
    {   
        out_buf[offset_out] = (uint8_t)((in_buf[offset_in] >> 8) & 0xFF);
        out_buf[offset_out+1] = (uint8_t)((in_buf[offset_in] >> 16) & 0xFF);
        out_buf[offset_out+2] = (uint8_t)((in_buf[offset_in] >> 24) & 0xFF);
        offset_in++;
        offset_out += 3;
        if (offset_out % 6 == 0) frames_read++; // Cada 6 bytes subo un frame
    }
    
    return frames_read*2*3; // 2 muestras por frame, de 3 bytes c/u
}
