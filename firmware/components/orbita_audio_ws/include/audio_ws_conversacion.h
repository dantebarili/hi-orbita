#pragma once

#include "esp_err.h"

/* CONVERSACION: compuerta del audio hacia el servidor (FSM del vault: audio_wake <-> server_send)
 *  ESPERANDO:     no se manda audio; el ring out guarda los ultimos ~32 s
 *  CONVERSACION:  se manda en tiempo real, empezando por el pre-roll
 *  Entra:  wake word (dev_wake_word)
 *  Sale:   `conversation_end` del servidor (srv_fin_conversacion) o timeout local de seguridad
 *          (AUDIO_WS_CONV_TIMEOUT_S sin `play` ni `orbita` del servidor) */
#define AUDIO_WS_PREROLL_SLOTS   94     // 94 x 32 ms = 3,0 s antes de la deteccion (la palabra + margen)
#define AUDIO_WS_CONV_TIMEOUT_S  60

// Cierra la compuerta y crea el timer. Llamar ANTES de orbita_sink_start().
esp_err_t audio_ws_conversacion_init(void);

// Para orbita_wakeword_start(): abre la conversacion. Rapida, no bloquea.
void audio_ws_conversacion_on_wake(void);

// El servidor mostro actividad (play, orbita, fin de un play): reinicia el timeout.
void audio_ws_conversacion_actividad(void);

// `conversation_end` del servidor: vuelve a ESPERANDO.
void audio_ws_conversacion_fin(void);
