#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Solo tramas normales y acotadas del PN532. No son registros I2C.
size_t pn532_encode(const uint8_t *command, size_t length, uint8_t *out, size_t capacity);
bool pn532_decode(const uint8_t *frame, size_t available, uint8_t command,
                  const uint8_t **payload, size_t *length);
bool pn532_uid(const uint8_t *payload, size_t length, uint8_t uid[10], uint8_t *uid_len);
// Respuesta de InAutoPoll (UM0701-02 §7.3.13): NbTg [Type Ln AutoPollTargetData]... Toma el primer objetivo
// tipo A de 106 kbps (Type 0x10 Mifare o 0x20 ISO14443-4A). false = respuesta mal formada; *found = false si
// NbTg = 0 (se agotaron los intentos sin tarjeta).
bool pn532_autopoll_uid(const uint8_t *payload, size_t length, bool *found, uint8_t uid[10], uint8_t *uid_len);
