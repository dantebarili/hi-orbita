#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_sink.h"
#include "orbita_tiempo.h"

static const char *TAG = "sink";

/* SINK: ring_out --> tarea_sink --> msg (ORBITA_MSG_SLOTS slots juntos) --> sink.send
 *  Dato:        mensajes de hasta 4 x 512 muestras mono (4 KB), armados en un buffer propio
 *  Productor:   ring out (via orbita_ring_out_read)    Consumidor: sink.send (UART hoy, socket manana)
 *  Tarea:       core 0, prio 5, stack 4 KB (propuesta de arquitectura.md §3.1: red en el core 0)
 *  Ring vacio:  espera SINK_POLL_MS y reintenta (v1, sin notificacion)
 *  Si send falla: el mensaje se descarta y sube msgs_fallidos */

#define SINK_POLL_MS    10
#define SINK_STACK      4096
#define SINK_PRIO       5
#define SINK_CORE       0

static orbita_sink_t s_sink;
static TaskHandle_t  s_sink_handle;

// Buffers static: ni en el stack (1 KB + 4 KB lo agotarian) ni con malloc.
static orbita_slot_t s_slot;                                              // el slot recien leido
static int16_t       s_audio[ORBITA_MSG_SLOTS * ORBITA_SLOT_SAMPLES];     // audio del mensaje en curso
static orbita_msg_t  s_msg;                                               // el mensaje en curso

// Los escribe solo la tarea del sink.
static volatile uint32_t s_msgs_enviados;
static volatile uint32_t s_msgs_fallidos;
static volatile uint32_t s_slots_invalidos;
static volatile uint32_t s_max_read_us;     // tiempos maximos (A15)
static volatile uint32_t s_max_send_us;

// Entrega el mensaje en curso al sink y lo deja vacio.
static void cerrar_mensaje(void)
{
    s_msg.audio = s_audio;
    uint32_t t_send = orbita_t_us();
    esp_err_t err = s_sink.send(s_sink.ctx, &s_msg);
    orbita_max_us(&s_max_send_us, t_send);
    if (err == ESP_OK) {
        s_msgs_enviados++;
    } else {
        s_msgs_fallidos++;
    }
    s_msg = (orbita_msg_t){0};
}

static void tarea_sink(void *arg)
{
    while (1) {
        uint32_t t_read = orbita_t_us();
        esp_err_t err = orbita_ring_out_read(&s_slot);
        if (err == ESP_OK) {
            orbita_max_us(&s_max_read_us, t_read);   // solo lecturas con slot (el vacio no cuenta)
        }
        if (err != ESP_OK) {                         // vacio (lo normal) o ring sin iniciar
            vTaskDelay(pdMS_TO_TICKS(SINK_POLL_MS));
            continue;
        }
        if (s_slot.hdr.version != ORBITA_RING_VERSION) {
            s_slots_invalidos++;
            continue;
        }

        // Perdida antes de este slot: lo que haya juntado se manda aparte, para que cada mensaje
        // sea audio continuo y el hueco quede en `lost_antes` del mensaje siguiente.
        if (s_slot.hdr.lost_samples > 0 && s_msg.n_slots > 0) {
            cerrar_mensaje();
        }

        if (s_msg.n_slots == 0) {                    // primer slot del mensaje
            s_msg.seq_primero = s_slot.hdr.seq;
            s_msg.lost_antes  = s_slot.hdr.lost_samples;
        }

        // Se copian solo las muestras validas (n_samples), nunca el slot entero.
        memcpy(&s_audio[s_msg.n_samples], s_slot.audio, (size_t)s_slot.hdr.n_samples * sizeof(int16_t));
        s_msg.n_samples += s_slot.hdr.n_samples;
        s_msg.n_slots++;
        if (s_slot.hdr.flags & ORBITA_FLAG_VAD_OK) s_msg.n_vad_ok++;
        if (s_slot.hdr.flags & ORBITA_FLAG_VOZ)    s_msg.n_voz++;

        if (s_msg.n_slots == ORBITA_MSG_SLOTS) {
            cerrar_mensaje();
        }
    }
}

esp_err_t orbita_sink_start(const orbita_sink_t *sink)
{
    if (sink == NULL || sink->send == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sink_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sink = *sink;
    if (xTaskCreatePinnedToCore(tarea_sink, "sink", SINK_STACK, NULL, SINK_PRIO, &s_sink_handle, SINK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "no se pudo crear la tarea del sink");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void orbita_sink_reset_tiempos(void)
{
    s_max_read_us = 0;
    s_max_send_us = 0;
}

void orbita_sink_get_stats(orbita_sink_stats_t *out)
{
    *out = (orbita_sink_stats_t){0};
    out->msgs_enviados   = s_msgs_enviados;
    out->msgs_fallidos   = s_msgs_fallidos;
    out->slots_invalidos = s_slots_invalidos;
    out->max_read_us     = s_max_read_us;
    out->max_send_us     = s_max_send_us;
    if (s_sink_handle != NULL) {
        out->stack_libre = uxTaskGetStackHighWaterMark(s_sink_handle);
    }
}
