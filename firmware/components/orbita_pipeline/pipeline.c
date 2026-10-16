#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "audio_afe.h"
#include "audio_capture.h"
#include "audio_ring_out.h"
#include "orbita_tiempo.h"
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
#if !ORBITA_PIPELINE_USA_AFE
static int16_t s_mono_buf[BLOQUES_POR_CHUNK][ORBITA_SLOT_SAMPLES];
#endif

// Contadores: cada uno lo escribe UNA sola tarea.
static volatile uint32_t s_i2s_errores;

// Tiempos MAXIMOS en microsegundos (A15). Cada uno lo escribe una sola tarea. Ver orbita_pipeline_stats_t.
static volatile uint32_t s_max_captura_us;        // captura: una vuelta sin la lectura I2S
static volatile uint32_t s_max_ring_in_send_us;   // captura: xRingbufferSend al ring in
static volatile uint32_t s_max_on_crudo_us;       // captura: el callback on_crudo
static volatile uint32_t s_ring_in_max_items;     // captura: chunks esperando en el ring in (maximo)
static volatile uint32_t s_max_feed_us;           // feed: una vuelta sin la espera (con AFE incluye afe_feed)
static volatile uint32_t s_max_ring_out_write_us; // feed (sin AFE) o fetch (con AFE): ring_out_write
#if ORBITA_PIPELINE_USA_AFE
static volatile uint32_t s_max_fetch_us;          // fetch: una vuelta sin la espera del AFE
static volatile uint32_t s_max_on_afe_us;         // fetch: el callback on_afe
#endif

// Errores de escritura al ring out (argumento invalido: no deberia pasar). La escribe quien produce slots.
static volatile uint32_t s_ring_out_errores;

// Cada feed tiene que producir un numero entero de fetch (hoy 2).
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_AFE_FETCH_FRAMES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_AFE_FETCH_FRAMES");
// Cada chunk tiene que dar un numero entero de bloques mono: si no, el resto se perderia sin aviso.
_Static_assert(ORBITA_CHUNK_FRAMES % ORBITA_SLOT_SAMPLES == 0, "ORBITA_CHUNK_FRAMES debe ser multiplo de ORBITA_SLOT_SAMPLES");

/* RING IN: captura -> feed (con o sin AFE)
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
static volatile uint32_t s_bloques_mono;       // bloques mono armados (sin AFE; 2 por chunk)

#if ORBITA_PIPELINE_USA_AFE
static TaskHandle_t s_fetch_handle;
// Frames que pide cada afe_feed (lo fija esp-sr: 512 sin beamforming, 1024 con BSS). El chunk de captura
// (ORBITA_CHUNK_FRAMES) tiene que ser multiplo: se alimenta en ORBITA_CHUNK_FRAMES / s_feed_frames llamadas.
static int s_feed_frames;
static volatile uint32_t s_chunks_alimentados; // las siguientes las escribe tarea_feed / tarea_fetch
static volatile uint32_t s_chunks_recibidos;
static volatile uint32_t s_fetch_vacios;
static volatile uint32_t s_fetch_invalidos;
static volatile uint32_t s_bloques_voz;
#endif

// Muestras (mono, 16 kHz) perdidas desde la ultima llamada, para el `lost_samples` del proximo slot.
// Convierte los contadores de perdida en muestras: un error de I2S o un chunk que no entro al ring in
// son 1024 frames; un overflow del DMA, ORBITA_DMA_FRAME_NUM (240); un chunk invalido del ring in, 1024;
// con AFE, un fetch invalido, 512. Aproximado (un overflow parcial no se mide exacto).
// La llama SOLO la tarea que escribe el ring out (feed sin AFE, fetch con AFE): usa estado static.
static uint32_t muestras_perdidas_nuevas(void)
{
    static uint32_t i2s_prev, dma_prev, ring_in_prev, chunk_inv_prev;
#if ORBITA_PIPELINE_USA_AFE
    static uint32_t fetch_inv_prev;
#endif
    uint32_t i2s  = s_i2s_errores;
    uint32_t dma  = orbita_audio_get_dma_overflows();
    uint32_t ring_in = s_overruns_ring_in;
    uint32_t chunk_inv = s_chunks_invalidos;
    uint32_t perdidas = (i2s - i2s_prev) * ORBITA_CHUNK_FRAMES
                      + (dma - dma_prev) * ORBITA_DMA_FRAME_NUM
                      + (ring_in - ring_in_prev) * ORBITA_CHUNK_FRAMES
                      + (chunk_inv - chunk_inv_prev) * ORBITA_CHUNK_FRAMES;
#if ORBITA_PIPELINE_USA_AFE
    uint32_t fetch_inv = s_fetch_invalidos;
    perdidas += (fetch_inv - fetch_inv_prev) * ORBITA_AFE_FETCH_FRAMES;
    fetch_inv_prev = fetch_inv;
#endif
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

        wav_pack_chunk_16bit(s_crudo_buf, frames_leidos, (uint8_t *)s_chunk_buf);   // 32 -> 16 bit

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
//  TAREA DE FEED  -  ring in -> AFE  (sin AFE: ring in -> mono L -> bloques de 512)
// =============================================================================
static void tarea_feed(void *arg)
{
    while (1) {
        // Espera un chunk entero (hasta 100 ms). Devuelve un puntero DENTRO del
        // ring (no copia) y en `n` cuantos bytes tiene. NULL = no llego nada.
        size_t n = 0;
        int16_t *p = xRingbufferReceive(s_ring_in, &n, pdMS_TO_TICKS(100));
        if (p == NULL) {
            continue;
        }
        uint32_t t_feed = orbita_t_us();

#if ORBITA_PIPELINE_USA_AFE
        if (n == ORBITA_CHUNK_BYTES) {
            // Un chunk de 1024 frames puede ser 1 o 2 feeds, segun lo que pida el AFE (ver s_feed_frames).
            for (int f = 0; f < ORBITA_CHUNK_FRAMES; f += s_feed_frames) {
                orbita_afe_feed(p + f * ORBITA_CHANNELS);   // aca se va el tiempo de la tarea
            }
            s_chunks_alimentados++;
        } else {
            s_chunks_invalidos++;    // sin log (puede haber una toma en curso)
        }
#else
        if (n == ORBITA_CHUNK_BYTES) {
            // Estereo L,R,L,R...: se selecciona un canal sin cambiar la captura I2S.
            const int16_t *estereo = (const int16_t *)p;
            // Lo perdido antes de este chunk va en el primer slot; el segundo es continuo con el primero.
            uint32_t perdidas = muestras_perdidas_nuevas();
            for (size_t b = 0; b < BLOQUES_POR_CHUNK; b++) {
                for (size_t k = 0; k < ORBITA_SLOT_SAMPLES; k++) {
                    s_mono_buf[b][k] = estereo[2 * (b * ORBITA_SLOT_SAMPLES + k) + ORBITA_RAW_MIC_INDEX];
                }
                s_bloques_mono++;
                // flags = 0: sin AFE no hay VAD, compuerta abierta (se manda igual).
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
#endif

        // Se devuelve el lugar al ring, haya sido valido el chunk o no.
        vRingbufferReturnItem(s_ring_in, p);
        orbita_max_us(&s_max_feed_us, t_feed);
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
        uint32_t t_fetch = orbita_t_us();     // desde aca: trabajo sin la espera del AFE
        s_chunks_recibidos++;
        if (voz) {
            s_bloques_voz++;
        }

        // Al ring out: VAD_OK solo si el VAD quedo activo tras afe_config_check (si no, voz=false
        // no significa silencio y una compuerta cortaria todo); VOZ segun el VAD.
        // Los `fetch` vacios/invalidos no entran en las perdidas (pendiente de decidir).
        uint8_t flags = orbita_afe_vad_activo() ? (ORBITA_FLAG_VAD_OK | (voz ? ORBITA_FLAG_VOZ : 0)) : 0;
        uint32_t t_w = orbita_t_us();
        if (orbita_ring_out_write(audio, bytes / ORBITA_BYTES_PER_SAMPLE, flags, muestras_perdidas_nuevas()) != ESP_OK) {
            s_ring_out_errores++;
        }
        orbita_max_us(&s_max_ring_out_write_us, t_w);

        // `audio` es memoria del AFE y vale hasta el proximo fetch: el callback copia lo que
        // necesite, sin esperar. Si fetch se atrasa, el AFE llena su buffer interno (1,6 s) y
        // sobrescribe audio sin avisar: no se puede contar esa perdida.
        if (s_cfg.on_afe != NULL) {
            uint32_t t_cb = orbita_t_us();
            s_cfg.on_afe(audio, bytes / ORBITA_BYTES_PER_SAMPLE, voz);
            orbita_max_us(&s_max_on_afe_us, t_cb);
        }
        orbita_max_us(&s_max_fetch_us, t_fetch);
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

#if !ORBITA_PIPELINE_USA_AFE
    ESP_LOGI(TAG, "Audio crudo mono: %s", ORBITA_RAW_MIC_SOURCE);
#endif
    // El ring out se reserva ANTES del AFE (bloque contiguo de PSRAM; esp-sr la fragmenta) y antes
    // de crear las tareas que lo usan. Si ya estaba iniciado (otro modulo), se sigue con ese.
    esp_err_t ring_err = orbita_ring_out_init();
    if (ring_err != ESP_OK && ring_err != ESP_ERR_INVALID_STATE) {
        return ring_err;
    }

#if ORBITA_PIPELINE_USA_AFE
    esp_err_t err = orbita_afe_init();
    if (err != ESP_OK) {
        return err;
    }

    // El chunk lo fija la libreria, no nosotros: si cambia (otro tipo de AFE, otra
    // version de esp-sr), se frena aca en vez de entregar audio con tamanos desfasados.
    int feed_frames = orbita_afe_feed_chunk_frames();
    if (feed_frames <= 0 || ORBITA_CHUNK_FRAMES % feed_frames != 0) {
        ESP_LOGE(TAG, "ORBITA_CHUNK_FRAMES=%d no es multiplo de los frames que pide feed (%d)",
                 ORBITA_CHUNK_FRAMES, feed_frames);
        esp_rom_printf("pipeline: ORBITA_CHUNK_FRAMES=%d, feed pide %d\n", ORBITA_CHUNK_FRAMES, feed_frames);
        return ESP_ERR_INVALID_SIZE;
    }
    s_feed_frames = feed_frames;
    int fetch_frames = orbita_afe_fetch_chunk_frames();
    if (fetch_frames != ORBITA_AFE_FETCH_FRAMES) {
        ESP_LOGE(TAG, "ORBITA_AFE_FETCH_FRAMES=%d no coincide con lo que entrega fetch (%d)",
                 ORBITA_AFE_FETCH_FRAMES, fetch_frames);
        esp_rom_printf("pipeline: ORBITA_AFE_FETCH_FRAMES=%d, fetch entrega %d\n", ORBITA_AFE_FETCH_FRAMES, fetch_frames);
        return ESP_ERR_INVALID_SIZE;
    }

#endif

    // En PSRAM explicito (antes dependia del umbral SPIRAM_MALLOC_ALWAYSINTERNAL). Mismo tamano que
    // xRingbufferCreateNoSplit(): (item + cabecera) x cantidad.
    s_ring_in = xRingbufferCreateWithCaps((ORBITA_CHUNK_BYTES + RING_IN_ITEM_HEADER) * RING_IN_CHUNKS,
                                        RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring_in == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring in en PSRAM (%d bytes)", RING_IN_CHUNKS * (ORBITA_CHUNK_BYTES + RING_IN_ITEM_HEADER));
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

    // Linea de base de las perdidas: el I2S esta encendido desde orbita_audio_i2s_init() y nadie lo
    // leyo hasta ahora (con AFE, orbita_afe_init() tarda segundos y el DMA acumula overflows). Eso es
    // anterior al stream: se descarta el resultado para que el primer slot no traiga ese numero.
    // Seguro: feed/fetch estan esperando datos y todavia no llaman a la funcion.
    (void)muestras_perdidas_nuevas();

    // Captura todo el tiempo, desde el arranque, fija al core 1 (WiFi usa el 0 por defecto).
    if (xTaskCreatePinnedToCore(tarea_captura, "captura", 4096, NULL, 6, &s_captura_handle, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void orbita_pipeline_reset_tiempos(void)
{
    s_max_captura_us = 0;
    s_max_ring_in_send_us = 0;
    s_max_on_crudo_us = 0;
    s_ring_in_max_items = 0;
    s_max_feed_us = 0;
    s_max_ring_out_write_us = 0;
#if ORBITA_PIPELINE_USA_AFE
    s_max_fetch_us = 0;
    s_max_on_afe_us = 0;
#endif
}

void orbita_pipeline_get_stats(orbita_pipeline_stats_t *out)
{
    *out = (orbita_pipeline_stats_t){0};
    out->i2s_errores = s_i2s_errores;
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
#if ORBITA_PIPELINE_USA_AFE
    out->chunks_alimentados = s_chunks_alimentados;
    out->chunks_recibidos = s_chunks_recibidos;
    out->fetch_vacios = s_fetch_vacios;
    out->fetch_invalidos = s_fetch_invalidos;
    out->bloques_voz = s_bloques_voz;
    out->max_fetch_us = s_max_fetch_us;
    out->max_on_afe_us = s_max_on_afe_us;
    if (s_fetch_handle != NULL) {
        out->stack_libre_fetch = uxTaskGetStackHighWaterMark(s_fetch_handle);
    }
#endif
}
