/**
 * @file    wakeword_modelo.h
 * @brief   Envoltorio en C del modelo TFLite Micro (privado del componente).
 *
 * @details TFLite Micro es C++. Todo el C++ queda en wakeword_modelo.cc; el resto del
 *          componente (C) lo usa a traves de estas funciones. `extern "C"` le dice al
 *          compilador de C++ que exporte los nombres como C, para que wakeword.c los encuentre.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Lo que el modelo espera y entrega, leido del propio .tflite (no escrito a mano). */
typedef struct {
    int      stride;         // columnas de features por Invoke (dims[1] de la entrada; 3 en los nuestros)
    int      features;       // numeros por columna (dims[2]; tiene que ser 40)
    float    escala_in;      // cuantizacion de la entrada: int8 = feature / escala_in + cero_in
    int32_t  cero_in;
    int32_t  cero_out;       // salida uint8: probabilidad = (q - cero_out) / 255
    size_t   arena_usada;    // bytes de arena que uso AllocateTensors (para ajustar el tamano)
    bool     modelo_interno; // true si el .tflite se copio a RAM interna (si no, quedo en PSRAM)
} ww_modelo_info_t;

/**
 * @brief   Copia el .tflite a RAM alineada, reserva la arena y prepara el interprete.
 * @param   tflite       inicio del .tflite (embebido en flash)
 * @param   tflite_len   bytes del .tflite
 * @param   arena_bytes  tamano de la arena (tensor_arena_size del .json, con margen)
 * @param   info         [out] lo que espera el modelo
 */
esp_err_t ww_modelo_init(const uint8_t *tflite, size_t tflite_len, size_t arena_bytes, ww_modelo_info_t *info);

/**
 * @brief   Corre una inferencia con `stride` columnas de features int8 (stride * 40 bytes).
 * @param   features  columnas una detras de otra, en orden de llegada
 * @param   prob_u8   [out] probabilidad cuantizada (0-255, tal cual sale del modelo)
 */
esp_err_t ww_modelo_invoke(const int8_t *features, uint8_t *prob_u8);

#ifdef __cplusplus
}
#endif
