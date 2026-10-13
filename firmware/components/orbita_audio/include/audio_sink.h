/**
 * @file    audio_sink.h
 * @brief   Sink: tarea que saca slots del ring out, los junta en mensajes y se los pasa a quien envia.
 * @details Diseno en orbita-obsidian/arquitectura.md §3.5. La tarea no sabe de UART ni de sockets:
 *          llama a `send` del `orbita_sink_t` registrado. Cambiar de destino = registrar otro
 *          orbita_sink_t, sin tocar captura, ring ni esta tarea.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "audio_ring_out.h"

// Slots por mensaje. 4 x 32 ms = 128 ms, ~4 KB. PROVISORIO: lo define Comunicacion (arquitectura.md §3.5).
#define ORBITA_MSG_SLOTS  4

/* MENSAJE: lo que el sink entrega a `send` de una vez. Audio CONTINUO (sin huecos adentro):
 *          si entre dos slots hubo una perdida, el mensaje en curso se cierra y empieza otro.
 *          NO es el header del cable: el sink de Comunicacion lo arma a partir de estos campos.
 *  seq_primero:  `seq` del primer slot (los siguientes son seq_primero+1, +2...)
 *  lost_antes:   muestras perdidas justo antes del primer slot (0 = continuo con el mensaje anterior)
 *  n_slots:      slots juntados (1..ORBITA_MSG_SLOTS; menos si el mensaje se cerro por una perdida)
 *  n_voz:        de esos slots, cuantos traen VOZ     } resumen de VOZ para el header del cable:
 *  n_vad_ok:     de esos slots, cuantos traen VAD_OK  } pendiente de decidir (plan, decision 4)
 *  n_samples:    muestras de audio validas en `audio` (suma de los `n_samples` de los slots)
 *  audio:        muestras mono 16 bit. Vale SOLO durante la llamada a `send` (se reusa el buffer) */
typedef struct {
    uint32_t       seq_primero;
    uint32_t       lost_antes;
    uint16_t       n_slots;
    uint16_t       n_voz;
    uint16_t       n_vad_ok;
    uint16_t       n_samples;
    const int16_t *audio;
} orbita_msg_t;

/* SINK: el destino de los mensajes (UART, socket...).
 *  send:  función que envia el mensaje, según el sink. Corre en la tarea del sink (core 0): puede tardar (red), nunca en la captura.
 *         ESP_OK = enviado. Otro valor = fallo: el mensaje se descarta y se cuenta (la retencion con
 *         reintentos es A6). 
 *  ctx:   dato propio del sink (p. ej. el socket); se pasa tal cual a `send`.
 *  Falta (cuando lo pida el socket): open / close para conectar y reconectar. */
typedef struct {
    esp_err_t (*send)(void *ctx, const orbita_msg_t *msg);   
    void       *ctx;
} orbita_sink_t;

typedef struct {
    uint32_t msgs_enviados;
    uint32_t msgs_fallidos;      // `send` devolvio error
    uint32_t slots_invalidos;    // slots con `version` distinta (se saltean)
    uint32_t max_read_us;        // maximo de orbita_ring_out_read con slot, en us
    uint32_t max_send_us;        // maximo de `send`, en us (presupuesto: < 128 000 en promedio, arquitectura.md §3.5)
    uint32_t stack_libre;        // bytes minimos libres del stack de la tarea
} orbita_sink_stats_t;

/**
 * @brief  Crea la tarea del sink (core 0, prioridad 5, stack 4 KB). Llamar UNA vez, despues de
 *         orbita_ring_out_init().
 * @param [in]  sink  destino; se copia (el puntero `ctx` tiene que seguir vivo)
 * @return ESP_OK; ESP_ERR_INVALID_ARG (sink o send NULL); ESP_ERR_INVALID_STATE (ya arrancado); ESP_ERR_NO_MEM.
 */
esp_err_t orbita_sink_start(const orbita_sink_t *sink);

/** Pone en 0 los tiempos maximos. */
void orbita_sink_reset_tiempos(void);

/** Copia los contadores actuales. */
void orbita_sink_get_stats(orbita_sink_stats_t *out);
