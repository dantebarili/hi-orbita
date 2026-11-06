#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* LED DE ESTADO: traduce la FSM del vault a un color y un patron del aro (orbita_led). La tabla de colores
 * esta en audio_ws_led.c: es el unico lugar donde se cambian. Prioridad, de mayor a menor:
 *  LOGIN/OUT   toque de la tarjeta NFC, 1 latido (1,3 s) (fuera de la FSM)          verde / blanco tenue
 *  HABLANDO    parlante sonando                         (audio_play)                 lavanda, fijo
 *  SIN RED     WebSocket desconectado                   (error_reintento)            ambar tenue, latido
 *  ESCUCHANDO  conversacion abierta                     (server_send / server_back)  celeste, respiracion
 *  REPOSO      nada de lo anterior                      (audio_wake)                 celeste tenue, fijo
 * La red se consulta cada 500 ms con orbita_ws_is_connected(); el resto llega por los avisos de abajo.
 * Todas son rapidas: solo esperan un mutex que se suelta enseguida (nunca a la red ni al aro). No usar desde ISR. */

// Arranca el aro (orbita_led_init) y el timer de la red. Si el aro falla, el resto del firmware sigue.
esp_err_t audio_ws_led_init(void);

// La conversacion se abrio (wake word) o se cerro (conversation_end o timeout).
void audio_ws_led_conversacion(bool abierta);

// El parlante empezo (STARTED) o termino (DONE / STOPPED).
void audio_ws_led_hablando(bool hablando);

// Toque NFC: un latido verde (login) o blanco tenue (logout) y despues vuelve al estado. Lo llama la tarea nfc.
void audio_ws_led_nfc(bool login);
