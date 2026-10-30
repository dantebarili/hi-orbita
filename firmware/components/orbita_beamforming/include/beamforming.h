/**
 * @file    beamforming.h
 * @brief   Beamforming propio: array diferencial endfire de 2 mics -> hipercardioide (mono).
 *
 * @details Diseno: artifact "Hipercardioide con dos cardioides" / "Array diferencial de 2 mics" (29/10).
 *          Mics en linea, separados d = c/fs = 343/16000 = 21,44 mm (= 1 muestra de retardo acustico).
 *
 *            c_F[n] = x1[n] - x2[n-1]        cardioide hacia adelante (nulo atras)
 *            c_B[n] = x2[n] - x1[n-1]        cardioide hacia atras    (nulo adelante)
 *            y_h[n] = c_F[n] - beta*c_B[n]   beta = 0,5 -> hipercardioide (nulos a ~110 grados, costados -12 dB)
 *            v[n]   = a*v[n-1] + y_h[n]      ecualizador: integrador con perdida, a = exp(-2*pi*fc/fs)
 *            y[n]   = v[n] / 2               mismo nivel que un mic solo
 *
 *          x1 = mic de adelante, x2 = mic de atras (ya multiplicado por la correccion de calibracion).
 *          Sin dependencias de ESP-IDF: es C puro, se puede compilar y probar en la PC.
 *          Falta (a proposito, primera prueba): division en bandas (> 3 kHz con un mic solo).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    // --- Parametros (los fija orbita_bf_init) ---
    bool  adelante_es_l;     // true: el slot L del I2S es el mic de adelante (x1)
    float ganancia_atras;    // 1/g: corrige la diferencia de ganancia del mic de atras (1,0 = sin calibrar)
    float beta;              // 0 cardioide, 0,27 supercardioide, 0,5 hipercardioide, 1 dipolo
    bool  ecualizar;         // false: sale y_h/2 crudo (sube 6 dB/oct), util para depurar
    float a;                 // polo del integrador
    // --- Estado (sigue de un bloque al otro) ---
    float x1_ant;            // x1[n-1]
    float x2_ant;            // x2[n-1]
    float v;                 // acumulador del integrador
} orbita_bf_t;

/**
 * @brief  Carga los parametros y pone el estado en cero.
 *
 * @param [out] bf              estado a inicializar
 * @param [in]  adelante_es_l   true si el mic del slot L es el de adelante
 * @param [in]  ganancia_atras  multiplica al mic de atras antes de restar (1,0 sin calibrar)
 * @param [in]  beta            peso de c_B (0,5 = hipercardioide)
 * @param [in]  fc_hz           corte del integrador (~150 Hz); <= 0 apaga el ecualizador
 * @param [in]  fs_hz           frecuencia de muestreo (16000)
 */
void orbita_bf_init(orbita_bf_t *bf, bool adelante_es_l, float ganancia_atras,
                    float beta, float fc_hz, float fs_hz);

/** Pone el estado en cero (llamar si hubo un hueco en el audio: las muestras anteriores ya no valen). */
void orbita_bf_reset(orbita_bf_t *bf);

/**
 * @brief  Procesa un bloque estereo entrelazado (L,R,L,R...) y deja una salida mono.
 *
 * @param [in,out] bf       estado
 * @param [in]     estereo  frames*2 muestras int16
 * @param [out]    mono     frames muestras int16 (redondeado y saturado)
 * @param [in]     frames   cantidad de frames
 */
void orbita_bf_procesar(orbita_bf_t *bf, const int16_t *estereo, int16_t *mono, size_t frames);
