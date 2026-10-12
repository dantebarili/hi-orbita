#include <stdlib.h>
#include "esp_log.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "audio_afe.h"

static const char *TAG = "afe";

// STRUCTS del AFE:
// `s_afe` (tabla de funciones: feed, fetch, ...)
// `s_data` (la instancia, con su estado interno).
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_data = NULL;

esp_err_t orbita_afe_init(void)
{
    // --- Modelos ---
    // Lee la particion "model" (partitions.csv). Con WebRTC no se usa ningun
    // modelo, pero la config lo pide igual; con NSNet/VADNet si hace falta.
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGW(TAG, "No hay modelos en la particion 'model' (ok mientras usemos WebRTC)");
    }

    // --- Config ---
    // "MM" = 2 canales de microfono, entrelazados (L R L R ...), igual que
    // sale de nuestro pack_buf. Si algun dia hay parlante con AEC: "MMR".
    // AFE_TYPE_SR = pensado para reconocimiento de voz (no para llamadas).
    // LOW_COST = menos CPU/RAM; se compara contra HIGH_PERF en la Fase B.
    // Medido: con AFE_TYPE_SR la salida es el mic 0 sin procesar. AFE_TYPE_VC trabaja con
    // frames de 10 ms (feed de 160 muestras), no de 64 ms: no es un cambio directo.
    afe_config_t *cfg = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init fallo");
        return ESP_FAIL;
    }

    // La wake word la hace Edge Impulse, no WakeNet: se apaga para no gastar
    // CPU ni cargar modelos. Sin parlante no hay referencia, asi que sin AEC.
    cfg->wakenet_init = false;
    cfg->aec_init = false;

    // MEDIDO: con SE (beamforming) activo y sin WakeNet, la salida del AFE era el mic 0 SIN
    // procesar (identica bit a bit al crudo, 64 ms mas tarde), con SR y con distintos ajustes
    // de canal/VAD. EXPERIMENTO: sin beamforming ni VAD, solo el NS de WebRTC sobre el mic 0
    // (segun el header, con SE apagado el AFE usa solo el primer mic).
    cfg->se_init = false;
    cfg->ns_init = true;
    cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    cfg->vad_init = false;

    // Revisa conflictos entre opciones y las corrige solo (ver el log).
    // OJO: con 2 mics, beamforming (SE) tiene prioridad sobre NS.
    cfg = afe_config_check(cfg);
    afe_config_print(cfg);

    // --- Instancia ---
    // create_from_config reserva memoria y crea las tareas internas del AFE.
    s_afe = esp_afe_handle_from_config(cfg);
    s_data = s_afe->create_from_config(cfg);
    if (s_data == NULL) {
        ESP_LOGE(TAG, "create_from_config fallo (¿falta RAM?)");
        return ESP_ERR_NO_MEM;
    }

    // --- Tamaños de configuración ---
    // Los tamaños son en MUESTRAS POR CANAL (no bytes): un feed() lee
    // chunk * canales * 2 bytes.
    ESP_LOGI(TAG, "feed:  %d muestras/canal x %d canales, %d Hz",
             s_afe->get_feed_chunksize(s_data), s_afe->get_feed_channel_num(s_data),
             s_afe->get_samp_rate(s_data));
    ESP_LOGI(TAG, "fetch: %d muestras, %d canal(es)",
             s_afe->get_fetch_chunksize(s_data), s_afe->get_fetch_channel_num(s_data));
    s_afe->print_pipeline(s_data);
    return ESP_OK;
}

int orbita_afe_feed_chunk_frames(void){
    if (s_data == NULL) {
        return 0;
    }

    int frames = s_afe->get_feed_chunksize(s_data);   // frames (muestras por canal) por feed()

    return frames;
}

// Función que alimenta a feed con un chunk
void orbita_afe_feed(const int16_t* chunk){
    
    if(s_data == NULL){
        return; // todavía no hay instancia del AFE, no se creó
    }

    s_afe->feed(s_data, chunk);
    
    return;
}

int orbita_afe_fetch_chunk_frames(void){
    if (s_data == NULL) {
        return 0;
    }
    return s_afe->get_fetch_chunksize(s_data);
}

// Pide un resultado al AFE. Se esconde afe_fetch_result_t: main.c solo ve
// un puntero al audio y su tamaño.
esp_err_t orbita_afe_fetch(const int16_t **audio, size_t *bytes, uint32_t timeout_ms, bool *voz)
{
    if (s_data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    afe_fetch_result_t *res = s_afe->fetch_with_delay(s_data, pdMS_TO_TICKS(timeout_ms));

    // Sin resultado, con error o sin datos: desde aca no se distinguen (a verificar
    // cual de los tres da el vencimiento de tiempo), asi que cuentan como "sin audio".
    if (res == NULL || res->ret_value != ESP_OK || res->data == NULL) {
        return ESP_ERR_TIMEOUT;
    }

    *audio = res->data;          // memoria del AFE: vale hasta el proximo fetch
    *bytes = (size_t)res->data_size;
    if (voz != NULL) {
        *voz = (res->vad_state == VAD_SPEECH);
    }
    return ESP_OK;
}
