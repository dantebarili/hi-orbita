#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "orbita_ws_downlink.h"

// Inicializa el playback antes de iniciar el cliente WebSocket.
esp_err_t audio_ws_playback_init(void);

// Callback de transporte: copia/entrega cada evento al modulo de playback.
// PLAY_DATA se copia al ring de playback antes de retornar.
bool audio_ws_playback_on_downlink(void *ctx, const orbita_ws_downlink_event_t *event);

// Estado para futuras decisiones del uplink mientras suena el parlante.
bool audio_ws_playback_is_active(void);
