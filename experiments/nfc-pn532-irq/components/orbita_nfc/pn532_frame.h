#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Solo tramas normales y acotadas del PN532. No son registros I2C.
size_t pn532_encode(const uint8_t *command, size_t length, uint8_t *out, size_t capacity);
bool pn532_decode(const uint8_t *frame, size_t available, uint8_t command,
                  const uint8_t **payload, size_t *length);
bool pn532_uid(const uint8_t *payload, size_t length, uint8_t uid[10], uint8_t *uid_len);
