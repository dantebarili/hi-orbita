/**
 * @file    audio_afe.h
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Órbita
 * @brief   Funciones del Audio Front-End (esp-sr): 2 mics -> beamforming + VAD -> audio mono.
 * @version 0.1.0
 * @date    2026-10-11
 *
 * @copyright Copyright (c) 2026 - Proyecto Órbita
 *
 * @details El resto del firmware no ve los tipos de esp-sr: solo estas funciones.
 *          La instancia del AFE vive privada en audio_afe.c. Cambiar la
 *          configuracion (WebRTC / NSNet, modo del AFE) se hace en audio_afe.c, sin tocar main.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @brief  Crea la instancia del AFE.
 *
 * @details Lee los modelos de la particion "model" y arma la config: 2 mics ("MM"),
 *          tipo SR, modo LOW_COST, WakeNet y AEC apagados. Despues la valida con
 *          afe_config_check (con 2 mics el beamforming desplaza a NS) e imprime
 *          config, chunks y pipeline. Crea las tareas internas del AFE.
 *          Llamar UNA vez, antes de cualquier feed.
 *
 * @return  ESP_OK si quedo listo.
 *          ESP_FAIL si fallo la config.
 *          ESP_ERR_NO_MEM si no pudo crear la instancia.
 */
esp_err_t orbita_afe_init(void);

/**
 * @brief  true si el VAD quedo activo despues de la validacion de la config (afe_config_check).
 *
 * @details El pipeline la usa para poner el flag VAD_OK del ring out: si el VAD esta apagado, el
 *          `voz` de orbita_afe_fetch() es siempre false y no significa "silencio". Valida despues de init.
 */
bool orbita_afe_vad_activo(void);

/**
 * @brief  Entrega un chunk de audio al AFE.
 *
 * @details El chunk tiene orbita_afe_feed_chunk_frames() frames, 2 canales entrelazados
 *          (L R L R ...), int16 a 16 kHz. Tarda ~20 ms (medido): llamarla desde una
 *          tarea propia, nunca desde la de captura. Alguien tiene que hacer fetch al
 *          mismo ritmo; si no, el AFE sobrescribe audio viejo y solo avisa por log.
 *          No hace nada si el AFE no esta creado.
 *
 * @param [in]  chunk  puntero al chunk de entrada (no se modifica)
 */
void orbita_afe_feed(const int16_t *chunk);

/**
 * @brief  Frames por canal que pide cada orbita_afe_feed().
 *
 * @details Hoy 1024 (64 ms). Sirve para chequear al arrancar que coincida con
 *          FRAMES_PER_CHUNK de main.c.
 *
 * @return  Frames por canal, o 0 si el AFE no esta creado.
 */
int orbita_afe_feed_chunk_frames(void);

/**
 * @brief  Muestras que entrega cada fetch.
 *
 * @details Hoy 512 (32 ms, mono): la mitad que un feed, asi que salen 2 fetch por
 *          cada feed. Sirve para chequear al arrancar que coincida con
 *          FETCH_FRAMES_PER_CHUNK de main.c.
 *
 * @return  Muestras por fetch, o 0 si el AFE no esta creado.
 */
int orbita_afe_fetch_chunk_frames(void);

/**
 * @brief  Pide al AFE un resultado ya procesado (audio mono).
 *
 * @details Espera hasta `timeout_ms` a que haya audio. El audio es memoria del AFE:
 *          vale SOLO hasta la proxima llamada a esta funcion, asi que si hay que
 *          conservarlo se copia antes de volver a pedir. Tiene que llamarse al
 *          ritmo del AFE (2 por cada feed); si no, el AFE llena su buffer y
 *          sobrescribe audio sin avisar.
 *
 * @param [out]  audio       puntero al audio mono (int16, 16 kHz); solo se escribe si devuelve ESP_OK
 * @param [out]  bytes       tamaño del audio en bytes; solo se escribe si devuelve ESP_OK
 * @param [in]   timeout_ms  cuanto esperar si todavia no hay audio
 * @param [out]  voz         true si el VAD marca voz en este bloque; puede ser NULL; solo se escribe si devuelve ESP_OK
 *
 * @return  ESP_OK si hay audio.
 *          ESP_ERR_TIMEOUT si no hubo audio (vencio el tiempo o el AFE devolvio error).
 *          ESP_ERR_INVALID_STATE si el AFE no esta creado.
 */
esp_err_t orbita_afe_fetch(const int16_t **audio, size_t *bytes, uint32_t timeout_ms, bool *voz);
