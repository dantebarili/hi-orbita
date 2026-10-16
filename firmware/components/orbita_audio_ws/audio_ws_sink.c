#include "sdkconfig.h"
#if CONFIG_ORBITA_LAB_WEBSOCKET
/* ADAPTADOR: audio_sink.c -> send(ctx,msg) -> orbita_ws_send -> servidor.   Protocolo orbita-pcm-v2.
 * Sesion continua: UN `hello` por conexion, audio sin fin, sin `end`. El servidor responde
 * `ready` y manda `ack` cada ~1 s; aca solo se cuentan (sirven para el reenvio, A6).
 * Si la red cae, send_audio espera con el mensaje en la mano y el ring acumula (S1);
 * al reconectar se abre sesion nueva (id nuevo) y el servidor mide el hueco por `seq`.
 * El buffer msg->audio solo vale en send: serializamos y enviamos antes de volver.
 */
#include "audio_ws_sink.h"
#include "audio_ws_wire.h"
#include "orbita_ws.h"
#include "pipeline.h"
#include "audio_ring_out.h"
#include "audio_capture.h"
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

// WARM-UP: al arrancar el microfono hay un escalon de continua que decae en ~5 s
// (medido el 13/10) y el DMA del I2S pisa audio mientras se crean las tareas del pipeline.
// Ese audio se descarta antes de mandar nada, aunque todavia no haya conexion.
#define WARMUP_MS      5000
#define WARMUP_SAMPLES (WARMUP_MS * 16)     // 16 muestras por ms (16 kHz mono)
#define STATS_LOG_MS   30000                // cada cuanto se imprime el resumen en el monitor
// Si mientras el sink retenia un mensaje el ring pisó al menos esta cantidad de slots (~1 s), el mensaje
// retenido quedo aislado de lo que sigue por un salto grande: se descarta en vez de mandar un fragmento viejo.
#define STALE_SLOTS    32
// Timeout de cada envio. Mas largo que una recuperacion normal de TCP tras perder un paquete (1-3 s): con 1 s
// abortabamos conexiones que se hubieran recuperado solas, y cada aborto cuesta 8 s de backoff y audio perdido.
#define SEND_TIMEOUT_MS 5000
// Ventana: maximo de mensajes enviados sin confirmar (un ack llega cada 8). Frena el drenado del atraso al ritmo
// del servidor en vez de llenar el buffer TCP. Si el servidor no confirma en ACK_STALL_MS se apaga en esa sesion.
#define ACK_WINDOW      32
#define ACK_STALL_MS    5000

// Todo este estado lo usa SOLO tarea_sink; buffers fuera de su pila.
static struct {
    bool active, waiting;
    TickType_t wait_since;                  // inicio de la espera sin red en curso
    TickType_t last_log;                    // ultimo resumen impreso
    uint32_t warmup_skipped, dma_base;      // muestras descartadas / overflows del DMA hasta fin del warm-up
    uint32_t reconnects, wait_max_ms;       // veces que volvio la red / espera mas larga
    uint32_t connection, index, samples;    // sesion actual: conexion local, mensajes y muestras enviados
    uint32_t hello_conn;                    // conexion en la que ya mandamos `hello` (0 = ninguna): el servidor acepta UNO por conexion
    int32_t last_ack;                       // ultimo indice confirmado por el servidor (-1 = ninguno)
    uint32_t acks, ignored;                 // acks recibidos / mensajes del servidor que no entendimos
    uint32_t window_waits;                  // veces que se esperó por la ventana de acks
    bool no_window;                         // el servidor no confirma: sin control de ventana en esta sesion
    uint64_t id, dropped_samples;
    uint32_t failed_attempts;
    uint8_t packet[ORBITA_WIRE_HEADER + ORBITA_WIRE_MAX_SAMPLES*2];
    char control[256];
    orbita_ws_message_t reply;
} s = { .last_ack = -1 };

// Busca `needle` dentro de un mensaje de texto. El texto recibido NO termina en '\0'.
static bool has_text(const orbita_ws_message_t *m, const char *needle)
{
    size_t n = strlen(needle);
    if (m->type != ORBITA_WS_TEXT || m->size < n) return false;
    for (size_t i = 0; i + n <= m->size; i++) {
        if (!memcmp(m->data + i, needle, n)) return true;
    }
    return false;
}

// Mensaje del servidor ya retirado de la cola. Hoy solo entendemos `ack`.
static void on_server_message(const orbita_ws_message_t *m)
{
    if (!has_text(m, "\"type\":\"ack\"")) { ++s.ignored; return; }
    static const char key[] = "\"index\":";
    for (size_t i = 0; i + sizeof(key) - 1 <= m->size; i++) {
        if (memcmp(m->data + i, key, sizeof(key) - 1)) continue;
        uint32_t v = 0;
        size_t digitos = 0;
        for (size_t j = i + sizeof(key) - 1; j < m->size && m->data[j] >= '0' && m->data[j] <= '9'; j++) {
            if (++digitos > 9) { digitos = 0; break; }          // mas de 9 digitos: no es un indice nuestro
            v = v * 10 + (uint32_t)(m->data[j] - '0');
        }
        if (digitos == 0) break;                                // sin digitos (o demasiados): ack invalido
        if (s.last_ack < 0) orbita_ws_backoff_reset();          // primer ack de la sesion: conexion estable
        s.last_ack = (int32_t)v;
        ++s.acks;
        return;
    }
    ++s.ignored;
}

// Vacia la cola de recepcion sin esperar (si se llena, el callback descarta los nuevos).
static void drain_server_messages(void)
{
    for (int i = 0; i < 8; i++) {
        esp_err_t err = orbita_ws_receive(&s.reply, 0);
        if (err == ESP_OK) on_server_message(&s.reply);
        else if (err != ESP_ERR_INVALID_STATE) break;       // TIMEOUT = cola vacia
    }
}

// Resumen de una linea en el monitor cada STATS_LOG_MS (reemplaza al JSON del `end` del v1).
static void log_stats(void)
{
    TickType_t now_t = xTaskGetTickCount();
    if ((TickType_t)(now_t - s.last_log) < pdMS_TO_TICKS(STATS_LOG_MS)) return;
    s.last_log = now_t;
    orbita_pipeline_stats_t now;
    orbita_ring_out_stats_t ring;
    orbita_sink_stats_t sink;
    orbita_pipeline_get_stats(&now);
    orbita_ring_out_get_stats(&ring);
    orbita_sink_get_stats(&sink);
    ESP_LOGI(TAG, "msgs=%" PRIu32 " ack=%" PRId32 " sin_ack=%" PRId32 " reconex=%" PRIu32
             " espera_max=%" PRIu32 "ms vent=%" PRIu32 " desc_sink=%" PRIu64 " ring(max=%u desc=%" PRIu32 ")"
             " dma=%" PRIu32 " i2s=%" PRIu32 " pila_sink=%" PRIu32 " ram_int=%u",
             s.index, s.last_ack, (int32_t)s.index - (s.last_ack + 1), s.reconnects,
             s.wait_max_ms, s.window_waits, s.dropped_samples, (unsigned)ring.max_ocupados, ring.descartados,
             now.dma_overflows - s.dma_base, now.i2s_errores, sink.stack_libre,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// Espera el `ready` de nuestro id en esta conexion, hasta timeout_ms. Ignora otros mensajes
// (un `ready` viejo o un ack): no cuestan un intento.
static bool wait_ready(uint32_t connection, uint32_t timeout_ms)
{
    char want[40];
    snprintf(want, sizeof(want), "\"id\":\"%016" PRIx64 "\"", s.id);
    TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        uint32_t usado = (uint32_t)(xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
        if (usado >= timeout_ms) return false;
        if (orbita_ws_receive(&s.reply, timeout_ms - usado) != ESP_OK ||
            s.reply.connection_id != connection) return false;
        if (has_text(&s.reply, "\"type\":\"ready\"") && has_text(&s.reply, want)) return true;
    }
}

// Abre la sesion: manda `hello` (UNO por conexion) y espera `ready`. Id nuevo por conexion.
// Si el ready no llega, la proxima llamada en la misma conexion solo vuelve a esperarlo: un segundo
// hello lo rechaza el servidor y cerraria la conexion.
static bool start_session(uint32_t connection)
{
    if (s.hello_conn != connection) {
        s.id = ((uint64_t)esp_random() << 32) | esp_random();
        s.connection = connection;
        s.index = s.samples = 0;
        s.last_ack = -1;
        s.no_window = false;
        int n = snprintf(s.control, sizeof(s.control),
            "{\"type\":\"hello\",\"protocol\":\"orbita-pcm-v2\",\"id\":\"%016" PRIx64
            "\",\"rate\":16000,\"channels\":1,\"format\":\"s16le\",\"source\":\"%s\"}",
            s.id, ORBITA_PIPELINE_USA_AFE ? "afe" : ORBITA_RAW_MIC_SOURCE);
        if (n <= 0 || (size_t)n >= sizeof(s.control) ||
            orbita_ws_send(ORBITA_WS_TEXT, s.control, n, SEND_TIMEOUT_MS) != ESP_OK) return false;
        s.hello_conn = connection;
    }
    if (!wait_ready(connection, 5000)) return false;
    s.active = true;
    ESP_LOGI(TAG, "Sesion %016" PRIx64 " abierta", s.id);
    return true;
}

static esp_err_t send_audio(void *ctx, const orbita_msg_t *msg)
{
    (void)ctx;
    // El ESP_OK aqui significa consumido; el contador sink.msgs_enviados NO equivale
    // a paquetes de red. Los reales son s.index (por sesion) y el resumen del monitor.
    if (s.warmup_skipped < WARMUP_SAMPLES) {            // descartar el arranque (ver WARMUP_MS)
        s.warmup_skipped += msg->n_samples;
        if (s.warmup_skipped >= WARMUP_SAMPLES) {
            s.dma_base = orbita_audio_get_dma_overflows();
            ESP_LOGI(TAG, "Warm-up terminado (%u ms descartados); dma_overflows durante el arranque=%" PRIu32,
                     (unsigned)(s.warmup_skipped / 16), s.dma_base);
        }
        return ESP_OK;
    }
    // S1 (retencion): este mensaje NO se descarta si falla la red. Se queda en la mano
    // de la tarea sink hasta enviarse; mientras tanto la captura sigue llenando el ring
    // (32 s de colchon; si se llena, el ring descarta lo mas viejo y lo marca en `lost`).
    // Si el envio fallo a medias, el servidor puede haberlo recibido; como la sesion nueva
    // lo manda de nuevo en otra conexion, nunca ve el mismo index dos veces.
    orbita_ring_out_stats_t ring0;                          // pisados del ring al empezar a retener
    orbita_ring_out_get_stats(&ring0);
    bool retenido = false;                                  // true si este mensaje ya tuvo que esperar
    for (;;) {
        log_stats();
        uint32_t connection = orbita_ws_connection_id();
        if (s.active && connection != s.connection) {
            s.active = false; ++s.failed_attempts;
            ESP_LOGW(TAG, "Cambio de conexion: sesion anterior cerrada tras %" PRIu32 " mensajes", s.index);
        }
        if (!connection) {                                  // sin red: esperar en tramos cortos
            if (!s.waiting) { s.waiting = true; s.wait_since = xTaskGetTickCount(); }
            orbita_ws_keepalive();                          // reinicia el cliente si su tarea murio
            retenido = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s.waiting) {                                    // volvio la red: cerrar la medicion
            uint32_t ms = (uint32_t)(xTaskGetTickCount() - s.wait_since) * portTICK_PERIOD_MS;
            if (ms > s.wait_max_ms) s.wait_max_ms = ms;
            ++s.reconnects; s.waiting = false;
            ESP_LOGW(TAG, "Red de vuelta tras %" PRIu32 " ms sin conexion", ms);
        }
        if (!s.active && !start_session(connection)) {      // sin ready todavia (o el envio del hello fallo)
            ++s.failed_attempts;
            retenido = true;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (retenido) {
            orbita_ring_out_stats_t ring;
            orbita_ring_out_get_stats(&ring);
            uint32_t pisados = ring.descartados - ring0.descartados;
            if (pisados >= STALE_SLOTS) {                   // el ring ya no tiene lo que sigue a este mensaje
                s.dropped_samples += msg->n_samples;
                ESP_LOGW(TAG, "Mensaje retenido descartado: el ring piso %" PRIu32 " slots mientras esperaba", pisados);
                return ESP_FAIL;
            }
        }
        // Ventana de acks: esperar confirmaciones si hay demasiados mensajes sin confirmar.
        if (s.active && !s.no_window && (int32_t)s.index - (s.last_ack + 1) >= ACK_WINDOW) {
            ++s.window_waits;
            TickType_t t0 = xTaskGetTickCount();
            while ((int32_t)s.index - (s.last_ack + 1) >= ACK_WINDOW) {
                drain_server_messages();
                if (orbita_ws_connection_id() != s.connection) break;    // se cayo: la vuelta siguiente reabre
                if ((uint32_t)(xTaskGetTickCount() - t0) * portTICK_PERIOD_MS > ACK_STALL_MS) {
                    s.no_window = true;
                    ESP_LOGW(TAG, "El servidor no confirma hace %d ms (sin_ack=%" PRId32 "): sigo sin ventana en esta sesion",
                             ACK_STALL_MS, (int32_t)s.index - (s.last_ack + 1));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;                                       // volver a evaluar conexion y ventana
        }
        orbita_wire_meta_t meta = {
            .stream_id=s.id, .index=s.index, .seq=msg->seq_primero, .lost=msg->lost_antes,
            .slots=msg->n_slots, .samples=msg->n_samples, .voice=msg->n_voz, .vad=msg->n_vad_ok,
        };
        size_t size = orbita_wire_encode(s.packet, sizeof(s.packet), &meta, msg->audio);
        if (!size) {                                        // mensaje invalido: no tiene arreglo, se descarta
            s.dropped_samples += msg->n_samples;
            return ESP_FAIL;
        }
        if (orbita_ws_connection_id() != s.connection ||
            orbita_ws_send(ORBITA_WS_BINARY, s.packet, size, SEND_TIMEOUT_MS) != ESP_OK) {
            // Si la conexion cambio, el tope de la vuelta cierra la sesion y abre una nueva. Si sigue la
            // misma, la biblioteca no mando nada (o ya cerro sola ante un error de escritura): se
            // reintenta el MISMO paquete, sin hello nuevo.
            ++s.failed_attempts;
            retenido = true;
            ESP_LOGW(TAG, "Envio fallido; se reintenta el mismo mensaje");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s.samples += msg->n_samples; ++s.index;
        drain_server_messages();                            // acks del servidor
        return ESP_OK;
    }
}

const orbita_sink_t *orbita_audio_ws_sink(void)
{
    static const orbita_sink_t sink = { .send=send_audio, .ctx=NULL };
    return &sink;
}

#endif
