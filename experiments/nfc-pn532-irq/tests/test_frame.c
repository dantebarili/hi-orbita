#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pn532_frame.h"

static size_t response(uint8_t cmd, const uint8_t *p, size_t n, uint8_t f[64])
{
    memset(f, 0, 64); f[2] = 0xff;
    f[3] = (uint8_t)(n + 2); f[4] = (uint8_t)(0 - f[3]);
    f[5] = 0xd5; f[6] = (uint8_t)(cmd + 1);
    memcpy(f + 7, p, n);
    uint8_t sum = 0;
    for (size_t i = 5; i < n + 7; i++) sum += f[i];
    f[n + 7] = (uint8_t)(0 - sum);
    return n + 9;
}

int main(void)
{
    const uint8_t cmd[] = {2}, expected[] = {0,0,0xff,2,0xfe,0xd4,2,0x2a,0};
    uint8_t f[64];
    assert(pn532_encode(cmd, 1, f, sizeof(f)) == sizeof(expected));
    assert(!memcmp(f, expected, sizeof(expected)));
    assert(pn532_encode(cmd, 1, f, 8) == 0);
    const uint8_t fw[] = {0x32,1,6,7};
    size_t total = response(2, fw, sizeof(fw), f), n;
    const uint8_t *p;
    assert(pn532_decode(f, total, 2, &p, &n) && n == 4 && !memcmp(p, fw, 4));
    for (size_t i = 0; i < total; i++) assert(!pn532_decode(f, i, 2, &p, &n));
    // Cada byte de cabecera, payload y checksum corrupto debe rechazarse.
    for (size_t i = 0; i < total; i++) {
        f[i] ^= 1; assert(!pn532_decode(f, total, 2, &p, &n)); f[i] ^= 1;
    }
    assert(!pn532_decode(f, total, 0x4a, &p, &n));
    f[3] = 0xff; f[4] = 1; assert(!pn532_decode(f, 64, 2, &p, &n));
    uint8_t data[16] = {1,1,0,4,0,0}, uid[10], length;
    const uint8_t sizes[] = {4,7,10};
    for (size_t i = 0; i < sizeof(sizes); i++) {
        data[5] = sizes[i];
        assert(pn532_uid(data, 6 + sizes[i], uid, &length) && length == sizes[i]);
        assert(!pn532_uid(data, 5 + sizes[i], uid, &length));
    }
    data[5] = 11; assert(!pn532_uid(data, sizeof(data), uid, &length));
    data[5] = 4; data[0] = 2; assert(!pn532_uid(data, sizeof(data), uid, &length));

    // InAutoPoll (UM0701-02 7.3.13): NbTg, Type, Ln, Tg, SENS_RES[2], SEL_RES, NFCIDLength, NFCID.
    bool found;
    const uint8_t nada[] = {0};
    assert(pn532_autopoll_uid(nada, 1, &found, uid, &length) && !found);
    for (size_t i = 0; i < sizeof(sizes); i++) {
        uint8_t ap[3 + 5 + 10] = {1, 0x10, (uint8_t)(5 + sizes[i]), 1, 0, 4, 8, sizes[i]};
        for (uint8_t k = 0; k < sizes[i]; k++) ap[8 + k] = (uint8_t)(0xA0 + k);
        size_t total_ap = 8 + sizes[i];
        assert(pn532_autopoll_uid(ap, total_ap, &found, uid, &length) && found && length == sizes[i]);
        assert(uid[0] == 0xA0 && uid[sizes[i] - 1] == 0xA0 + sizes[i] - 1);
        for (size_t corte = 1; corte < total_ap; corte++)        // truncada en cualquier punto: se rechaza
            assert(!pn532_autopoll_uid(ap, corte, &found, uid, &length));
        ap[1] = 0x20; assert(pn532_autopoll_uid(ap, total_ap, &found, uid, &length) && found);
        ap[1] = 0x40; assert(!pn532_autopoll_uid(ap, total_ap, &found, uid, &length));   // DEP: no se acepta
        ap[1] = 0x10; ap[7] = 5; assert(!pn532_autopoll_uid(ap, total_ap, &found, uid, &length));
    }
    const uint8_t tres[] = {3};
    assert(!pn532_autopoll_uid(tres, 1, &found, uid, &length));
    puts("PASS: tramas, checksums, truncados, respuesta incorrecta, limites de UID e InAutoPoll");
    return 0;
}
