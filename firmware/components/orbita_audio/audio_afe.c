#include <stdlib.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
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

    // Config objetivo: beamforming (SE) + VAD, sin NS (decision del 12/10). El NS queda afuera.
    // MEDIDO antes: con SE activo y sin WakeNet la salida era el mic 0 SIN procesar (identica bit a
    // bit al crudo, 64 ms mas tarde). Ahora se vuelve a probar mirando el log de afe_config_print
    // (antes y despues de afe_config_check) para ver si el AFE apaga algo por su cuenta.
    cfg->se_init = true;
    cfg->ns_init = false;
    cfg->vad_init = true;
    cfg->fixed_output_channel = false;   // probado: solo, no cambio nada (salida = mic 0)
    cfg->fixed_first_channel = false;    // prueba: el canal lo elige el VAD, no queda fijo en el mic
    cfg->vad_enable_channel_trigger = true;

    // Revisa conflictos entre opciones y las corrige solo (ver el log).
    // OJO: con 2 mics, beamforming (SE) tiene prioridad sobre NS.
    ESP_LOGI(TAG, "Config pedida (antes de afe_config_check):");
    afe_config_print(cfg);
    cfg = afe_config_check(cfg);
    ESP_LOGI(TAG, "Config final (despues de afe_config_check):");
    afe_config_print(cfg);
    // Por la ROM, para verlo aunque los logs normales no salgan en el monitor.
    esp_rom_printf("AFE config final: se=%d vad=%d ns=%d wakenet=%d aec=%d fixed_out=%d fixed_first=%d vad_trig=%d\n",
                   cfg->se_init, cfg->vad_init, cfg->ns_init, cfg->wakenet_init, cfg->aec_init,
                   cfg->fixed_output_channel, cfg->fixed_first_channel, cfg->vad_enable_channel_trigger);

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
    esp_rom_printf("AFE listo: feed %d muestras x %d canales, fetch %d muestras x %d canal(es)\n",
                   s_afe->get_feed_chunksize(s_data), s_afe->get_feed_channel_num(s_data),
                   s_afe->get_fetch_chunksize(s_data), s_afe->get_fetch_channel_num(s_data));
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
