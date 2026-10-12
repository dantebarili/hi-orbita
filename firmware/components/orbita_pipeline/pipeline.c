#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "audio_afe.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "pipeline.h"

static const char *TAG = "pipeline";

static orbita_pipeline_cfg_t s_cfg;
static TaskHandle_t s_captura_handle;

// Contadores: cada uno lo escribe UNA sola tarea (volatile alcanza para leerlos desde otra).
static volatile uint32_t s_i2s_errores;

// Cada feed tiene que producir un numero entero de fetch (hoy 2).
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_AFE_FETCH_FRAMES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_AFE_FETCH_FRAMES");

#if ORBITA_PIPELINE_USA_AFE
/* ----------------------------------------------------------------------------
 *  RING 1:      captura -> feed
 * ----------------------------------------------------------------------------
 *  Dato:        chunks de ENTRADA al AFE (estereo, 16 bit, sin header)
 *  Tipo:        NOSPLIT: un chunk por item, se guarda y se devuelve entero
 *  Item:        ORBITA_CHUNK_BYTES = 4096 B (1024 frames = 64 ms)
 *  Capacidad:   RING1_CHUNKS = 8 items = 512 ms de colchon
 *  Productor:   tarea_captura (core 1, prio 6)
 *  Consumidor:  tarea_feed    (core 1, prio 5)
 *  Si se llena: el chunk nuevo se descarta y aumenta s_overruns_ring1
 * ------------------------------------------------------------------------- */
#define RING1_CHUNKS        8

static RingbufHandle_t s_ring1;
static TaskHandle_t s_feed_handle;
static TaskHandle_t s_fetch_handle;
static volatile uint32_t s_overruns_ring1;     // la escribe la captura
static volatile uint32_t s_chunks_alimentados; // las siguientes las escribe tarea_feed / tarea_fetch
static volatile uint32_t s_chunks_invalidos;
static volatile uint32_t s_chunks_recibidos;
static volatile uint32_t s_fetch_vacios;
static volatile uint32_t s_fetch_invalidos;
static volatile uint32_t s_bloques_voz;
#endif

// =============================================================================
//  TAREA DE CAPTURA  -  I2S -> callback (y ring 1 si hay AFE)
// =============================================================================
// Corre siempre y nunca termina. Hay que leer siempre para vaciar el DMA (90 ms de margen).
static void tarea_captura(void *arg)
{
    // chunk_buf: chunk tal cual sale del mic (32 bit por muestra).
    // pack_buf:  el mismo chunk en 16 bit, listo para entregar.
    int32_t *chunk_buf = malloc(ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS * sizeof(int32_t));
    int16_t *pack_buf = malloc(ORBITA_CHUNK_BYTES);
    if (chunk_buf == NULL || pack_buf == NULL) {
        ESP_LOGE(TAG, "No se pudieron reservar los buffers de captura");
        vTaskDelete(NULL);  // una tarea nunca hace return: se borra a si misma
    }

    while (1) {
        size_t frames_leidos = 0;
        esp_err_t err = orbita_audio_i2s_read(chunk_buf, ORBITA_CHUNK_FRAMES, &frames_leidos);
        if (err != ESP_OK || frames_leidos != ORBITA_CHUNK_FRAMES) {
            s_i2s_errores++;
            vTaskDelay(pdMS_TO_TICKS(10));  // no girar al maximo si el error persiste
            continue;
        }

        wav_pack_chunk_16bit(chunk_buf, frames_leidos, (uint8_t *)pack_buf);   // 32 -> 16 bit

#if ORBITA_PIPELINE_USA_AFE
        // El AFE escucha todo el tiempo. Timeout 0: si no hay lugar, se pierde y queda contado.
        if (xRingbufferSend(s_ring1, pack_buf, ORBITA_CHUNK_BYTES, 0) != pdTRUE) {
            s_overruns_ring1++;
        }
#endif

        if (s_cfg.on_chunk != NULL) {
            s_cfg.on_chunk(pack_buf, frames_leidos);
        }
    }
}

#if ORBITA_PIPELINE_USA_AFE
// =============================================================================
//  TAREA DE FEED  -  ring 1 -> AFE
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

        if (n == ORBITA_CHUNK_BYTES) {
            orbita_afe_feed(p);      // ~20 ms: aca se va el tiempo de la tarea
            s_chunks_alimentados++;
        } else {
            s_chunks_invalidos++;    // sin log (puede haber una toma en curso)
        }

        // Se devuelve el lugar al ring, haya sido valido el chunk o no.
        vRingbufferReturnItem(s_ring1, p);
    }
}

// =============================================================================
//  TAREA DE FETCH  -  AFE -> callback on_afe_block
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
        if (s_cfg.on_afe_block != NULL) {
            s_cfg.on_afe_block(audio, bytes / ORBITA_BYTES_PER_SAMPLE, voz);
        }
    }
}
#endif  // ORBITA_PIPELINE_USA_AFE

// =============================================================================
//  API
// =============================================================================
esp_err_t orbita_pipeline_start(const orbita_pipeline_cfg_t *cfg)
{
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
        return ESP_ERR_INVALID_SIZE;
    }
    int fetch_frames = orbita_afe_fetch_chunk_frames();
    if (fetch_frames != ORBITA_AFE_FETCH_FRAMES) {
        ESP_LOGE(TAG, "ORBITA_AFE_FETCH_FRAMES=%d no coincide con lo que entrega fetch (%d)",
                 ORBITA_AFE_FETCH_FRAMES, fetch_frames);
        return ESP_ERR_INVALID_SIZE;
    }

    s_ring1 = xRingbufferCreateNoSplit(ORBITA_CHUNK_BYTES, RING1_CHUNKS);
    if (s_ring1 == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring 1 (%d bytes)", RING1_CHUNKS * ORBITA_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // Fetch primero: saca del AFE lo que produce, para que nunca se llene. Despues feed:
    // el consumidor tiene que existir antes que el productor (la captura).
    // Core 1, prioridad 5: menor que la captura (6), que puede interrumpirlas.
    if (xTaskCreatePinnedToCore(tarea_fetch, "fetch", 4096, NULL, 5, &s_fetch_handle, 1) != pdPASS ||
        xTaskCreatePinnedToCore(tarea_feed, "feed", 4096, NULL, 5, &s_feed_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif

    // Captura desde el arranque, fija al core 1 (WiFi usa el 0 por defecto).
    if (xTaskCreatePinnedToCore(tarea_captura, "captura", 4096, NULL, 6, &s_captura_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out)
{
    *out = (orbita_pipeline_stats_t){0};
    out->i2s_errores = s_i2s_errores;
    if (s_captura_handle != NULL) {
        out->stack_libre_captura = uxTaskGetStackHighWaterMark(s_captura_handle);
    }
#if ORBITA_PIPELINE_USA_AFE
    out->overruns_ring1 = s_overruns_ring1;
    out->chunks_alimentados = s_chunks_alimentados;
    out->chunks_invalidos = s_chunks_invalidos;
    out->chunks_recibidos = s_chunks_recibidos;
    out->fetch_vacios = s_fetch_vacios;
    out->fetch_invalidos = s_fetch_invalidos;
    out->bloques_voz = s_bloques_voz;
    out->stack_libre_feed = uxTaskGetStackHighWaterMark(s_feed_handle);
    out->stack_libre_fetch = uxTaskGetStackHighWaterMark(s_fetch_handle);
#endif
}
