#include "sdkconfig.h"
#if CONFIG_ORBITA_LAB_WEBSOCKET
/* ADAPTADOR: audio_sink.c -> send(ctx,msg) -> orbita_ws_send -> servidor.
 * La tarea sink ya desacopla captura y red. No agregamos otra cola de audio.
 * El buffer msg->audio solo vale en send: serializamos y enviamos antes de volver.
 * Una toma completa por arranque. Un corte deja la anterior incompleta y abre
 * otra con ID nuevo. No prometemos retención/reenvío (A6 aún no implementado).
 */
#include "audio_ws_sink.h"
#include "audio_ws_wire.h"
#include "orbita_ws.h"
#include "pipeline.h"
#include "audio_ring_out.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

_Static_assert(ORBITA_MSG_SLOTS == 4, "Actualizar protocolo si cambia el agrupamiento");
_Static_assert(ORBITA_SLOT_SAMPLES == 512, "Actualizar protocolo si cambia el slot");
_Static_assert(ORBITA_WIRE_HEADER + ORBITA_WIRE_MAX_SAMPLES*2 <= ORBITA_WS_MAX_MESSAGE_BYTES,
               "Mensaje no cabe en el transporte");
static const char *TAG = "audio_ws";

// Todo este estado lo usa SOLO tarea_sink; buffers fuera de su pila de 4 KB.
static struct {
    bool active, done, cooling;
    TickType_t failed_at;
    uint32_t connection, index, samples, crc;
    uint64_t id, dropped_samples, bypassed_samples;
    uint32_t failed_attempts;
    uint8_t packet[ORBITA_WIRE_HEADER + ORBITA_WIRE_MAX_SAMPLES*2];
    char control[1600];
    orbita_ws_message_t reply;
} s;

static bool receive_reply(const char *kind, uint32_t connection, bool *gaps)
{
    if (orbita_ws_receive(&s.reply, 10000) != ESP_OK ||
        s.reply.type != ORBITA_WS_TEXT || s.reply.connection_id != connection) return false;
    char expected[80];
    int len = snprintf(expected, sizeof(expected), "%s %016" PRIx64, kind, s.id);
    if (len < 0 || s.reply.size < (size_t)len || memcmp(s.reply.data, expected, len)) return false;
    if (!gaps) return s.reply.size == (size_t)len;
    if (s.reply.size == (size_t)len+6 && !memcmp(s.reply.data+len, " clean", 6)) {
        *gaps = false; return true;
    }
    if (s.reply.size == (size_t)len+5 && !memcmp(s.reply.data+len, " gaps", 5)) {
        *gaps = true; return true;
    }
    return false;
}

static bool send_control(int size)
{
    return size > 0 && (size_t)size < sizeof(s.control) &&
        orbita_ws_send(ORBITA_WS_TEXT, s.control, size, 1000) == ESP_OK;
}

static bool start_stream(uint32_t connection)
{
    s.id = ((uint64_t)esp_random() << 32) | esp_random();
    s.connection = connection;
    s.index = s.samples = s.crc = 0;
    int n = snprintf(s.control, sizeof(s.control),
        "{\"type\":\"start\",\"protocol\":\"orbita-pcm-v1\",\"id\":\"%016" PRIx64
        "\",\"rate\":16000,\"channels\":1,\"format\":\"s16le\",\"target_samples\":%u,"
        "\"source\":\"%s\",\"dropped_before\":%" PRIu64 ",\"failed_attempts\":%" PRIu32 "}",
        s.id, (unsigned)(CONFIG_ORBITA_WS_CAPTURE_SECONDS * 16000),
        ORBITA_PIPELINE_USA_AFE ? "afe" : "mic_l", s.dropped_samples, s.failed_attempts);
    if (!send_control(n) || !receive_reply("ready", connection, NULL)) return false;
    s.active = true;
    ESP_LOGI(TAG, "Toma %016" PRIx64 " iniciada (%d s nominales)", s.id, CONFIG_ORBITA_WS_CAPTURE_SECONDS);
    return true;
}

static bool finish_stream(void)
{
    orbita_pipeline_stats_t now;
    orbita_ring_out_stats_t ring;
    orbita_sink_stats_t sink;
    orbita_pipeline_get_stats(&now);
    orbita_ring_out_get_stats(&ring);
    orbita_sink_get_stats(&sink);
    // Contadores desde arranque: incluyen errores previos al primer paquete y al ready.
    // Es conservador: una toma posterior puede estar bien y heredar un error anterior.
    int n = snprintf(s.control, sizeof(s.control),
        "{\"type\":\"end\",\"id\":\"%016" PRIx64 "\",\"messages\":%" PRIu32
        ",\"samples\":%" PRIu32 ",\"crc32\":%" PRIu32 ",\"stats\":{"
        "\"dma_overflows\":%" PRIu32 ",\"i2s_errors\":%" PRIu32
        ",\"ring_in_overruns\":%" PRIu32 ",\"invalid_chunks\":%" PRIu32
        ",\"invalid_fetch\":%" PRIu32 ",\"ring_out_errors\":%" PRIu32
        ",\"sink_invalid_slots\":%" PRIu32 ",\"ring_discarded_total\":%" PRIu32 ",\"ring_max_slots\":%u,"
        "\"capture_us_max\":%" PRIu32 ",\"feed_us_max\":%" PRIu32 ",\"fetch_us_max\":%" PRIu32
        ",\"send_us_max_previous\":%" PRIu32 ",\"stack_capture\":%" PRIu32
        ",\"stack_feed\":%" PRIu32 ",\"stack_fetch\":%" PRIu32 ",\"stack_sink\":%" PRIu32
        ",\"heap_internal_free\":%u,\"psram_free\":%u}}",
        s.id, s.index, s.samples, s.crc,
        now.dma_overflows, now.i2s_errores,
        now.overruns_ring_in, now.chunks_invalidos,
        now.fetch_invalidos, now.ring_out_errores,
        sink.slots_invalidos, ring.descartados, (unsigned)ring.max_ocupados, now.max_captura_us, now.max_feed_us, now.max_fetch_us,
        sink.max_send_us, now.stack_libre_captura, now.stack_libre_feed, now.stack_libre_fetch, sink.stack_libre,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    bool gaps = false;
    if (!send_control(n) || !receive_reply("saved", s.connection, &gaps)) return false;
    s.done = true; s.active = false;
    ESP_LOGI(TAG, "WAV GUARDADO %s: muestras=%" PRIu32 " mensajes=%" PRIu32
             "; descartadas/inciertas antes=%" PRIu64 ". Reiniciar para repetir.",
             gaps ? "CON PERDIDAS (ver JSON)" : "SIN PERDIDAS DETECTADAS", s.samples, s.index, s.dropped_samples);
    return true;
}

static esp_err_t send_audio(void *ctx, const orbita_msg_t *msg)
{
    (void)ctx;
    // Al terminar seguimos drenando para no provocar overruns artificiales del ring.
    // El ESP_OK aquí significa consumido; el contador sink.msgs_enviados NO equivale
    // a paquetes de red. Los reales son s.index y el resumen final del servidor.
    if (s.done) { s.bypassed_samples += msg->n_samples; return ESP_OK; }
    uint32_t connection = orbita_ws_connection_id();
    if (s.active && connection != s.connection) {
        s.active = false; ++s.failed_attempts;
        ESP_LOGW(TAG, "Cambio de conexion: toma anterior incompleta, sin reenviar");
    }
    if (!connection || (s.cooling && (TickType_t)(xTaskGetTickCount()-s.failed_at) < pdMS_TO_TICKS(5000))) {
        s.dropped_samples += msg->n_samples;
        return ESP_ERR_INVALID_STATE;
    }
    s.cooling = false;
    if (!s.active && !start_stream(connection)) goto failed;
    orbita_wire_meta_t meta = {
        .stream_id=s.id, .index=s.index, .seq=msg->seq_primero, .lost=msg->lost_antes,
        .slots=msg->n_slots, .samples=msg->n_samples, .voice=msg->n_voz, .vad=msg->n_vad_ok,
    };
    size_t size = orbita_wire_encode(s.packet, sizeof(s.packet), &meta, msg->audio);
    if (!size || orbita_ws_connection_id() != s.connection ||
        orbita_ws_send(ORBITA_WS_BINARY, s.packet, size, 1000) != ESP_OK) goto failed;
    s.crc = orbita_wire_crc(s.crc, s.packet+ORBITA_WIRE_HEADER, size-ORBITA_WIRE_HEADER);
    s.samples += msg->n_samples; ++s.index;
    if (s.samples >= CONFIG_ORBITA_WS_CAPTURE_SECONDS * 16000u && !finish_stream()) goto failed;
    return ESP_OK;
failed:
    // Error ambiguo: pudo llegar. No repetirlo ni concatenarlo en otra toma.
    s.dropped_samples += msg->n_samples; ++s.failed_attempts;
    s.active = false; s.cooling = true; s.failed_at = xTaskGetTickCount();
    ESP_LOGW(TAG, "Toma fallida/sin confirmacion; se intentara otra en 5 s. Ver parciales en servidor.");
    return ESP_FAIL;
}

const orbita_sink_t *orbita_audio_ws_sink(void)
{
    static const orbita_sink_t sink = { .send=send_audio, .ctx=NULL };
    return &sink;
}

#endif
