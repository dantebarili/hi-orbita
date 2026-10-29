#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "frontend.h"
#include "frontend_util.h"
#include "audio_capture.h"
#include "orbita_tiempo.h"
#include "pipeline.h"
#include "wakeword.h"
#include "wakeword_modelo.h"

static const char *TAG = "wakeword";

// --- Modelo embebido y su manifiesto (.json) ---
// EMBED_FILES (CMakeLists) mete el archivo en el binario y crea dos simbolos: inicio y fin.
// `asm("nombre")` le dice al compilador con que nombre buscar cada uno al enlazar.
extern const uint8_t s_tflite_ini[] asm("_binary_orbita_tflite_start");
extern const uint8_t s_tflite_fin[] asm("_binary_orbita_tflite_end");
#define WW_NOMBRE      "orbita"
#define WW_CUTOFF      0.40f       // orbita-dataset/docs/resultados_v0.md: streaming + promedio de 5, 90,3 % detecta y
                                   // 0 activaciones en 5,3 h (aparecen bajo 0,29). Voces TTS: ajustar en placa
                                   // (si se activa solo, subir de a 0,1)
#define WW_VENTANA     5
#define WW_ARENA       50000

// --- Frontend: TIENE que coincidir con el entrenamiento (pymicro-features, micro_features.cpp) ---
#define FE_FEATURES           40
#define FE_VENTANA_MS         30
#define FE_PASO_MS            10
#define FE_FLOAT32_SCALE      0.0390625f    // pymicro-features: feature = uint16 * 1/25.6
#define IGNORAR_INICIAL       100           // inferencias sin detectar al arrancar y tras detectar (ESPHome)
#define MAX_STRIDE            4             // stride del modelo (3 en los nuestros), con margen
#define RESUMEN_US            10000000      // log de resumen cada 10 s

/* RING WAKE: captura -> tarea wakeword
 *  Dato:        ring de FreeRTOS NOSPLIT, 1 item = 1 chunk mono (1024 muestras = 64 ms) + marca de hueco
 *  Capacidad:   16 items = 1 s (~33 KB, en PSRAM)
 *  Productor:   on_crudo, en tarea_captura (prio 6)    Consumidor: tarea_wakeword (prio 4), ambas core 1
 *  Si se llena: se descarta el chunk nuevo, sube s_overruns y el siguiente va marcado como hueco */
typedef struct {
    bool    hueco;                              // se perdio audio ANTES de este chunk
    int16_t muestras[ORBITA_CHUNK_FRAMES];
} chunk_mono_t;
#define RING_ITEMS        16
#define RING_ITEM_HEADER  8                     // cabecera que FreeRTOS pone a cada item

static RingbufHandle_t s_ring;
static TaskHandle_t s_tarea;
static chunk_mono_t s_chunk_cb;                 // armado del chunk en on_crudo (static: no gasta stack de la captura)
static bool s_hueco_pendiente;                  // lo escribe solo on_crudo
static volatile uint32_t s_overruns;            // chunks que no entraron al ring (on_crudo)

// Estado de la tarea (solo la tarea lo toca)
static struct FrontendState s_fe;
static ww_modelo_info_t s_modelo;
static float s_factor_q;                        // FE_FLOAT32_SCALE / escala_in (precalculado)
static uint8_t s_cutoff_u8;
static int8_t s_entrada[MAX_STRIDE * FE_FEATURES];
static int s_paso;                              // columnas ya cargadas en s_entrada (0..stride-1)
static uint8_t s_probs[WW_VENTANA];             // ventana deslizante de probabilidades (0-255)
static int s_probs_idx;
static int s_ignorar;                           // < 0: no se detecta todavia (cuenta hacia 0)
static uint32_t s_n_features, s_n_inferencias;  // contadores

// Resumen (los escribe solo la tarea)
static uint32_t s_detecciones, s_huecos;
static uint8_t s_prob_max_periodo;
static volatile uint32_t s_max_frontend_us, s_max_invoke_us;

static void reiniciar_deteccion(void)
{
    memset(s_probs, 0, sizeof(s_probs));
    s_probs_idx = 0;
    s_ignorar = -IGNORAR_INICIAL;
}

// Hueco en el audio: el frontend (reduccion de ruido, PCAN) y la ventana arrancan de cero. La
// memoria interna del modelo no se puede resetear sin recargarlo; se tapa con la espera inicial.
static void reiniciar_por_hueco(void)
{
    FrontendReset(&s_fe);
    s_paso = 0;
    reiniciar_deteccion();
    s_huecos++;
}

// uint16 del frontend -> int8 del modelo: int8 = round(v * 1/25.6 / escala_in) + cero_in.
// No se usa la formula fija de ESPHome ((v*256 + 333)/666 - 128): asume escala 26/256 y los
// modelos traen 26/255 (difiere en ~6 % de los valores por 1 LSB). Mismo calculo en float32 que
// tools/wakeword_ref.py, para que el test bit a bit de igual.
static void cuantizar(const uint16_t *v, int8_t *out)
{
    for (int i = 0; i < FE_FEATURES; i++) {
        long q = lrintf((float)v[i] * s_factor_q) + s_modelo.cero_in;
        out[i] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
    }
}

// Misma decision que ESPHome (streaming_model.cpp): promedio de las ultimas WW_VENTANA
// probabilidades contra el cutoff, y espera de IGNORAR_INICIAL inferencias que solo avanza
// mientras la probabilidad esta por debajo del cutoff.
static void decidir(uint8_t p)
{
    s_probs[s_probs_idx] = p;
    s_probs_idx = (s_probs_idx + 1) % WW_VENTANA;
    if (p > s_prob_max_periodo) {
        s_prob_max_periodo = p;
    }
    if (p < s_cutoff_u8 && s_ignorar < 0) {
        s_ignorar++;
    }
    uint32_t suma = 0;
    for (int i = 0; i < WW_VENTANA; i++) {
        suma += s_probs[i];
    }
    if (s_ignorar == 0 && suma > (uint32_t)s_cutoff_u8 * WW_VENTANA) {
        s_detecciones++;
        ESP_LOGW(TAG, ">>> DETECCION \"%s\" (inferencia %u, promedio %u/255)", WW_NOMBRE,
                 (unsigned)s_n_inferencias, (unsigned)(suma / WW_VENTANA));
        reiniciar_deteccion();
    }
}

// Pasa `n` muestras por el frontend; cada columna de features nueva va a la entrada del modelo
// y, cuando hay `stride` columnas, se corre una inferencia.
static void procesar_audio(const int16_t *m, size_t n)
{
    while (n > 0) {
        size_t leidas = 0;
        uint32_t t_fe = orbita_t_us();
        // El frontend junta muestras por dentro y devuelve una columna cada 160 muestras nuevas
        // (10 ms); si todavia no le alcanza, size = 0. `leidas` dice cuantas consumio.
        struct FrontendOutput o = FrontendProcessSamples(&s_fe, m, n, &leidas);
        m += leidas;
        n -= leidas;
        if (o.size == 0) {
            if (leidas == 0) {
                break;          // no deberia pasar; evita girar para siempre
            }
            continue;
        }
        orbita_max_us(&s_max_frontend_us, t_fe);

        int8_t *columna = &s_entrada[s_paso * FE_FEATURES];
        cuantizar(o.values, columna);
        s_n_features++;
        s_paso++;
        if (s_paso < s_modelo.stride) {
            continue;
        }
        s_paso = 0;

        uint8_t p = 0;
        uint32_t t_inv = orbita_t_us();
        if (ww_modelo_invoke(s_entrada, &p) != ESP_OK) {
            ESP_LOGE(TAG, "Invoke fallo");
            continue;
        }
        orbita_max_us(&s_max_invoke_us, t_inv);
        s_n_inferencias++;
        decidir(p);
    }
}

static void log_resumen(void)
{
    orbita_pipeline_stats_t ps;
    orbita_pipeline_get_stats(&ps);
    ESP_LOGI(TAG, "%s: detecciones=%u prob_max_10s=%u/255 (cutoff %u) | frontend_max=%u us invoke_max=%u us | "
             "huecos=%u overruns_ring=%u | captura: dma_ovf=%u ring_in_ovr=%u captura_max=%u us | "
             "stack libre=%u RAM interna libre=%u",
             WW_NOMBRE, (unsigned)s_detecciones, (unsigned)s_prob_max_periodo, (unsigned)s_cutoff_u8,
             (unsigned)s_max_frontend_us, (unsigned)s_max_invoke_us, (unsigned)s_huecos, (unsigned)s_overruns,
             (unsigned)ps.dma_overflows, (unsigned)ps.overruns_ring_in, (unsigned)ps.max_captura_us,
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_prob_max_periodo = 0;
}

static void tarea_wakeword(void *arg)
{
    uint32_t t_resumen = orbita_t_us();
    while (1) {
        size_t n = 0;
        chunk_mono_t *c = xRingbufferReceive(s_ring, &n, pdMS_TO_TICKS(100));
        if (c != NULL) {
            if (n == sizeof(chunk_mono_t)) {
                if (c->hueco) {
                    reiniciar_por_hueco();
                }
                procesar_audio(c->muestras, ORBITA_CHUNK_FRAMES);
            }
            vRingbufferReturnItem(s_ring, c);
        }
        if (orbita_t_us() - t_resumen >= RESUMEN_US) {
            t_resumen = orbita_t_us();
            log_resumen();
        }
    }
}

void orbita_wakeword_on_crudo(const int16_t *chunk_ini, size_t frames, bool hueco)
{
    if (s_ring == NULL || frames != ORBITA_CHUNK_FRAMES) {
        return;
    }
    // Estereo L,R,L,R...: promedio de los dos mics (el mismo mono que el modo crudo).
    for (size_t k = 0; k < frames; k++) {
        s_chunk_cb.muestras[k] = orbita_mono_lr(chunk_ini, k);
    }
    s_chunk_cb.hueco = hueco || s_hueco_pendiente;
    if (xRingbufferSend(s_ring, &s_chunk_cb, sizeof(s_chunk_cb), 0) == pdTRUE) {
        s_hueco_pendiente = false;
    } else {
        s_overruns++;
        s_hueco_pendiente = true;                       // el proximo que entre avisa que falta audio
    }
}

esp_err_t orbita_wakeword_start(void)
{
    if (s_tarea != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ww_modelo_init(s_tflite_ini, (size_t)(s_tflite_fin - s_tflite_ini), WW_ARENA, &s_modelo);
    if (err != ESP_OK) {
        return err;
    }
    if (s_modelo.features != FE_FEATURES || s_modelo.stride < 1 || s_modelo.stride > MAX_STRIDE) {
        ESP_LOGE(TAG, "El modelo pide [%d x %d]; se esperaba [1..%d x %d]", s_modelo.stride, s_modelo.features,
                 MAX_STRIDE, FE_FEATURES);
        return ESP_FAIL;
    }
    s_factor_q = FE_FLOAT32_SCALE / s_modelo.escala_in;
    s_cutoff_u8 = (uint8_t)(WW_CUTOFF * 255.0f);        // ESPHome guarda el cutoff como uint8 (0-255)
    reiniciar_deteccion();

    // Frontend: los mismos valores que pymicro-features (micro_features.cpp) y ESPHome
    // (preprocessor_settings.h). Si se cambia uno, el modelo deja de reconocer sin dar error.
    struct FrontendConfig cfg;
    cfg.window.size_ms = FE_VENTANA_MS;
    cfg.window.step_size_ms = FE_PASO_MS;
    cfg.filterbank.num_channels = FE_FEATURES;
    cfg.filterbank.lower_band_limit = 125.0f;
    cfg.filterbank.upper_band_limit = 7500.0f;
    cfg.filterbank.output_scale_shift = 0;                 // no lo usa la libreria
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = 0.025f;
    cfg.noise_reduction.odd_smoothing = 0.06f;
    cfg.noise_reduction.min_signal_remaining = 0.05f;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = 0.95f;
    cfg.pcan_gain_control.offset = 80.0f;
    cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1;
    cfg.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&cfg, &s_fe, ORBITA_SAMPLE_RATE_HZ)) {
        ESP_LOGE(TAG, "FrontendPopulateState fallo (sin memoria)");
        return ESP_ERR_NO_MEM;
    }

    s_ring = xRingbufferCreateWithCaps((sizeof(chunk_mono_t) + RING_ITEM_HEADER) * RING_ITEMS,
                                       RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Core 1, prioridad 4: por debajo de captura (6) y feed (5), para no frenarlas nunca.
    // Stack de 6 KB: TFLite Micro; medir con el resumen y ajustar.
    if (xTaskCreatePinnedToCore(tarea_wakeword, "wakeword", 6144, NULL, 4, &s_tarea, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Wake word \"%s\": stride=%d cutoff=%u/255 ventana=%d, mic %s", WW_NOMBRE, s_modelo.stride,
             (unsigned)s_cutoff_u8, WW_VENTANA, ORBITA_RAW_MIC_SOURCE);
    return ESP_OK;
}
