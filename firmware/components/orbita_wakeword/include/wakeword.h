/**
 * @file    wakeword.h
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Orbita
 * @brief   Wake word (microWakeWord, modelo orbita) sobre el audio crudo del pipeline.
 * @version 0.1.0
 * @date    2026-10-25
 *
 * @copyright Copyright (c) 2026 - Proyecto Orbita
 *
 * @details on_crudo copia el mic elegido a un ring; la tarea `wakeword` arma las features
 *          (mismo frontend que el entrenamiento), corre el modelo .tflite en streaming y decide
 *          como ESPHome (promedio de las ultimas N probabilidades contra el cutoff).
 *          Explicacion completa: orbita-obsidian/wakeword_firmware.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Carga el modelo, prepara el frontend y crea el ring y la tarea `wakeword`.
 *
 * @details Llamar UNA vez, antes de orbita_pipeline_start() (la arena del modelo se reserva
 *          primero en RAM interna).
 *
 * @return  ESP_OK; ESP_ERR_INVALID_STATE si ya se arranco; ESP_ERR_NO_MEM; ESP_FAIL si el
 *          modelo o el frontend no se pudieron preparar (se loguea el motivo).
 */
esp_err_t orbita_wakeword_start(void);

/**
 * @brief   Callback para orbita_pipeline_cfg_t.on_crudo (ver orbita_crudo_cb_t en pipeline.h).
 *
 * @details Corre en la tarea de captura: solo copia el promedio de los dos mics (orbita_mono_lr) al ring, con
 *          timeout 0. Si el ring esta lleno, descarta el chunk y lo marca como hueco.
 */
void orbita_wakeword_on_crudo(const int16_t *chunk_ini, size_t frames, bool hueco);

#ifdef __cplusplus
}
#endif
