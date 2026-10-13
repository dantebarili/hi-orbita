#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "audio_afe.h"
#include "audio_capture.h"
#include "audio_ring_out.h"
#include "wav_writer.h"
#include "pipeline.h"

static const char *TAG = "pipeline";

static orbita_pipeline_cfg_t s_cfg;
static TaskHandle_t s_captura_handle;

// Buffers de la captura, fijos (sin malloc: no hay nada que pueda fallar al arrancar ni que liberar).
// s_crudo_buf: chunk tal cual sale del mic (32 bit por muestra). s_chunk_buf: el mismo en 16 bit.
// Los usa solo tarea_captura.
static int32_t s_crudo_buf[ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS];
static int16_t s_chunk_buf[ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS];

// Bloques mono de 512 muestras que salen de un chunk (hoy 2). Cada uno sera un slot del ring out.
#define BLOQUES_POR_CHUNK   (ORBITA_CHUNK_FRAMES / ORBITA_SLOT_SAMPLES)

// Buffer de tarea_feed (sin AFE): el chunk estereo reducido a mono, un bloque por fila.
// Es static para no gastar el stack de la tarea (2 KB de 4 KB).
static int16_t s_mono_buf[BLOQUES_POR_CHUNK][ORBITA_SLOT_SAMPLES];

// Contadores: cada uno lo escribe UNA sola tarea.
static volatile uint32_t s_i2s_errores;

// Cada feed tiene que producir un numero entero de fetch (hoy 2).
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_AFE_FETCH_FRAMES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_AFE_FETCH_FRAMES");
// Cada chunk tiene que dar un numero entero de bloques mono: si no, el resto se perderia sin aviso.
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_SLOT_SAMPLES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_SLOT_SAMPLES");

/* RING IN: captura -> feed (con o sin AFE)
 *  Dato:        ring de freeRTOS, chunks estereo 16 bit, sin header (NOSPLIT, 1 chunk por item)
 *  Item:        4096 B (1024 frames = 64 ms)    Capacidad: 8 items = 512 ms
 *  Productor:   tarea_captura (prio 6)    Consumidor: tarea_feed (prio 5), ambas core 1
 *  Si se llena: se descarta el chunk nuevo y sube s_overruns_ring1 */
#define RING1_CHUNKS        8

static RingbufHandle_t s_ring1;
static TaskHandle_t s_feed_handle;
static volatile uint32_t s_overruns_ring1;     // la escribe la captura
static volatile uint32_t s_chunks_invalidos;   // las dos siguientes las escribe tarea_feed
static volatile uint32_t s_bloques_mono;       // bloques mono armados (sin AFE; 2 por chunk)

#if ORBITA_PIPELINE_USA_AFE
static TaskHandle_t s_fetch_handle;
static volatile uint32_t s_chunks_alimentados; // las siguientes las escribe tarea_feed / tarea_fetch
static volatile uint32_t s_chunks_recibidos;
static volatile uint32_t s_fetch_vacios;
static volatile uint32_t s_fetch_invalidos;
static volatile uint32_t s_bloques_voz;
#endif

// =============================================================================
//  TAREA DE CAPTURA  -  I2S -> callback (y ring in)
// =============================================================================
// Corre siempre y nunca termina. Hay que leer siempre para vaciar el DMA (90 ms de margen).
static void tarea_captura(void *arg)
{
    // Hueco: se perdio audio antes del proximo chunk que se entregue (lectura fallida u
    // overflow del DMA). Se avisa al callback y se baja despues de entregar.
    uint32_t dma_prev = orbita_audio_get_dma_overflows();
    bool hueco = false;

    while (1) {
        size_t frames_leidos = 0;
        esp_err_t err = orbita_audio_i2s_read(s_crudo_buf, ORBITA_CHUNK_FRAMES, &frames_leidos);
        if (err != ESP_OK || (frames_leidos != ORBITA_CHUNK_FRAMES)) {
            s_i2s_errores++;
            hueco = true;
            vTaskDelay(pdMS_TO_TICKS(10));  // no girar al maximo si el error persiste
            continue;
        }

        uint32_t dma_ahora = orbita_audio_get_dma_overflows();
        if (dma_ahora != dma_prev) {
            hueco = true;
            dma_prev = dma_ahora;
        }

        wav_pack_chunk_16bit(s_crudo_buf, frames_leidos, (uint8_t *)s_chunk_buf);   // 32 -> 16 bit

        // Timeout 0: si no hay lugar, el chunk se pierde y queda contado.
        if (xRingbufferSend(s_ring1, s_chunk_buf, ORBITA_CHUNK_BYTES, 0) != pdTRUE) {
            s_overruns_ring1++;
        }
        // Se entrega el chunk solo si alguien se registro (on_crudo != NULL). Si no, se descarta.
        // s_chunk_buf se reescribe en la vuelta siguiente: el callback tiene que copiar lo que necesite.
        if (s_cfg.on_crudo != NULL) {
            s_cfg.on_crudo(s_chunk_buf, frames_leidos, hueco);
        }
        hueco = false;
    }
}

// =============================================================================
//  TAREA DE FEED  -  ring in -> AFE  (sin AFE: ring in -> mono L -> bloques de 512)
// =============================================================================
static void tarea_feed(void *arg)
{
    while (1) {
        // Espera un chunk entero (hasta 100 ms). Devuelve un puntero DENTRO del
        // ring (no copia) y en `n` cuantos bytes tiene. NULL = no llego nada.
        size_t n = 0;
        int16_t *p = xRingbufferReceive(s_ring1, &n, pdMS_TO_TICKS(100));
        if (p == NULL) {
            continue;
        }

#if ORBITA_PIPELINE_USA_AFE
        if (n == ORBITA_CHUNK_BYTES) {
            orbita_afe_feed(p);      // ~20 ms: aca se va el tiempo de la tarea
            s_chunks_alimentados++;
        } else {
            s_chunks_invalidos++;    // sin log (puede haber una toma en curso)
        }
#else
        if (n == ORBITA_CHUNK_BYTES) {
            // Estereo intercalado L,R,L,R...: el frame f tiene su L en la posicion 2*f. Se descarta R.
            const int16_t *estereo = (const int16_t *)p;
            for (size_t b = 0; b < BLOQUES_POR_CHUNK; b++) {
                for (size_t k = 0; k < ORBITA_SLOT_SAMPLES; k++) {
                    s_mono_buf[b][k] = estereo[2 * (b * ORBITA_SLOT_SAMPLES + k)];
                }
                s_bloques_mono++;    // TODO (A5): entregar s_mono_buf[b] al ring out
            }
        } else {
            s_chunks_invalidos++;
        }
#endif

        // Se devuelve el lugar al ring, haya sido valido el chunk o no.
        vRingbufferReturnItem(s_ring1, p);
    }
}

#if ORBITA_PIPELINE_USA_AFE
// =============================================================================
//  TAREA DE FETCH  -  AFE -> callback on_afe
// =============================================================================
static void tarea_fetch(void *arg)
{
    while (1) {
        // Espera hasta 100 ms a que el AFE tenga audio procesado. El timeout es parte
        // de la API del AFE: sin audio, devuelve error en vez de colgarse.
        const int16_t *audio = NULL;
        size_t bytes = 0;
        bool voz = false;
        if (orbita_afe_fetch(&audio, &bytes, 100, &voz) != ESP_OK) {
            s_fetch_vacios++;
            continue;
        }

        if (bytes != ORBITA_AFE_FETCH_BYTES) {
            s_fetch_invalidos++;
            continue;
        }
        s_chunks_recibidos++;
        if (voz) {
            s_bloques_voz++;
        }

        // `audio` es memoria del AFE y vale hasta el proximo fetch: el callback copia lo que
        // necesite, sin esperar. Si fetch se atrasa, el AFE llena su buffer interno (1,6 s) y
        // sobrescribe audio sin avisar: no se puede contar esa perdida.
        if (s_cfg.on_afe != NULL) {
            s_cfg.on_afe(audio, bytes / ORBITA_BYTES_PER_SAMPLE, voz);
        }
    }
}
#endif  // ORBITA_PIPELINE_USA_AFE

// =============================================================================
//  INICIA CONFIG: se lo llama con orbita_pipeline_start(cfg)
//  no se elige la opcion desde el arfgumento pero la constante hara q se inicie 
//  con / sin AFE y de ahi sale el pipeline que esta en uso
// =============================================================================
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg)
{
    if (s_captura_handle != NULL) {
        return ESP_ERR_INVALID_STATE;   // ya arrancado: una segunda llamada duplicaria tareas y rings
    }
    if (cfg != NULL) {
        s_cfg = *cfg;
    }

#if ORBITA_PIPELINE_USA_AFE
    esp_err_t err = orbita_afe_init();
    if (err != ESP_OK) {
        return err;
    }

    // El chunk lo fija la libreria, no nosotros: si cambia (otro tipo de AFE, otra
    // version de esp-sr), se frena aca en vez de entregar audio con tamanos desfasados.
    int feed_frames = orbita_afe_feed_chunk_frames();
    if (feed_frames != ORBITA_CHUNK_FRAMES) {
        ESP_LOGE(TAG, "ORBITA_CHUNK_FRAMES=%d no coincide con los frames que pide feed (%d)",
                 ORBITA_CHUNK_FRAMES, feed_frames);
        esp_rom_printf("pipeline: ORBITA_CHUNK_FRAMES=%d, feed pide %d\n", ORBITA_CHUNK_FRAMES, feed_frames);
        return ESP_ERR_INVALID_SIZE;
    }
    int fetch_frames = orbita_afe_fetch_chunk_frames();
    if (fetch_frames != ORBITA_AFE_FETCH_FRAMES) {
        ESP_LOGE(TAG, "ORBITA_AFE_FETCH_FRAMES=%d no coincide con lo que entrega fetch (%d)",
                 ORBITA_AFE_FETCH_FRAMES, fetch_frames);
        esp_rom_printf("pipeline: ORBITA_AFE_FETCH_FRAMES=%d, fetch entrega %d\n", ORBITA_AFE_FETCH_FRAMES, fetch_frames);
        return ESP_ERR_INVALID_SIZE;
    }

#endif

    s_ring1 = xRingbufferCreateNoSplit(ORBITA_CHUNK_BYTES, RING1_CHUNKS);
    if (s_ring1 == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring in (%d bytes)", RING1_CHUNKS * ORBITA_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // Fetch primero (solo con AFE): saca del AFE lo que produce, para que nunca se llene.
    // Despues feed: el consumidor tiene que existir antes que el productor (la captura).
    // Core 1, prioridad 5: menor que la captura (6), que puede interrumpirlas.
#if ORBITA_PIPELINE_USA_AFE
    if (xTaskCreatePinnedToCore(tarea_fetch, "fetch", 4096, NULL, 5, &s_fetch_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    if (xTaskCreatePinnedToCore(tarea_feed, "feed", 4096, NULL, 5, &s_feed_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    // Captura todo el tiempo, desde el arranque, fija al core 1 (WiFi usa el 0 por defecto).
    if (xTaskCreatePinnedToCore(tarea_captura, "captura", 4096, NULL, 6, &s_captura_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out)
{
    *out = (orbita_pipeline_stats_t){0};
    out->i2s_errores = s_i2s_errores;
    out->dma_overflows = orbita_audio_get_dma_overflows();
    if (s_captura_handle != NULL) {
        out->stack_libre_captura = uxTaskGetStackHighWaterMark(s_captura_handle);
    }
    out->overruns_ring1 = s_overruns_ring1;
    out->chunks_invalidos = s_chunks_invalidos;
    out->bloques_mono = s_bloques_mono;
    if (s_feed_handle != NULL) {
        out->stack_libre_feed = uxTaskGetStackHighWaterMark(s_feed_handle);
    }
#if ORBITA_PIPELINE_USA_AFE
    out->chunks_alimentados = s_chunks_alimentados;
    out->chunks_recibidos = s_chunks_recibidos;
    out->fetch_vacios = s_fetch_vacios;
    out->fetch_invalidos = s_fetch_invalidos;
    out->bloques_voz = s_bloques_voz;
    if (s_fetch_handle != NULL) {
        out->stack_libre_fetch = uxTaskGetStackHighWaterMark(s_fetch_handle);
    }
#endif
}
