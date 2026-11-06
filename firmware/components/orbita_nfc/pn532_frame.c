#include "pn532_frame.h"
#include <string.h>

size_t pn532_encode(const uint8_t *cmd, size_t n, uint8_t *out, size_t cap)
{
    if (!cmd || !out || !n || n > 32 || cap < n + 8) return 0;
    out[0] = 0; out[1] = 0; out[2] = 0xff;
    out[3] = (uint8_t)(n + 1); out[4] = (uint8_t)(0 - out[3]);
    out[5] = 0xd4;
    memcpy(out + 6, cmd, n);
    uint8_t sum = 0xd4;
    for (size_t i = 0; i < n; i++) sum += cmd[i];
    out[n + 6] = (uint8_t)(0 - sum); out[n + 7] = 0;
    return n + 8;
}

bool pn532_decode(const uint8_t *f, size_t available, uint8_t cmd,
                  const uint8_t **payload, size_t *length)
{
    if (!f || !payload || !length || available < 9) return false;
    size_t n = f[3];
    if (f[0] || f[1] || f[2] != 0xff || n < 2 || n == 0xff ||
        (uint8_t)(f[3] + f[4]) != 0 || n + 7 > available) return false;
    if (f[5] != 0xd5 || f[6] != (uint8_t)(cmd + 1) || f[n + 6] != 0) return false;
    uint8_t sum = 0;
    for (size_t i = 5; i <= n + 5; i++) sum += f[i];
    if (sum) return false;
    *payload = f + 7; *length = n - 2;
    return true;
}

bool pn532_uid(const uint8_t *p, size_t n, uint8_t uid[10], uint8_t *uid_len)
{
    // InListPassiveTarget, Type A: NbTg,Tg,SENS_RES[2],SEL_RES,NFCIDLength,NFCID.
    if (!p || !uid || !uid_len || n < 6 || p[0] != 1 || p[1] != 1) return false;
    uint8_t len = p[5];
    if ((len != 4 && len != 7 && len != 10) || n < (size_t)6 + len) return false;
    memcpy(uid, p + 6, len); *uid_len = len;
    return true;
}

bool pn532_autopoll_uid(const uint8_t *p, size_t n, bool *found, uint8_t uid[10], uint8_t *uid_len)
{
    // NbTg, Type, Ln, TargetData = Tg, SENS_RES[2], SEL_RES, NFCIDLength, NFCID (igual que InListPassiveTarget).
    if (!p || !found || !uid || !uid_len || n < 1 || p[0] > 2) return false;
    *found = false;
    if (p[0] == 0) return n == 1;
    if (n < 3) return false;
    uint8_t type = p[1], ln = p[2];
    const uint8_t *d = p + 3;
    if ((type != 0x10 && type != 0x20) || ln < 6 || (size_t)3 + ln > n) return false;
    uint8_t len = d[4];
    if ((len != 4 && len != 7 && len != 10) || (size_t)5 + len > ln) return false;
    memcpy(uid, d + 5, len); *uid_len = len;
    *found = true;
    return true;
}
