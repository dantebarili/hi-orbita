#pragma once
#include <stddef.h>
#include <stdint.h>

// Límite por mensaje, no por conexión. 8 KiB alcanza para el ensayo de 4 KiB.
#define ORBITA_WS_MAX_MESSAGE_BYTES 8192

typedef enum {
    ORBITA_WS_TEXT = 1,
    ORBITA_WS_BINARY = 2
} orbita_ws_type_t;

typedef struct {
    orbita_ws_type_t type;
    size_t size;
    uint32_t connection_id; // Generación local de conexión; NO es session_id del backend.
    uint8_t data[ORBITA_WS_MAX_MESSAGE_BYTES]; // Texto también usa size, sin '\0'.
} orbita_ws_message_t;
