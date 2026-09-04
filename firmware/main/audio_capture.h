/**
 * @file    audio_capture.h
 * @author  Dante Barili - Juan Ignacio Forster / Proyecto Órbita
 * @brief   Controlador de bajo nivel para interfaz I2S de dos microfonos, DMA y gestión en PSRAM.
 * @version 1.0.0
 * @date    2026-09-03
 * 
 * @copyright Copyright (c) 2026 - Proyecto Órbita
 * 
 * @details Este módulo gestiona la captura en tiempo real de micrófonos MEMS I2S
 *          usando el periférico I2S0 del ESP32-S3, canalizando las ráfagas
 *          hacia la memoria volátil externa mediante acceso directo a memoria (DMA).
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h" 

// Inicializacion de pines
#define ORBITA_I2S_BCLK_GPIO 4
#define ORBITA_I2S_WS_GPIO   5
#define ORBITA_I2S_DIN_GPIO  6

#define ORBITA_SAMPLE_RATE_HZ 16000

/**
 * @brief  Inicializacion del canal I2S
 *
 * @details Inicializacion del canal I2S en modo
 *          estandar, RX (receptor), estereo (x2 mics,L y R), 32 bits por slot 
 *
 * @return  X con error, Y por confirmacion
 *         
 */
esp_err_t orbita_audio_i2s_init(void);

/**
 * @brief  
 *
 * @details 
 *          
 *
 * @param [in]   out_buf     puntero a la posicion de memoria donde queremos copiar los datos del mic
 * @param [in]   frame_count     cuantos frames queremos leer
 * @param [in]   frame_read    puntero a la direccion donde queremos guardar cuantos frames consiguio leer
 *
 * @return  err
 *         
 */
// Lee `frame_count` frames estereo (cada frame = 1 muestra L + 1 muestra R,
// cada una de 32 bits crudos tal como los entrega el mic).
// `out_buf` debe tener espacio para frame_count * 2 * sizeof(int32_t).
esp_err_t orbita_audio_i2s_read(int32_t *out_buf, size_t frame_count, size_t *frames_read);
