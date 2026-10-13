#pragma once
#include <stdbool.h>
#include "orbita_ws_message.h"

// Lógica C pura: permite probar la fragmentación en PC sin ESP-IDF.
typedef struct {
    orbita_ws_message_t message;
    bool active;
    bool frame_open;
    bool frame_fin;
    uint8_t frame_opcode;
    size_t frame_size;
    size_t frame_received;
} ws_rx_t;

typedef enum {
    WS_RX_MORE,
    WS_RX_COMPLETE,
    WS_RX_CONTROL,
    WS_RX_INVALID,
    WS_RX_TOO_LARGE
} ws_rx_result_t;

void ws_rx_reset(ws_rx_t *rx);
ws_rx_result_t ws_rx_feed(ws_rx_t *rx, uint8_t opcode, bool fin,
                         size_t frame_size, size_t offset,
                         const void *data, size_t size);
