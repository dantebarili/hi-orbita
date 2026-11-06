#include <math.h>
#include "beamforming.h"

// Butterworth de 2.o orden (Q = 1/raiz(2)) por transformada bilineal (formulas del "Audio EQ Cookbook" de
// R. Bristow-Johnson). Dos en cascada = Linkwitz-Riley de 4.o orden. Con fc = 4 kHz, fs = 16 kHz:
// LP b = {0,2929, 0,5858, 0,2929}, HP b = {0,2929, -0,5858, 0,2929}, a1 = 0, a2 = 0,1716.
static void biquad_butter(orbita_biquad_t *q, bool pasa_altos, float fc_hz, float fs_hz)
{
    const float w0 = 2.0f * (float)M_PI * fc_hz / fs_hz;
    const float c = cosf(w0);
    const float alfa = sinf(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + alfa;
    const float k = pasa_altos ? (1.0f + c) / 2.0f : (1.0f - c) / 2.0f;
    q->b0 = k / a0;
    q->b1 = (pasa_altos ? -2.0f * k : 2.0f * k) / a0;
    q->b2 = k / a0;
    q->a1 = -2.0f * c / a0;
    q->a2 = (1.0f - alfa) / a0;
    q->z1 = q->z2 = 0.0f;
}

// Una muestra por el biquad (directa II traspuesta, igual que hpf_muestra en pipeline.c).
static inline float biquad_muestra(orbita_biquad_t *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

void orbita_bf_reset(orbita_bf_t *bf)
{
    bf->l_ant = 0.0f;
    bf->r_hist[0] = bf->r_hist[1] = bf->r_hist[2] = 0.0f;
    bf->x1_ant = 0.0f;
    bf->x2_ant = 0.0f;
    bf->v = 0.0f;
    for (int i = 0; i < 2; i++) {          // solo las memorias: los coeficientes no cambian
        bf->lp[i].z1 = bf->lp[i].z2 = 0.0f;
        bf->hp[i].z1 = bf->hp[i].z2 = 0.0f;
    }
}

void orbita_bf_init(orbita_bf_t *bf, bool adelante_es_l, float ganancia_atras,
                    float beta, float fc_hz, float fs_hz, float corrimiento_r, float cruce_hz)
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

    bf->bandas = (cruce_hz > 0.0f);
    for (int i = 0; i < 2; i++) {
        biquad_butter(&bf->lp[i], false, bf->bandas ? cruce_hz : 1000.0f, fs_hz);   // sin bandas: no se usan
        biquad_butter(&bf->hp[i], true,  bf->bandas ? cruce_hz : 1000.0f, fs_hz);
    }
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
        float mic = bf->x1_ant;                  // x1[n-1]: rama de agudos (se guarda antes de pisarlo)
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

        // 3) Bandas: hipercardioide debajo del cruce + mic de adelante arriba. Se suma en float.
        if (bf->bandas) {
            float bajos = biquad_muestra(&bf->lp[1], biquad_muestra(&bf->lp[0], y));
            float altos = biquad_muestra(&bf->hp[1], biquad_muestra(&bf->hp[0], mic));
            y = bajos + altos;
        }

        long q = lrintf(y);                      // redondeo y saturacion, igual que el pasaaltos de la captura
        mono[n] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
    }
}
