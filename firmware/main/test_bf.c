// #############################################################################
// ##  PRUEBA DE BEAMFORMING por UART0 (ver test_bf.h). Solo laboratorio.    ##
// #############################################################################
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "pipeline.h"
#include "beamforming.h"
#include "test_bf.h"

#if ORBITA_PIPELINE_USA_AFE
#error "La prueba de beamforming va con el AFE apagado (menuconfig -> Orbita pipeline)"
#endif

static const char *TAG = "test_bf";

// --- Cable (igual que test.c) ---
#define BF_UART_NUM          UART_NUM_0
#define BF_UART_BAUD_AUDIO   921600
#define BF_UART_BAUD_LOG     115200

/* TOMA en PSRAM
 *  Largo:    80 chunks x 1024 frames = 81 920 frames = 5,12 s (chunks enteros: 5 s justos no da)
 *  Buffer:   estereo int16 tal cual sale de la captura (ya con el pasaaltos de 70 Hz): 80 x 4096 B = 320 KB, PSRAM
 *  Salida:   WAV 3 canales x 16 bit x 16 kHz = 96 KB/s -> 480 KB, ~5,3 s a 921600 baud */
#define BF_TOMA_CHUNKS       80
#define BF_TOMA_FRAMES       (BF_TOMA_CHUNKS * ORBITA_CHUNK_FRAMES)
#define BF_CANALES_WAV       3

/* PARAMETROS DEL BEAMFORMER (artifact "Array diferencial de 2 mics", 29/10)
 *  Geometria: mics en linea a d = 21,44 mm (1 muestra). Con otra d los nulos se corren.
 *  Adelante:  slot L = mic de adelante (apunta al que habla). Cambiar si se monto al reves.
 *  beta:      0,5 hipercardioide (0 cardioide, 1 dipolo: mismo codigo, para comparar)
 *  fc:        150 Hz el integrador; 0 = sin ecualizador (sale la resta cruda, +6 dB/oct)
 *  ganancia:  1,0 = sin calibrar (falta la calibracion con Goertzel) */
#define BF_ADELANTE_ES_L     true
#define BF_BETA              0.5f
#define BF_FC_HZ             150.0f
#define BF_GANANCIA_ATRAS    1.0f

static int16_t *s_toma;                       // BF_TOMA_FRAMES frames estereo, en PSRAM
static volatile bool s_grabando;              // main -> captura: copiar chunks a s_toma
static volatile uint32_t s_chunks_grabados;   // captura -> main
static volatile uint32_t s_huecos;            // chunks que llegaron con hueco durante la toma
static char s_ultima_toma[400] = "Fin: sin tomas todavia";

// =============================================================================
//  CALLBACK DE LA CAPTURA: solo copia el chunk (rapido, no bloquea)
// =============================================================================
static void bf_on_crudo(const int16_t *chunk_ini, size_t frames, bool hueco)
{
    if (!s_grabando || frames != ORBITA_CHUNK_FRAMES) {
        return;
    }
    uint32_t i = s_chunks_grabados;
    // El primer chunk de la toma siempre puede venir con hueco (antes no se grababa): no cuenta.
    if (hueco && i > 0) {
        s_huecos++;
    }
    memcpy(&s_toma[i * ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS], chunk_ini, ORBITA_CHUNK_BYTES);
    s_chunks_grabados = i + 1;
    if (s_chunks_grabados >= BF_TOMA_CHUNKS) {
        s_grabando = false;                   // listo: la captura deja de copiar
    }
}

static const orbita_pipeline_cfg_t s_pipeline_cfg = { .on_crudo = bf_on_crudo };

const orbita_pipeline_cfg_t *test_bf_pipeline_cfg(void)
{
    return &s_pipeline_cfg;
}

// =============================================================================
//  API
// =============================================================================
void test_bf_init(void)
{
    esp_err_t err = uart_driver_install(BF_UART_NUM, 256, 8192, 0, NULL, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART0 ya tenia driver (consola): se reusa.");
    } else {
        ESP_ERROR_CHECK(err);
    }
    usb_serial_jtag_driver_config_t usb_cfg = { .rx_buffer_size = 256, .tx_buffer_size = 1024 };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    s_toma = heap_caps_malloc((size_t)BF_TOMA_FRAMES * ORBITA_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_toma != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Toma de %u frames (%.2f s) en PSRAM. beta=%.2f fc=%.0f Hz adelante=%s",
             (unsigned)BF_TOMA_FRAMES, (double)BF_TOMA_FRAMES / ORBITA_SAMPLE_RATE_HZ,
             (double)BF_BETA, (double)BF_FC_HZ, BF_ADELANTE_ES_L ? "L" : "R");
}

// Una toma: grabar 5,12 s -> beamformer por chunks -> WAV [L, R, BF] por UART0.
static void test_bf_tomar(void)
{
    static uint8_t wav_header[WAV_HEADER_SIZE];
    static int16_t mono[ORBITA_CHUNK_FRAMES];                       // salida del beamformer, un chunk
    static int16_t salida[ORBITA_CHUNK_FRAMES * BF_CANALES_WAV];    // frames [L, R, BF]; static: fuera del stack

    // ---------- 1. Grabar (la captura copia a PSRAM; aca solo se espera) ----------
    orbita_pipeline_stats_t ini, fin;
    orbita_pipeline_get_stats(&ini);
    s_huecos = 0;
    s_chunks_grabados = 0;
    s_grabando = true;                        // ultimo: la captura arranca con contadores en cero
    while (s_grabando) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    orbita_pipeline_get_stats(&fin);

    // ---------- 2. Aviso a la PC, header y UART en modo audio ----------
    const char *marcador = "WAV_ON_UART0\n";
    usb_serial_jtag_write_bytes(marcador, strlen(marcador), portMAX_DELAY);
    uint32_t data_size = (uint32_t)BF_TOMA_FRAMES * BF_CANALES_WAV * ORBITA_BYTES_PER_SAMPLE;
    wav_build_header(wav_header, ORBITA_SAMPLE_RATE_HZ, BF_CANALES_WAV, ORBITA_BYTES_PER_SAMPLE * 8, data_size);

    esp_log_level_set("*", ESP_LOG_NONE);     // ningun log por UART0 hasta terminar (se mezclaria con el audio)
    uart_set_baudrate(BF_UART_NUM, BF_UART_BAUD_AUDIO);
    uart_write_bytes(BF_UART_NUM, (const char *)wav_header, WAV_HEADER_SIZE);

    // ---------- 3. Beamformer + envio, chunk por chunk ----------
    orbita_bf_t bf;                           // estado nuevo por toma: la toma anterior no es continua con esta
    orbita_bf_init(&bf, BF_ADELANTE_ES_L, BF_GANANCIA_ATRAS, BF_BETA, BF_FC_HZ, ORBITA_SAMPLE_RATE_HZ);
    uint32_t bf_us_max = 0, enviados = 0;

    for (uint32_t c = 0; c < BF_TOMA_CHUNKS; c++) {
        const int16_t *estereo = &s_toma[c * ORBITA_CHUNK_FRAMES * ORBITA_CHANNELS];

        int64_t t0 = esp_timer_get_time();
        orbita_bf_procesar(&bf, estereo, mono, ORBITA_CHUNK_FRAMES);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > bf_us_max) {
            bf_us_max = us;
        }

        for (int k = 0; k < ORBITA_CHUNK_FRAMES; k++) {
            salida[3 * k]     = estereo[2 * k];      // ch0: mic L crudo
            salida[3 * k + 1] = estereo[2 * k + 1];  // ch1: mic R crudo
            salida[3 * k + 2] = mono[k];             // ch2: beamformado
        }
        uart_write_bytes(BF_UART_NUM, (const char *)salida, sizeof(salida));   // bloquea si el TX esta lleno
        enviados += sizeof(salida);
    }

    // ---------- 4. Cierre ----------
    uart_wait_tx_done(BF_UART_NUM, portMAX_DELAY);   // que salgan los ultimos bytes antes de bajar el baud
    uart_set_baudrate(BF_UART_NUM, BF_UART_BAUD_LOG);
    esp_log_level_set("*", ESP_LOG_INFO);

    // bf_us_max: cuanto tarda el beamformer en un chunk de 64 ms (en la PSRAM; en la captura seria similar o menos).
    snprintf(s_ultima_toma, sizeof(s_ultima_toma),
             "Fin: mandados %u de %u bytes, canales=[L,R,BF], huecos=%u, dma_overflows=%u, i2s_errores=%u, "
             "bf_us_max=%u por chunk de 64 ms, beta=%.2f fc=%.0f adelante=%s ganancia_atras=%.3f",
             (unsigned)enviados, (unsigned)data_size, (unsigned)s_huecos,
             (unsigned)(fin.dma_overflows - ini.dma_overflows), (unsigned)(fin.i2s_errores - ini.i2s_errores),
             (unsigned)bf_us_max, (double)BF_BETA, (double)BF_FC_HZ, BF_ADELANTE_ES_L ? "L" : "R",
             (double)BF_GANANCIA_ATRAS);
    ESP_LOGI(TAG, "%s", s_ultima_toma);
}

void test_bf_loop(void)
{
    uint8_t cmd;
    while (1) {
        int n = usb_serial_jtag_read_bytes(&cmd, 1, 0);
        if (n == 1 && cmd == 'g') {
            test_bf_tomar();
        } else if (n == 1 && cmd == 's') {
            usb_serial_jtag_write_bytes(s_ultima_toma, strlen(s_ultima_toma), portMAX_DELAY);
            usb_serial_jtag_write_bytes("\n", 1, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
