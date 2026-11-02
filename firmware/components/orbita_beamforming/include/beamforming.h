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
 *
 *          CORRIMIENTO L/R (01/11, vault Beamforming/resumen_beamforming.html): el canal R del I2S llega adelantado 0,86 muestras
 *          (electrico, sigue al canal, no al mic). Antes de la resta se retrasa R con un FIR de Lagrange
 *          de 4 coeficientes (retardo 1 + corrimiento) y L con 1 muestra pura: quedan alineados.
 *          Validado en Python contra un retardo ideal (FFT): mismos dB en las 4 tomas; error < 0,2 dB
 *          y < 0,03 muestras hasta 3 kHz. Latencia extra: 1 muestra (62,5 us).
 *          Sin dependencias de ESP-IDF: es C puro, se puede compilar y probar en la PC.
 *          Falta (a proposito, primera prueba): division en bandas (> 3 kHz con un mic solo).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PARAMETROS DEL PRODUCTO (los usa orbita_pipeline; medidos en placa el 01/11, ver vault Beamforming/)
 *  Adelante:     slot L = mic de adelante (apunta al que habla). Si se monta al reves, false.
 *  beta:         0,5 hipercardioide (nulo ~110°, costados -12 dB, atras -6 dB). 0,27 = supercardioide
 *                (mas rechazo atras, menos de costado).
 *  fc:           150 Hz el integrador (debajo, pasaaltos natural).
 *  Ganancia:     1,0 = sin calibrar (los mics difieren ~1,3 dB; la calibracion por unidad esta pendiente).
 *  Corrimiento:  el canal R llega 0,86 muestras antes que L (electrico, del bus I2S). */
#define ORBITA_BF_ADELANTE_ES_L    true
#define ORBITA_BF_BETA             0.5f
#define ORBITA_BF_FC_HZ            150.0f
#define ORBITA_BF_GANANCIA_ATRAS   1.0f
#define ORBITA_BF_CORRIMIENTO_R    0.86f

typedef struct {
    // --- Parametros (los fija orbita_bf_init) ---
    bool  adelante_es_l;     // true: el slot L del I2S es el mic de adelante (x1)
    float ganancia_atras;    // 1/g: corrige la diferencia de ganancia del mic de atras (1,0 = sin calibrar)
    float beta;              // 0 cardioide, 0,27 supercardioide, 0,5 hipercardioide, 1 dipolo
    bool  ecualizar;         // false: sale y_h/2 crudo (sube 6 dB/oct), util para depurar
    float a;                 // polo del integrador
    float h_r[4];            // FIR de Lagrange que retrasa R (1 + corrimiento_r) muestras
    // --- Estado (sigue de un bloque al otro) ---
    float l_ant;             // L[n-1]: L retrasado 1 muestra
    float r_hist[3];         // R[n-1], R[n-2], R[n-3] para el FIR
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
 * @param [in]  corrimiento_r   muestras que el canal R llega ADELANTADO respecto de L (0 a 1; medido: 0,86).
 *                              0 = sin compensar (igual que antes, salvo 1 muestra de latencia)
 */
void orbita_bf_init(orbita_bf_t *bf, bool adelante_es_l, float ganancia_atras,
                    float beta, float fc_hz, float fs_hz, float corrimiento_r);

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
