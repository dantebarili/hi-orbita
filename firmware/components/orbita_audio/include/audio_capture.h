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
 * @brief  Inicializa y enciende el canal I2S de captura.
 *
 * @details Modo estandar (Philips), solo RX, la ESP32 como master, estereo
 *          (L y R: un mic por slot), 32 bits por slot, 16 kHz. Buffer DMA de
 *          6 descriptores x 240 frames = 1440 frames (~90 ms): es el maximo
 *          que puede pasar entre dos orbita_audio_i2s_read() sin perder audio.
 *          Registra el contador de overflows del DMA antes de encender el canal.
 *          Llamar UNA vez, antes de cualquier lectura.
 *
 * @return  ESP_OK si el canal quedo encendido.
 *          Si no, el codigo de error del driver (i2s_new_channel, init_std_mode,
 *          register_event_callback o enable) y se loguea cual fallo.
 */
esp_err_t orbita_audio_i2s_init(void);

/**
 * @brief  Lee `frame_count` frames estereo del mic.
 *
 * @details Cada frame = 1 muestra L + 1 muestra R, de 32 bits crudos tal como los
 *          entrega el mic. BLOQUEA hasta tener todos los frames pedidos, asi que
 *          la lectura dura frame_count / 16000 s (1024 frames = 64 ms) y marca el
 *          ritmo del que la llama. Entre dos llamadas no pueden pasar mas de ~90 ms.
 *
 * @param [out]  out_buf       destino; con espacio para frame_count * 2 * sizeof(int32_t) bytes
 * @param [in]   frame_count   cuantos frames se piden
 * @param [out]  frames_read   frames realmente leidos; puede ser NULL si no interesa
 *
 * @return  ESP_OK, o el error de i2s_channel_read (se loguea).
 */
esp_err_t orbita_audio_i2s_read(int32_t *out_buf, size_t frame_count, size_t *frames_read);

/**
 * @brief  Cuantas veces el DMA piso audio que nadie habia leido (lectura llego tarde).
 *
 * @details Cada cuenta = un buffer de DMA (240 frames, 15 ms) perdido: un hueco en el audio.
 *          Lo incrementa una interrupcion. Es distinto de `overruns` de main.c,
 *          que cuenta chunks que no entraron a un ring lleno.
 *
 * @return  Cantidad acumulada desde el ultimo reset.
 */
uint32_t orbita_audio_get_dma_overflows(void);

/**
 * @brief  Pone el contador de overflows del DMA en 0.
 *
 * @details Llamar al empezar cada grabacion.
 */
void orbita_audio_reset_dma_overflows(void);
