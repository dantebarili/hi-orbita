#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Interfaz local para unir comunicacion con playback, sin depender de I2S.
#define ORBITA_WS_PLAY_HEADER_BYTES 24u
#define ORBITA_WS_MAX_PLAY_BYTES 4096u
#define ORBITA_WS_PLAY_MAX_PCM_BYTES 480000u // 15 s, mono s16le 16000 Hz
typedef enum {
    ORBITA_WS_CONVERSATION_BEGIN,
    ORBITA_WS_CONVERSATION_END,
    ORBITA_WS_PLAY_BEGIN,
    ORBITA_WS_PLAY_DATA,
    ORBITA_WS_PLAY_END,
    ORBITA_WS_PLAY_STOP,
    ORBITA_WS_DOWNLINK_RESET,
} orbita_ws_downlink_kind_t;
typedef struct {
    orbita_ws_downlink_kind_t kind;
    uint32_t connection_id; // generacion LOCAL: devolverla en done/reject
    uint64_t id;            // id de conversacion o play_id, segun kind
    uint32_t at_index;      // solo CONVERSATION_BEGIN
    uint32_t seq;           // solo PLAY_DATA
    bool has_total_bytes;   // solo PLAY_BEGIN; false permite TTS en streaming
    uint32_t total_bytes;
    bool rejected;      // STOP por error: transporte ya encola play_reject
    const uint8_t *pcm; // solo PLAY_DATA: valido durante el callback
    size_t pcm_bytes;   // bytes (no muestras); s16le mono 16000 Hz
} orbita_ws_downlink_event_t;

// Despacho serializado de la biblioteca: puede correr en su tarea o en la
// emisora al desconectar. Copiar PCM al ring propio y volver pronto; NO esperar,
// enviar por red, hacer I2S ni conservar punteros. true=aceptado; false en
// BEGIN/DATA/END detiene y reporta reject. STOP/RESET vacian playback sin esperar.
// Orden: BEGIN -> DATA... -> END; STOP/RESET pueden interrumpir. RESET invalida todo.
typedef bool (*orbita_ws_downlink_callback_t)(void *ctx, const orbita_ws_downlink_event_t *event);
typedef enum {
    ORBITA_WS_PLAY_FINISHED,
    ORBITA_WS_PLAY_STOPPED,
    ORBITA_WS_PLAY_UNDERRUN,
} orbita_ws_play_reason_t;
