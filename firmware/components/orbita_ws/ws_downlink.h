#pragma once
#include "orbita_ws_downlink.h"
// Estado privado: solo lo toca el despacho serializado de eventos WebSocket.
typedef struct {
    orbita_ws_downlink_callback_t callback;
    void *ctx;
    void (*reject)(void *ctx, uint32_t connection, uint64_t id);
    void *reject_ctx;
    uint32_t connection;
    uint64_t id;
    uint32_t next_seq, received, expected;
    bool known, receiving, has_expected;
} ws_downlink_t;
typedef enum { WS_DL_PASS, WS_DL_HANDLED, WS_DL_INVALID } ws_dl_result_t;
void ws_downlink_reset(ws_downlink_t *dl, uint32_t connection);
void ws_downlink_fail(ws_downlink_t *dl);
ws_dl_result_t ws_downlink_control(ws_downlink_t *dl, const void *data, size_t size);
ws_dl_result_t ws_downlink_binary(ws_downlink_t *dl, const void *data, size_t size);

void ws_downlink_finish(ws_downlink_t *dl, uint32_t connection, uint64_t id);
