#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "audio_capture.h"
#include "beamforming.h"
#include "audio_ring_out.h"
#include "orbita_tiempo.h"
#include "pipeline.h"

static const char *TAG = "pipeline";

static orbita_pipeline_cfg_t s_cfg;
static TaskHandle_t s_captura_handle;

// Buffers de la captura, fijos (sin malloc: no hay nada que pueda fallar al arrancar ni que liberar).
// s_crudo_buf: chunk tal cual sale del mic (32 bit por muestra). s_chunk_buf: el mismo en 16 bit.
// Los usa solo tarea_captura.
static int32_t s_crudo_buf[ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS];
static int16_t s_chunk_buf[ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS];

/* PASA-ALTOS de la captura: saca la continua de los mics (medida: -121 a +147 LSB segun la toma)
 *  Filtro:   Butterworth 2.o orden, fc = 70 Hz, fs = 16 kHz, un biquad por canal (L y R)
 *            -3 dB @ 70 Hz, -0,9 dB @ 100 Hz, 0 dB @ 1 kHz. Polos en r = 0,981.
 *  Coefs:    scipy.signal.butter(2, 70, 'high', fs=16000) -> b = g*[1, -2, 1], a = [1, A1, A2]
 *  Forma:    directa II traspuesta, float (FPU del S3); el estado sigue de un chunk al otro
 *  Dato:     int32 del I2S (24 bit alineados a la izquierda) -> float en unidades de int16 ->
 *            filtro -> redondeo y saturacion a int16. Costo: ~5 MAC por muestra. */
#define HPF_HZ      70
#define HPF_G       0.98075006f
#define HPF_A1     -1.96112953f
#define HPF_A2      0.96187072f

typedef struct {
    float z1, z2;               // memorias del biquad (forma directa II traspuesta)
} hpf_estado_t;

static hpf_estado_t s_hpf[ORBITA_CHANNELS];     // uno por canal; solo lo toca tarea_captura

// Pausa de la captura mientras suena el parlante (sin AEC, el eco dispara la wake word y ensucia el STT).
// La escribe orbita_pipeline_pausar (otra tarea) y la lee la captura: `volatile` obliga a releerla cada vuelta.
#define PAUSA_COLA_CHUNKS   5           // 5 x 64 ms = 320 ms mas despues de que el parlante termina
static volatile bool s_pausa;
static volatile uint32_t s_chunks_pausados;     // chunks no entregados por la pausa (estadistica)

// Una muestra por el biquad. Directa II traspuesta: y = b0*x + z1; z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y.
static inline float hpf_muestra(hpf_estado_t *e, float x)
{
    float y = HPF_G * x + e->z1;
    e->z1 = -2.0f * HPF_G * x - HPF_A1 * y + e->z2;
    e->z2 = HPF_G * x - HPF_A2 * y;
    return y;
}

// Chunk estereo int32 (L,R,L,R...) -> pasa-altos por canal -> int16 redondeado y saturado.
// Reemplaza al corte >>16 de antes: se filtra con los 24 bit del mic y despues se baja a 16.
static void hpf_a_16bit(const int32_t *in, int16_t *out, size_t frames)
{
    const float escala = 1.0f / 65536.0f;       // int32 alineado a la izquierda -> unidades de int16
    for (size_t k = 0; k < frames * ORBITA_CHANNELS; k++) {
        // k % 2: 0 = L, 1 = R (cada canal con su propio estado)
        float y = hpf_muestra(&s_hpf[k % ORBITA_CHANNELS], (float)in[k] * escala);
        long q = lrintf(y);                     // redondeo al entero mas cercano
        out[k] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
    }
}

// Bloques mono de 512 muestras que salen de un chunk (hoy 2). Cada uno sera un slot del ring out.
#define BLOQUES_POR_CHUNK   (ORBITA_CHUNK_FRAMES / ORBITA_SLOT_SAMPLES)

// Buffer de tarea_feed: el chunk estereo ya beamformado (mono), un bloque por fila. Las filas son
// contiguas en memoria, asi el beamformer escribe el chunk entero de una. Static: no gasta el stack de la tarea.
static int16_t s_mono_buf[BLOQUES_POR_CHUNK][ORBITA_SLOT_SAMPLES];

// Estado del beamformer (sigue de un chunk al otro). Lo toca solo tarea_feed.
static orbita_bf_t s_bf;

// Contadores: cada uno lo escribe UNA sola tarea.
static volatile uint32_t s_i2s_errores;

// Tiempos MAXIMOS en microsegundos (A15). Cada uno lo escribe una sola tarea. Ver orbita_pipeline_stats_t.
static volatile uint32_t s_max_captura_us;        // captura: una vuelta sin la lectura I2S
static volatile uint32_t s_max_ring_in_send_us;   // captura: xRingbufferSend al ring in
static volatile uint32_t s_max_on_crudo_us;       // captura: el callback on_crudo
static volatile uint32_t s_ring_in_max_items;     // captura: chunks esperando en el ring in (maximo)
static volatile uint32_t s_max_feed_us;           // feed: una vuelta sin la espera (beamformer incluido)
static volatile uint32_t s_max_ring_out_write_us; // feed: ring_out_write

// Errores de escritura al ring out (argumento invalido: no deberia pasar). La escribe tarea_feed.
static volatile uint32_t s_ring_out_errores;

// Cada chunk tiene que dar un numero entero de bloques mono: si no, el resto se perderia sin aviso.
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_SLOT_SAMPLES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_SLOT_SAMPLES");

/* RING IN: captura -> feed
 *  Dato:        ring de freeRTOS, chunks estereo 16 bit, sin header (NOSPLIT, 1 chunk por item)
 *  Item:        4096 B (1024 frames = 64 ms)    Capacidad: 8 items = 512 ms (~32 KB, en PSRAM)
 *  Productor:   tarea_captura (prio 6)    Consumidor: tarea_feed (prio 5), ambas core 1
 *  Si se llena: se descarta el chunk nuevo y sube s_overruns_ring_in */
#define RING_IN_CHUNKS        8
#define RING_IN_ITEM_HEADER   8      // cabecera que FreeRTOS pone a cada item (ItemHeader_t: 2 x 32 bit)

static RingbufHandle_t s_ring_in;
static TaskHandle_t s_feed_handle;
static volatile uint32_t s_overruns_ring_in;     // la escribe la captura
static volatile uint32_t s_chunks_invalidos;   // las dos siguientes las escribe tarea_feed
static volatile uint32_t s_bloques_mono;       // bloques mono armados (2 por chunk)

// Muestras (mono, 16 kHz) perdidas desde la ultima llamada, para el `lost_samples` del proximo slot.
// Convierte los contadores de perdida en muestras: un error de I2S o un chunk que no entro al ring in
// son 1024 frames; un overflow del DMA, ORBITA_DMA_FRAME_NUM (240); un chunk invalido del ring in, 1024.
// Aproximado (un overflow parcial no se mide exacto). La llama SOLO tarea_feed: usa estado static.
static uint32_t muestras_perdidas_nuevas(void)
{
    static uint32_t i2s_prev, dma_prev, ring_in_prev, chunk_inv_prev;
    uint32_t i2s  = s_i2s_errores;
    uint32_t dma  = orbita_audio_get_dma_overflows();
    uint32_t ring_in = s_overruns_ring_in;
    uint32_t chunk_inv = s_chunks_invalidos;
    uint32_t perdidas = (i2s - i2s_prev) * ORBITA_CHUNK_FRAMES
                      + (dma - dma_prev) * ORBITA_DMA_FRAME_NUM
                      + (ring_in - ring_in_prev) * ORBITA_CHUNK_FRAMES
                      + (chunk_inv - chunk_inv_prev) * ORBITA_CHUNK_FRAMES;
    chunk_inv_prev = chunk_inv;
    i2s_prev = i2s;
    dma_prev = dma;
    ring_in_prev = ring_in;
    return perdidas;
}

// =============================================================================
//  TAREA DE CAPTURA  -  I2S -> callback (y ring in)
// =============================================================================
// Corre siempre y nunca termina. Hay que leer siempre para vaciar el DMA (120 ms de margen).
static void tarea_captura(void *arg)
{
    // Hueco: se perdio audio antes del proximo chunk que se entregue (lectura fallida u
    // overflow del DMA). Se avisa al callback y se baja despues de entregar.
    uint32_t dma_prev = orbita_audio_get_dma_overflows();
    bool hueco = false;
    uint32_t cola = 0;                  // chunks de pausa que faltan tras soltar s_pausa

    while (1) {
        size_t frames_leidos = 0;
        esp_err_t err = orbita_audio_i2s_read(s_crudo_buf, ORBITA_CHUNK_FRAMES, &frames_leidos);
        if (err != ESP_OK || (frames_leidos != ORBITA_CHUNK_FRAMES)) {
            s_i2s_errores++;
            hueco = true;
            vTaskDelay(pdMS_TO_TICKS(10));  // no girar al maximo si el error persiste
            continue;
        }

        uint32_t t_vuelta = orbita_t_us();      // desde aca hasta el final de la vuelta: trabajo sin la lectura

        uint32_t dma_ahora = orbita_audio_get_dma_overflows();
        if (dma_ahora != dma_prev) {
            hueco = true;
            dma_prev = dma_ahora;
        }

        // Si hubo hueco el estado del filtro sigue igual: el salto da un transitorio de pocos ms.
        hpf_a_16bit(s_crudo_buf, s_chunk_buf, frames_leidos);     // 32 -> pasa-altos -> 16 bit

        // Pausa (parlante sonando, ver orbita_pipeline_pausar): se sigue leyendo el I2S y filtrando,
        // pero el chunk no va a ningun ring ni a on_crudo. Al salir, PAUSA_COLA_CHUNKS mas (reverberacion
        // y amplificador) y el primer chunk va marcado como hueco (la wake word reinicia su frontend).
        if (s_pausa) {
            cola = PAUSA_COLA_CHUNKS;
        }
        if (s_pausa || cola > 0) {
            if (!s_pausa) {
                cola--;
            }
            s_chunks_pausados++;
            hueco = true;
            orbita_max_us(&s_max_captura_us, t_vuelta);
            continue;
        }

        // Timeout 0: si no hay lugar, el chunk se pierde y queda contado.
        uint32_t t_send = orbita_t_us();
        if (xRingbufferSend(s_ring_in, s_chunk_buf, ORBITA_CHUNK_BYTES, 0) != pdTRUE) {
            s_overruns_ring_in++;
        }
        orbita_max_us(&s_max_ring_in_send_us, t_send);
        UBaseType_t esperando = 0;      // chunks que esperan a feed: si sube, feed no sigue el ritmo
        vRingbufferGetInfo(s_ring_in, NULL, NULL, NULL, NULL, &esperando);
        if (esperando > s_ring_in_max_items) {
            s_ring_in_max_items = esperando;
        }
        // Se entrega el chunk solo si alguien se registro (on_crudo != NULL). Si no, se descarta.
        // s_chunk_buf se reescribe en la vuelta siguiente: el callback tiene que copiar lo que necesite.
        if (s_cfg.on_crudo != NULL) {
            uint32_t t_cb = orbita_t_us();
            s_cfg.on_crudo(s_chunk_buf, frames_leidos, hueco);
            orbita_max_us(&s_max_on_crudo_us, t_cb);
        }
        hueco = false;
        orbita_max_us(&s_max_captura_us, t_vuelta);
    }
}

// =============================================================================
//  TAREA DE FEED  -  ring in -> beamformer -> bloques mono de 512 -> ring out
// =============================================================================
static void tarea_feed(void *arg)
{
    uint32_t pausados_prev = s_chunks_pausados;
    while (1) {
        // Espera un chunk entero (hasta 100 ms). Devuelve un puntero DENTRO del
        // ring (no copia) y en `n` cuantos bytes tiene. NULL = no llego nada.
        size_t n = 0;
        int16_t *p = xRingbufferReceive(s_ring_in, &n, pdMS_TO_TICKS(100));
        if (p == NULL) {
            continue;
        }
        uint32_t t_feed = orbita_t_us();

        if (n == ORBITA_CHUNK_BYTES) {
            // Lo perdido antes de este chunk va en el primer slot; el segundo es continuo con el primero.
            uint32_t perdidas = muestras_perdidas_nuevas();
            // Si el audio no es continuo con el chunk anterior (se perdio audio o hubo una pausa del
            // parlante), las muestras viejas que guarda el beamformer ya no valen: se arranca de cero.
            uint32_t pausados = s_chunks_pausados;
            if (perdidas > 0 || pausados != pausados_prev) {
                orbita_bf_reset(&s_bf);
                pausados_prev = pausados;
            }
            // Estereo L,R,L,R... -> hipercardioide mono, el chunk entero (los 2 bloques son contiguos).
            orbita_bf_procesar(&s_bf, (const int16_t *)p, &s_mono_buf[0][0], ORBITA_CHUNK_FRAMES);
            for (size_t b = 0; b < BLOQUES_POR_CHUNK; b++) {
                s_bloques_mono++;
                // flags = 0: no hay VAD, compuerta abierta (se manda igual).
                uint32_t t_w = orbita_t_us();
                if (orbita_ring_out_write(s_mono_buf[b], ORBITA_SLOT_SAMPLES, 0, perdidas) != ESP_OK) {
                    s_ring_out_errores++;
                }
                orbita_max_us(&s_max_ring_out_write_us, t_w);
                perdidas = 0;
            }
        } else {
            s_chunks_invalidos++;
        }

        // Se devuelve el lugar al ring, haya sido valido el chunk o no.
        vRingbufferReturnItem(s_ring_in, p);
        orbita_max_us(&s_max_feed_us, t_feed);
    }
}


// =============================================================================
//  ARRANQUE: orbita_pipeline_start(cfg)
// =============================================================================
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg)
{
    if (s_captura_handle != NULL) {
        return ESP_ERR_INVALID_STATE;   // ya arrancado: una segunda llamada duplicaria tareas y rings
    }
    if (cfg != NULL) {
        s_cfg = *cfg;
    }

    orbita_bf_init(&s_bf, ORBITA_BF_ADELANTE_ES_L, ORBITA_BF_GANANCIA_ATRAS, ORBITA_BF_BETA, ORBITA_BF_FC_HZ,
                   ORBITA_SAMPLE_RATE_HZ, ORBITA_BF_CORRIMIENTO_R, ORBITA_BF_CRUCE_HZ);
    ESP_LOGI(TAG, "Audio al servidor: hipercardioide (beta %.2f, corrimiento R %.2f) hasta %.0f Hz, arriba mic %s; "
             "wake word: mic %s; pasa-altos %d Hz",
             (double)ORBITA_BF_BETA, (double)ORBITA_BF_CORRIMIENTO_R, (double)ORBITA_BF_CRUCE_HZ,
             ORBITA_BF_ADELANTE_ES_L ? "L" : "R", ORBITA_BF_ADELANTE_ES_L ? "L" : "R", HPF_HZ);
    // El ring out (bloque contiguo de PSRAM) se reserva antes de crear las tareas que lo usan.
    // Si ya estaba iniciado (otro modulo), se sigue con ese.
    esp_err_t ring_err = orbita_ring_out_init();
    if (ring_err != ESP_OK && ring_err != ESP_ERR_INVALID_STATE) {
        return ring_err;
    }

    // En PSRAM explicito (antes dependia del umbral SPIRAM_MALLOC_ALWAYSINTERNAL). Mismo tamano que
    // xRingbufferCreateNoSplit(): (item + cabecera) x cantidad.
    s_ring_in = xRingbufferCreateWithCaps((ORBITA_CHUNK_BYTES + RING_IN_ITEM_HEADER) * RING_IN_CHUNKS,
                                        RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_in == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring in en PSRAM (%d bytes)", RING_IN_CHUNKS * (ORBITA_CHUNK_BYTES + RING_IN_ITEM_HEADER));
        return ESP_ERR_NO_MEM;
    }

    // Feed primero: el consumidor tiene que existir antes que el productor (la captura).
    // Core 1, prioridad 5: menor que la captura (6), que puede interrumpirlas.
    if (xTaskCreatePinnedToCore(tarea_feed, "feed", 4096, NULL, 5, &s_feed_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Linea de base de las perdidas: el I2S esta encendido desde orbita_audio_i2s_init() y nadie lo
    // leyo hasta ahora (el DMA acumula overflows). Eso es anterior al stream: se descarta el resultado
    // para que el primer slot no traiga ese numero. Seguro: feed espera datos y todavia no la llama.
    (void)muestras_perdidas_nuevas();

    // Captura todo el tiempo, desde el arranque, fija al core 1 (WiFi usa el 0 por defecto).
    if (xTaskCreatePinnedToCore(tarea_captura, "captura", 4096, NULL, 6, &s_captura_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void orbita_pipeline_pausar(bool pausa)
{
    s_pausa = pausa;
}

void orbita_pipeline_reset_tiempos(void)
{
    s_max_captura_us = 0;
    s_max_ring_in_send_us = 0;
    s_max_on_crudo_us = 0;
    s_ring_in_max_items = 0;
    s_max_feed_us = 0;
    s_max_ring_out_write_us = 0;
}

void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out)
{
    *out = (orbita_pipeline_stats_t){0};
    out->i2s_errores = s_i2s_errores;
    out->chunks_pausados = s_chunks_pausados;
    out->dma_overflows = orbita_audio_get_dma_overflows();
    if (s_captura_handle != NULL) {
        out->stack_libre_captura = uxTaskGetStackHighWaterMark(s_captura_handle);
    }
    out->overruns_ring_in = s_overruns_ring_in;
    out->chunks_invalidos = s_chunks_invalidos;
    out->bloques_mono = s_bloques_mono;
    out->ring_out_errores = s_ring_out_errores;
    out->max_captura_us = s_max_captura_us;
    out->max_ring_in_send_us = s_max_ring_in_send_us;
    out->max_on_crudo_us = s_max_on_crudo_us;
    out->ring_in_max_items = s_ring_in_max_items;
    out->max_feed_us = s_max_feed_us;
    out->max_ring_out_write_us = s_max_ring_out_write_us;
    if (s_feed_handle != NULL) {
        out->stack_libre_feed = uxTaskGetStackHighWaterMark(s_feed_handle);
    }
}
