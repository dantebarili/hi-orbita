#include <math.h>
#include "beamforming.h"

void orbita_bf_reset(orbita_bf_t *bf)
{
    bf->x1_ant = 0.0f;
    bf->x2_ant = 0.0f;
    bf->v = 0.0f;
}

void orbita_bf_init(orbita_bf_t *bf, bool adelante_es_l, float ganancia_atras,
                    float beta, float fc_hz, float fs_hz)
{
    bf->adelante_es_l = adelante_es_l;
    bf->ganancia_atras = ganancia_atras;
    bf->beta = beta;
    bf->ecualizar = (fc_hz > 0.0f);
    // fc = 150 Hz, fs = 16 kHz -> a = 0,9428. Debajo de fc la ganancia queda plana (pasaaltos natural).
    bf->a = bf->ecualizar ? expf(-2.0f * (float)M_PI * fc_hz / fs_hz) : 0.0f;
    orbita_bf_reset(bf);
}

void orbita_bf_procesar(orbita_bf_t *bf, const int16_t *estereo, int16_t *mono, size_t frames)
{
    // Indices dentro del frame [L, R]: cual es el de adelante (x1) y cual el de atras (x2).
    const int i1 = bf->adelante_es_l ? 0 : 1;
    const int i2 = 1 - i1;

    for (size_t n = 0; n < frames; n++) {
        float x1 = (float)estereo[2 * n + i1];
        float x2 = (float)estereo[2 * n + i2] * bf->ganancia_atras;

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
