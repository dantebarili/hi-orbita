// Prueba nativa: compilar junto con audio_ws_wire.c (sin ESP-IDF).
#include "audio_ws_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    const uint8_t known[] = "123456789";
    assert(orbita_wire_crc(0, known, 9) == 0xcbf43926u);
    assert(orbita_wire_crc(orbita_wire_crc(0, known, 4), known+4, 5) == 0xcbf43926u);
    int16_t audio[2048];
    for (int i=0; i<2048; ++i) audio[i] = (int16_t)(i-1024);
    uint8_t packet[4137]; memset(packet, 0xa5, sizeof(packet));
    orbita_wire_meta_t meta = {.stream_id=0x0123456789abcdefULL, .slots=4, .samples=2048};
    assert(orbita_wire_encode(packet, 4135, &meta, audio) == 0);
    assert(packet[0] == 0xa5);
    size_t n = orbita_wire_encode(packet, 4136, &meta, audio);
    assert(n == 4136 && packet[4136] == 0xa5);
    assert(packet[40] == 0 && packet[41] == 0xfc); // -1024 little endian
    if (argc == 2) {
        FILE *f = fopen(argv[1], "wb"); assert(f);
        assert(fwrite(packet, 1, n, f) == n); assert(fclose(f) == 0);
    }
    meta.samples = 2049;
    assert(orbita_wire_encode(packet, sizeof(packet), &meta, audio) == 0);
    meta.samples = 1; meta.vad = 0; meta.voice = 1;
    assert(orbita_wire_encode(packet, sizeof(packet), &meta, audio) == 0);
    puts("wire C OK");
    return 0;
}
