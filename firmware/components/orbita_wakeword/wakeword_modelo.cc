// Envoltorio de TFLite Micro (C++) con API en C: ver wakeword_modelo.h.
// Mismo armado que ESPHome micro_wake_word (streaming_model.cpp): resolver con las ops del
// modelo streaming, variables de recurso para su estado interno y arena reservada una sola vez.

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "wakeword_modelo.h"

static const char *TAG = "ww_modelo";

// Arena chica aparte para las variables de recurso (el estado del modelo streaming). ESPHome usa 1024.
#define ARENA_VARIABLES_BYTES   1024
#define MAX_VARIABLES           20

// Todo static: el modelo vive desde el arranque hasta que se apaga el equipo (no hay unload).
static tflite::MicroMutableOpResolver<20> s_resolver;
static tflite::MicroInterpreter *s_interprete;
static TfLiteTensor *s_entrada;
static TfLiteTensor *s_salida;
static int s_stride;

// Reserva alineada a 16 bytes (lo pide TFLite Micro), preferentemente en RAM interna; si no
// entra, en PSRAM. `interna` dice donde quedo.
static uint8_t *reservar(size_t bytes, bool *interna)
{
    uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *interna = (p != NULL);
    if (p == NULL) {
        p = (uint8_t *)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return p;
}

// Las ops que usan los modelos microWakeWord (lista de ESPHome). Las que el modelo no use
// solo ocupan un poco de flash; si falta una, AllocateTensors falla y lo dice.
static bool registrar_ops(void)
{
    return s_resolver.AddCallOnce() == kTfLiteOk && s_resolver.AddVarHandle() == kTfLiteOk &&
           s_resolver.AddReshape() == kTfLiteOk && s_resolver.AddReadVariable() == kTfLiteOk &&
           s_resolver.AddStridedSlice() == kTfLiteOk && s_resolver.AddConcatenation() == kTfLiteOk &&
           s_resolver.AddAssignVariable() == kTfLiteOk && s_resolver.AddConv2D() == kTfLiteOk &&
           s_resolver.AddMul() == kTfLiteOk && s_resolver.AddAdd() == kTfLiteOk &&
           s_resolver.AddMean() == kTfLiteOk && s_resolver.AddFullyConnected() == kTfLiteOk &&
           s_resolver.AddLogistic() == kTfLiteOk && s_resolver.AddQuantize() == kTfLiteOk &&
           s_resolver.AddDepthwiseConv2D() == kTfLiteOk && s_resolver.AddAveragePool2D() == kTfLiteOk &&
           s_resolver.AddMaxPool2D() == kTfLiteOk && s_resolver.AddPad() == kTfLiteOk &&
           s_resolver.AddPack() == kTfLiteOk && s_resolver.AddSplitV() == kTfLiteOk;
}

extern "C" esp_err_t ww_modelo_init(const uint8_t *tflite, size_t tflite_len, size_t arena_bytes, ww_modelo_info_t *info)
{
    if (s_interprete != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // El .tflite embebido no tiene alineacion garantizada: se copia a un buffer alineado
    // (en RAM interna si entra: los pesos se leen en cada Invoke y la PSRAM es mas lenta).
    uint8_t *copia = reservar(tflite_len, &info->modelo_interno);
    uint8_t *arena = NULL;
    bool arena_interna = false;
    if (copia != NULL) {
        memcpy(copia, tflite, tflite_len);
        arena = reservar(arena_bytes, &arena_interna);
    }
    uint8_t *arena_var = (uint8_t *)heap_caps_aligned_alloc(16, ARENA_VARIABLES_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (copia == NULL || arena == NULL || arena_var == NULL) {
        ESP_LOGE(TAG, "Sin memoria para modelo (%u B) + arena (%u B)", (unsigned)tflite_len, (unsigned)arena_bytes);
        return ESP_ERR_NO_MEM;     // lo reservado queda: el llamador detiene el arranque
    }

    const tflite::Model *modelo = tflite::GetModel(copia);
    if (modelo->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Esquema del .tflite %u, se esperaba %d", (unsigned)modelo->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }
    if (!registrar_ops()) {
        ESP_LOGE(TAG, "No se pudieron registrar las ops");
        return ESP_FAIL;
    }

    tflite::MicroAllocator *alloc_var = tflite::MicroAllocator::Create(arena_var, ARENA_VARIABLES_BYTES);
    tflite::MicroResourceVariables *variables = tflite::MicroResourceVariables::Create(alloc_var, MAX_VARIABLES);
    // `new (buffer) Tipo(...)` ("placement new") construye el objeto en memoria ya reservada:
    // asi el interprete no usa el heap de C++ y su tamano queda fijo desde el arranque.
    static uint8_t buf_interprete[sizeof(tflite::MicroInterpreter)] __attribute__((aligned(16)));
    s_interprete = new (buf_interprete) tflite::MicroInterpreter(modelo, s_resolver, arena, arena_bytes, variables);
    if (s_interprete->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors fallo: falta una op o la arena (%u B) es chica", (unsigned)arena_bytes);
        return ESP_FAIL;
    }

    s_entrada = s_interprete->input(0);
    s_salida = s_interprete->output(0);
    // Entrada esperada: [1, stride, 40] int8. Salida: [1, 1] uint8. Si no, el modelo no es de microWakeWord v2.
    if (s_entrada->type != kTfLiteInt8 || s_entrada->dims->size != 3 || s_entrada->dims->data[0] != 1 ||
        s_salida->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "Entrada/salida inesperada (tipo in=%d out=%d, dims=%d)",
                 s_entrada->type, s_salida->type, s_entrada->dims->size);
        return ESP_FAIL;
    }
    s_stride = s_entrada->dims->data[1];
    info->stride = s_stride;
    info->features = s_entrada->dims->data[2];
    info->escala_in = s_entrada->params.scale;
    info->cero_in = s_entrada->params.zero_point;
    info->cero_out = s_salida->params.zero_point;
    info->arena_usada = s_interprete->arena_used_bytes();
    ESP_LOGI(TAG, "Modelo %u B (%s), arena %u/%u B (%s), entrada [1,%d,%d] escala=%.8f cero=%d",
             (unsigned)tflite_len, info->modelo_interno ? "interna" : "PSRAM",
             (unsigned)info->arena_usada, (unsigned)arena_bytes, arena_interna ? "interna" : "PSRAM",
             info->stride, info->features, (double)info->escala_in, (int)info->cero_in);
    return ESP_OK;
}

extern "C" esp_err_t ww_modelo_invoke(const int8_t *features, uint8_t *prob_u8)
{
    memcpy(s_entrada->data.int8, features, (size_t)s_stride * s_entrada->dims->data[2]);
    if (s_interprete->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }
    *prob_u8 = s_salida->data.uint8[0];
    return ESP_OK;
}
