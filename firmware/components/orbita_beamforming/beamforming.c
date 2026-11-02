#include <math.h>
#include "beamforming.h"

void orbita_bf_reset(orbita_bf_t *bf)
{
    bf->l_ant = 0.0f;
    bf->r_hist[0] = bf->r_hist[1] = bf->r_hist[2] = 0.0f;
    bf->x1_ant = 0.0f;
    bf->x2_ant = 0.0f;
    bf->v = 0.0f;
}

void orbita_bf_init(orbita_bf_t *bf, bool adelante_es_l, float ganancia_atras,
                    float beta, float fc_hz, float fs_hz, float corrimiento_r)
{
    bf->adelante_es_l = adelante_es_l;
    bf->ganancia_atras = ganancia_atras;
    bf->beta = beta;
    bf->ecualizar = (fc_hz > 0.0f);
    // fc = 150 Hz, fs = 16 kHz -> a = 0,9428. Debajo de fc la ganancia queda plana (pasaaltos natural).
    bf->a = bf->ecualizar ? expf(-2.0f * (float)M_PI * fc_hz / fs_hz) : 0.0f;

    // Lagrange de 3.er orden para un retardo D entre 1 y 2 (ahi es mas exacto). D = 1,86 ->
    // h = {-0,0229, 0,1484, 0,9118, -0,0373} (suman 1). Con corrimiento 0: h = {0, 1, 0, 0} = 1 muestra pura.
    const float D = 1.0f + corrimiento_r;
    bf->h_r[0] = -(D - 1.0f) * (D - 2.0f) * (D - 3.0f) / 6.0f;
    bf->h_r[1] =  D * (D - 2.0f) * (D - 3.0f) / 2.0f;
    bf->h_r[2] = -D * (D - 1.0f) * (D - 3.0f) / 2.0f;
    bf->h_r[3] =  D * (D - 1.0f) * (D - 2.0f) / 6.0f;
    orbita_bf_reset(bf);
}

void orbita_bf_procesar(orbita_bf_t *bf, const int16_t *estereo, int16_t *mono, size_t frames)
{
    for (size_t n = 0; n < frames; n++) {
        // 1) Alinear: R por el FIR (1 + corrimiento), L 1 muestra pura. El corrimiento es del CANAL R,
        //    por eso se corrige antes de elegir cual es el mic de adelante.
        float l = (float)estereo[2 * n];
        float r = (float)estereo[2 * n + 1];
        float r_al = bf->h_r[0] * r + bf->h_r[1] * bf->r_hist[0]
                   + bf->h_r[2] * bf->r_hist[1] + bf->h_r[3] * bf->r_hist[2];
        float l_al = bf->l_ant;
        bf->r_hist[2] = bf->r_hist[1];         // corre la historia: R[n-1] -> R[n-2] -> R[n-3]
        bf->r_hist[1] = bf->r_hist[0];
        bf->r_hist[0] = r;
        bf->l_ant = l;

        // 2) Adelante (x1) y atras (x2), ya alineados.
        float x1 = bf->adelante_es_l ? l_al : r_al;
        float x2 = (bf->adelante_es_l ? r_al : l_al) * bf->ganancia_atras;

        float c_f = x1 - bf->x2_ant;             // cardioide adelante
        float c_b = x2 - bf->x1_ant;             // cardioide atras
        float y_h = c_f - bf->beta * c_b;        // hipercardioide (con beta = 0,5)
        bf->x1_ant = x1;
        bf->x2_ant = x2;

        // El acumulador puede pasar el rango de int16 (ganancia en DC = 1/(1-a) ~ 17): va en float.
        float y;
        if (bf->ecualizar) {
            bf->v = bf->a * bf->v + y_h;
            y = 0.5f * bf->v;
        } else {
            y = 0.5f * y_h;
        }

        long q = lrintf(y);                      // redondeo y saturacion, igual que el pasaaltos de la captura
        mono[n] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
    }
}
