#include "audio_ring_out.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

/* RING OUT: (SIN AFE) feed -->ring_out--> sink | (CON AFE) fetch -->ring_out--> sink
 *  Dato:        ring propio. audio en SLOTS de 512 muestras mono, 16 bit c/muestra + header de 16 B
 *  Item:        1 SLOT = 1040 B (header 16 B + 512 muestras = 1024 B = 32 ms)
 *  Capacidad:   1024 SLOTS ≈ 1,04 MB = 32,8 s (en PSRAM)
 *  Productor:   tarea_feed (sin AFE) o tarea_fetch (con AFE), core 1, prio 5
 *  Consumidor:  sink (A7), core 0 (propuesta)
 *  Si se llena: se descarta el slot mas antiguo y sube `descartados`
 *  Lock:        spinlock (portMUX): interrupciones apagadas ~20-50 us por llamada (memcpy de a lo sumo 1 KB).
 *               Sin mutex: si WiFi/TLS (prio mayor) desalojan al sink con el mutex tomado, el productor
 *               espera o pierde el slot. Con el spinlock nadie desaloja al que lo tiene. */

static const char *TAG = "ring_out";

typedef struct {
    orbita_slot_t *slots;         // arreglo de ORBITA_RING_SLOTS slots, reservado en PSRAM por init
    uint16_t       write_idx;     // donde se escribe el proximo slot (0..ORBITA_RING_SLOTS-1)
    uint16_t       read_idx;      // el slot mas viejo, el proximo que lee el sink
    uint16_t       ocupados;      // slots que esperan al sink
    uint16_t       max_ocupados;  // maximo historico
    uint32_t       next_seq;      // seq del proximo slot
    uint32_t       escritos;
    uint32_t       descartados;
} orbita_ring_out_t;

static orbita_ring_out_t s_ring;     // solo un puntero y enteros: ocupa pocos bytes de RAM interna
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // protege todo s_ring y la copia del slot

static inline uint16_t siguiente(uint16_t idx)
{
    return (uint16_t)((idx + 1) % ORBITA_RING_SLOTS);   // vuelve a 0 despues del ultimo slot
}

// Reserva los slots en PSRAM (~1 MB). Llamar UNA vez, ANTES de crear las tareas que usan el ring
// (productor y sink) y antes de iniciar el AFE (el bloque es contiguo: con la PSRAM fragmentada puede fallar).
esp_err_t orbita_ring_out_init(void)
{
    if (s_ring.slots != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // calloc = malloc + todo en 0. MALLOC_CAP_SPIRAM = en la PSRAM, no en la RAM interna (la escasa).
    orbita_slot_t *slots = heap_caps_calloc(ORBITA_RING_SLOTS, sizeof(orbita_slot_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (slots == NULL) {
        ESP_LOGE(TAG, "sin PSRAM para %d slots (%u B)", ORBITA_RING_SLOTS,
                 (unsigned)(ORBITA_RING_SLOTS * sizeof(orbita_slot_t)));
        return ESP_ERR_NO_MEM;
    }

    s_ring = (orbita_ring_out_t){ .slots = slots };   // el resto queda en 0; `slots` != NULL = listo
    return ESP_OK;
}

esp_err_t orbita_ring_out_write(const int16_t *audio, uint16_t n_samples, uint8_t flags, uint32_t lost_samples)
{
    if (s_ring.slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio == NULL || n_samples == 0 || n_samples > ORBITA_SLOT_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);

    // Lleno: se pisa el mas viejo. Sus muestras pasan a lost_samples del que queda primero.
    if (s_ring.ocupados == ORBITA_RING_SLOTS) {
        const orbita_slot_hdr_t *viejo = &s_ring.slots[s_ring.read_idx].hdr;
        uint32_t perdido = viejo->lost_samples + viejo->n_samples;
        s_ring.read_idx = siguiente(s_ring.read_idx);
        s_ring.slots[s_ring.read_idx].hdr.lost_samples += perdido;
        s_ring.ocupados--;
        s_ring.descartados++;
    }

    orbita_slot_t *slot = &s_ring.slots[s_ring.write_idx];
    memcpy(slot->audio, audio, (size_t)n_samples * sizeof(int16_t));
    slot->hdr = (orbita_slot_hdr_t){
        .seq          = s_ring.next_seq++,
        .lost_samples = lost_samples,
        .n_samples    = n_samples,
        .flags        = flags,
        .version      = ORBITA_RING_VERSION,
    };

    s_ring.write_idx = siguiente(s_ring.write_idx);
    s_ring.ocupados++;
    if (s_ring.ocupados > s_ring.max_ocupados) {
        s_ring.max_ocupados = s_ring.ocupados;
    }
    s_ring.escritos++;

    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t orbita_ring_out_read(orbita_slot_t *dst)
{
    if (s_ring.slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mux);

    if (s_ring.ocupados == 0) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_NOT_FOUND;
    }

    // Solo header + muestras validas: el resto del slot es audio viejo de otra vuelta del ring
    // y no debe llegar al sink (privacidad). El sink usa siempre hdr.n_samples.
    const orbita_slot_t *slot = &s_ring.slots[s_ring.read_idx];
    memcpy(dst, slot, sizeof(orbita_slot_hdr_t) + (size_t)slot->hdr.n_samples * sizeof(int16_t));
    s_ring.read_idx = siguiente(s_ring.read_idx);
    s_ring.ocupados--;

    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

uint16_t orbita_ring_out_quedarse_con(uint16_t n)
{
    if (s_ring.slots == NULL) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    uint16_t sobran = s_ring.ocupados > n ? (uint16_t)(s_ring.ocupados - n) : 0;
    // Avanzar el indice de lectura `sobran` lugares (con la vuelta del ring): no se copia nada.
    s_ring.read_idx = (uint16_t)((s_ring.read_idx + sobran) % ORBITA_RING_SLOTS);
    s_ring.ocupados -= sobran;
    if (s_ring.ocupados > 0) {
        s_ring.slots[s_ring.read_idx].hdr.lost_samples = 0;     // corte a proposito, no perdida
    }
    portEXIT_CRITICAL(&s_mux);
    return sobran;
}

void orbita_ring_out_get_stats(orbita_ring_out_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_ring.slots == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    out->ocupados     = s_ring.ocupados;
    out->max_ocupados = s_ring.max_ocupados;
    out->escritos     = s_ring.escritos;
    out->descartados  = s_ring.descartados;
    portEXIT_CRITICAL(&s_mux);
}
