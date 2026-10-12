// #############################################################################
// ##  MODULO DE LABORATORIO: tomas de test por UART0  -  SE BORRA ENTERO    ##
// ##  (ver test.h). Nada de esto queda en el producto.                      ##
// #############################################################################
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "pipeline.h"
#include "test.h"

static const char *TAG = "test";

// --- Cable ---
#define TEST_UART_NUM        UART_NUM_0
#define TEST_UART_BAUD_AUDIO 921600         // durante la toma (audio)
#define TEST_UART_BAUD_LOG   115200         // el resto del tiempo (logs)

// --- La toma se cuenta en BLOQUES de 512 muestras (= un fetch del AFE) ---
// Ambas fuentes entregan bloques de 1024 B (512 muestras mono): el envio intercala bloque a
// bloque [ch0, ch1, ch0, ch1 ...] = WAV de 2 canales, 64 KB/s.
#define BLOQUE_MUESTRAS     ORBITA_AFE_FETCH_FRAMES
#define BLOQUE_BYTES        ORBITA_AFE_FETCH_BYTES
#define BLOQUES_POR_CHUNK   (ORBITA_CHUNK_FRAMES / BLOQUE_MUESTRAS)
#define TOMA_SEGUNDOS       20                 // largo de la toma (multiplo de 4 s: ver assert)
#define TOMA_BLOQUES        ((TOMA_SEGUNDOS * ORBITA_SAMPLE_RATE_HZ) / BLOQUE_MUESTRAS)
_Static_assert((TOMA_SEGUNDOS * ORBITA_SAMPLE_RATE_HZ) % BLOQUE_MUESTRAS == 0,
               "TOMA_SEGUNDOS debe dar un numero entero de bloques de 512 muestras (multiplo de 4 s)");
#define TEST_BLOQUES     32                 // capacidad de cada ring de test (~1 s)
#define VAD_LOG_BLOQUES     64                 // solo se registran los primeros bloques (el texto de "Fin:" no crece con la toma)

static RingbufHandle_t s_ring_ch0;             // captura -> envio: canal 0 (mic L crudo), bloques de 1024 B
static RingbufHandle_t s_ring_ch1;             // canal 1: mic R crudo (captura) o salida del AFE (fetch), bloques de 1024 B
static volatile bool s_graba_ch0;              // main -> captura: copiar el crudo a s_ring_ch0
static volatile bool s_graba_afe;              // main -> fetch: copiar la salida del AFE a s_ring_ch1
static volatile bool s_toma_terminada;         // envio -> main: la toma termino (bien o abortada)
static volatile uint32_t s_overruns_ch0;       // bloques del canal 0 que no entraron (los escribe la captura; baseline)
static volatile uint32_t s_overruns_ch1;       // bloques del canal 1 que no entraron (captura o fetch segun el modo; baseline)
static volatile uint32_t s_bytes_enviados;
static volatile uint32_t s_max_items_ch0;      // maximo de bloques esperando en cada ring (~ latencia del AFE / 32 ms)
static volatile uint32_t s_max_items_ch1;
static char s_vad_toma[VAD_LOG_BLOQUES + 1];   // por bloque de la toma: '1' voz, '0' silencio, '.' sin dato (32 ms c/u)
static char s_ultima_toma[640] = "Fin: sin tomas todavia";   // resumen de la ultima toma (lo escribe y lo lee el loop)

// =============================================================================
//  CALLBACKS DEL PIPELINE
// =============================================================================
// Se llaman desde la tarea de captura / fetch: rapidos, sin esperar (timeout 0).

static void test_on_crudo(const int16_t *chunk_ini, size_t frames, bool hueco)
{
    (void)hueco;                                   // el test no usa hueco (los huecos ya salen en la linea Fin:)
    static int16_t canal_l[ORBITA_CHUNK_FRAMES];   // static: no ocupan el stack de la captura
    static uint32_t bloques_ch0;                   // bloques ya entregados en la toma actual
#if !ORBITA_PIPELINE_USA_AFE
    static int16_t canal_r[ORBITA_CHUNK_FRAMES];   // el canal 1 sale de la captura solo sin AFE
    static uint32_t bloques_ch1;
#endif

    if (!s_graba_ch0) {
        bloques_ch0 = 0;                           // sin toma (o abortada): el conteo vuelve a cero
#if !ORBITA_PIPELINE_USA_AFE
        bloques_ch1 = 0;
#endif
        return;
    }
    if (frames != ORBITA_CHUNK_FRAMES) {
        return;
    }

    for (int i = 0; i < ORBITA_CHUNK_FRAMES; i++) {
        canal_l[i] = chunk_ini[2 * i];
#if !ORBITA_PIPELINE_USA_AFE
        canal_r[i] = chunk_ini[2 * i + 1];
#endif
    }

    // Un bloque perdido no cuenta (la toma sigue hasta juntar TOMA_BLOQUES) y queda en los overruns.
    for (int b = 0; b < BLOQUES_POR_CHUNK; b++) {
        if (bloques_ch0 < TOMA_BLOQUES) {
            if (xRingbufferSend(s_ring_ch0, &canal_l[b * BLOQUE_MUESTRAS], BLOQUE_BYTES, 0) == pdTRUE) {
                bloques_ch0++;
            } else {
                s_overruns_ch0++;
            }
        }
#if !ORBITA_PIPELINE_USA_AFE
        if (bloques_ch1 < TOMA_BLOQUES) {
            if (xRingbufferSend(s_ring_ch1, &canal_r[b * BLOQUE_MUESTRAS], BLOQUE_BYTES, 0) == pdTRUE) {
                bloques_ch1++;
            } else {
                s_overruns_ch1++;
            }
        }
#endif
    }

    bool toma_completa = (bloques_ch0 >= TOMA_BLOQUES);
#if !ORBITA_PIPELINE_USA_AFE
    toma_completa = toma_completa && (bloques_ch1 >= TOMA_BLOQUES);   // sin AFE hay que juntar los dos canales
#endif
    if (toma_completa) {
        s_graba_ch0 = false;
        bloques_ch0 = 0;
#if !ORBITA_PIPELINE_USA_AFE
        bloques_ch1 = 0;
#endif
    }
}

#if ORBITA_PIPELINE_USA_AFE
static void test_on_afe(const int16_t *mono_ini, size_t muestras, bool voz)
{
    static uint32_t bloques_afe;                   // bloques del AFE ya entregados en la toma actual

    if (!s_graba_afe) {
        bloques_afe = 0;
    } else if (xRingbufferSend(s_ring_ch1, mono_ini, muestras * ORBITA_BYTES_PER_SAMPLE, 0) == pdTRUE) {
        if (bloques_afe < VAD_LOG_BLOQUES) {
            s_vad_toma[bloques_afe] = voz ? '1' : '0';
        }
        if (++bloques_afe >= TOMA_BLOQUES) {
            s_graba_afe = false;
            bloques_afe = 0;
        }
    } else {
        s_overruns_ch1++;
    }
}
#endif

// =============================================================================
// Configuracion de callback - TEST
// =============================================================================
// Se crea la struct de cfg pipeline para el test por UART. test_pipeline_cfg() le pasa esta cfg a orbita_pipeline_start() asi
// se usa test_on_crudo como callback. Cuando sea el final para mi se tiene que hacer un if y que se inicialize la otra con NULL en funcion
// de si es con o sin afe
static const orbita_pipeline_cfg_t s_pipeline_cfg = {
    .on_crudo = test_on_crudo,
#if ORBITA_PIPELINE_USA_AFE
    .on_afe = test_on_afe,
#endif
};

// =============================================================================
//  TAREA DE ENVIO  -  combina ch0 + ch1 y los escribe al UART
// =============================================================================
// Toma exactamente TOMA_BLOQUES bloques de cada ring. No alinea en el tiempo: la muestra n
// del ch1 va con la muestra n del ch0 (con el AFE llega unas decenas de ms despues).
static void tarea_envio(void *arg)
{
    static int16_t par[BLOQUE_MUESTRAS * 2];       // 512 frames [ch0, ch1]; static: no ocupa stack

    for (uint32_t i = 0; i < TOMA_BLOQUES; i++) {
        UBaseType_t esperan = 0;
        vRingbufferGetInfo(s_ring_ch0, NULL, NULL, NULL, NULL, &esperan);
        if (esperan > s_max_items_ch0) {
            s_max_items_ch0 = esperan;
        }
        vRingbufferGetInfo(s_ring_ch1, NULL, NULL, NULL, NULL, &esperan);
        if (esperan > s_max_items_ch1) {
            s_max_items_ch1 = esperan;
        }

        size_t n0 = 0, n1 = 0;
        int16_t *ch0 = xRingbufferReceive(s_ring_ch0, &n0, pdMS_TO_TICKS(1000));
        int16_t *ch1 = xRingbufferReceive(s_ring_ch1, &n1, pdMS_TO_TICKS(1000));

        // Si falta una fuente (1 s sin datos) o el tamano no es el esperado, se aborta la toma.
        if (ch0 == NULL || ch1 == NULL || n0 != BLOQUE_BYTES || n1 != BLOQUE_BYTES) {
            if (ch0 != NULL) {
                vRingbufferReturnItem(s_ring_ch0, ch0);
            }
            if (ch1 != NULL) {
                vRingbufferReturnItem(s_ring_ch1, ch1);
            }
            s_graba_ch0 = false;
            s_graba_afe = false;
            break;
        }

        for (int k = 0; k < BLOQUE_MUESTRAS; k++) {
            par[2 * k] = ch0[k];
            par[2 * k + 1] = ch1[k];
        }
        vRingbufferReturnItem(s_ring_ch0, ch0);
        vRingbufferReturnItem(s_ring_ch1, ch1);

        // Si el buffer TX del UART esta lleno, esto bloquea (contrapresion).
        uart_write_bytes(TEST_UART_NUM, (const char *)par, sizeof(par));
        s_bytes_enviados += sizeof(par);
    }

    s_toma_terminada = true;
    vTaskDelete(NULL);
}

// Descarta lo que haya quedado en un ring (de una toma abortada).
static void vaciar_ring(RingbufHandle_t ring)
{
    size_t n = 0;
    void *p;
    while ((p = xRingbufferReceive(ring, &n, 0)) != NULL) {
        vRingbufferReturnItem(ring, p);
    }
}

// =============================================================================
//  API
// =============================================================================
void test_init(void)
{
    // --- Drivers de comunicacion ---
    esp_err_t uart_err = uart_driver_install(TEST_UART_NUM, 256, 8192, 0, NULL, 0);
    if (uart_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART0 ya tenia un driver instalado (consola) -- lo reusamos.");
    } else {
        ESP_ERROR_CHECK(uart_err);
    }

    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 1024,    // la linea "Fin:" mide ~420 B: con 256 el write no entra y no manda nada
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    // --- Rings de test ---
    s_ring_ch0 = xRingbufferCreateNoSplit(BLOQUE_BYTES, TEST_BLOQUES);
    s_ring_ch1 = xRingbufferCreateNoSplit(BLOQUE_BYTES, TEST_BLOQUES);
    ESP_ERROR_CHECK((s_ring_ch0 != NULL && s_ring_ch1 != NULL) ? ESP_OK : ESP_ERR_NO_MEM);
}

const orbita_pipeline_cfg_t *test_pipeline_cfg(void)
{
    return &s_pipeline_cfg;
}

// Una toma completa: marcador, header del WAV, arranque, espera y linea "Fin:".
static void test_tomar(void)
{
    static uint8_t wav_header[WAV_HEADER_SIZE];

    // ---------- 1. Reinicio de contadores ----------
    s_toma_terminada = false;
    s_bytes_enviados = 0;
    s_max_items_ch0 = 0;
    s_max_items_ch1 = 0;
    vaciar_ring(s_ring_ch0);                       // restos de una toma abortada
    vaciar_ring(s_ring_ch1);
    memset(s_vad_toma, '.', VAD_LOG_BLOQUES);

    // Baselines de lo que corre siempre (contadores uint32_t sin signo: la resta sirve aunque den la vuelta)
    orbita_pipeline_stats_t ini;
    orbita_pipeline_get_stats(&ini);
    uint32_t ch0_ini = s_overruns_ch0;
    uint32_t ch1_ini = s_overruns_ch1;

    // ---------- 2. Aviso a la PC y header del WAV ----------
    // Marcador por USB-Serial-JTAG: desde aca el audio viene por UART0.
    const char *marcador = "WAV_ON_UART0\n";
    usb_serial_jtag_write_bytes(marcador, strlen(marcador), portMAX_DELAY);

    // El header declara el tamano TOTAL de antemano (en streaming no se sabe cuanto va a
    // llegar de verdad; la PC compara bytes recibidos contra este numero para detectar huecos).
    // Frame del WAV = [ch0, ch1] = 4 B.
    uint32_t wav_data_size = (uint32_t)TOMA_BLOQUES * BLOQUE_MUESTRAS * ORBITA_FRAME_BYTES;
    wav_build_header(wav_header, ORBITA_SAMPLE_RATE_HZ, ORBITA_CHANNELS, ORBITA_BYTES_PER_SAMPLE * 8, wav_data_size);

    // ---------- 3. UART en modo audio ----------
    // Hasta el final de la toma no puede salir ningun log por UART0: se mezclaria texto con
    // el audio y se corrompe el archivo.
    esp_log_level_set("*", ESP_LOG_NONE);
    uart_set_baudrate(TEST_UART_NUM, TEST_UART_BAUD_AUDIO);

    // El header sale ANTES de crear la tarea de envio: asi es lo primero en el cable.
    uart_write_bytes(TEST_UART_NUM, (const char *)wav_header, WAV_HEADER_SIZE);

    // ---------- 4. Arranque de la toma ----------
    // Core 0 (red y control): en el core 1 competiria con la captura y el AFE.
    xTaskCreatePinnedToCore(tarea_envio, "envio", 4096, NULL, 5, NULL, 0);

    // Las banderas van ULTIMO: asi captura y fetch nunca arrancan con valores viejos.
    s_graba_ch0 = true;
    s_graba_afe = true;

    // ---------- 5. Espera y cierre ----------
    while (!s_toma_terminada) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Esperar a que salgan fisicamente los ultimos bytes antes de bajar el baud; si no, los ultimos quedan corruptos.
    uart_wait_tx_done(TEST_UART_NUM, portMAX_DELAY);
    uart_set_baudrate(TEST_UART_NUM, TEST_UART_BAUD_LOG);
    esp_log_level_set("*", ESP_LOG_INFO);

    // Se guarda en un buffer para poder repetirlo con 's' si la PC se perdio el log.
    orbita_pipeline_stats_t fin;
    orbita_pipeline_get_stats(&fin);
    snprintf(s_ultima_toma, sizeof(s_ultima_toma),
             "Fin: mandados %u de %u bytes, overruns_ch0=%u, overruns_ch1=%u, overruns_ring1=%u, dma_overflows=%u, i2s_errores=%u, "
             "bloques max ch0=%u ch1=%u de %u, stack libre captura=%u, "
             "feed=%u, feed_invalidos=%u, stack libre feed=%u, "
             "fetch=%u, fetch_vacios=%u, fetch_invalidos=%u, stack libre fetch=%u, bloques_voz=%u, vad=%s",
             (unsigned)s_bytes_enviados, (unsigned)wav_data_size,
             (unsigned)(s_overruns_ch0 - ch0_ini), (unsigned)(s_overruns_ch1 - ch1_ini),
             (unsigned)(fin.overruns_ring1 - ini.overruns_ring1),
             (unsigned)(fin.dma_overflows - ini.dma_overflows),
             (unsigned)(fin.i2s_errores - ini.i2s_errores),
             (unsigned)s_max_items_ch0, (unsigned)s_max_items_ch1, (unsigned)TEST_BLOQUES,
             (unsigned)fin.stack_libre_captura,
             (unsigned)(fin.chunks_alimentados - ini.chunks_alimentados),
             (unsigned)(fin.chunks_invalidos - ini.chunks_invalidos), (unsigned)fin.stack_libre_feed,
             (unsigned)(fin.chunks_recibidos - ini.chunks_recibidos),
             (unsigned)(fin.fetch_vacios - ini.fetch_vacios),
             (unsigned)(fin.fetch_invalidos - ini.fetch_invalidos), (unsigned)fin.stack_libre_fetch,
             (unsigned)(fin.bloques_voz - ini.bloques_voz), s_vad_toma);
    ESP_LOGI(TAG, "%s", s_ultima_toma);
}

void test_loop(void)
{
    uint8_t cmd;

    while (1) {
        int n = usb_serial_jtag_read_bytes(&cmd, sizeof(char), 0);

        if (n == 1 && cmd == 'g') {
            test_tomar();
        } else if (n == 1 && cmd == 's') {
            // Stats de la ultima toma por USB-Serial-JTAG (no pasa por la UART del audio).
            usb_serial_jtag_write_bytes(s_ultima_toma, strlen(s_ultima_toma), portMAX_DELAY);
            usb_serial_jtag_write_bytes("\n", 1, portMAX_DELAY);
        } else {
            // Sin comando: la captura ya vacia el I2S; aca solo se espera para no girar al maximo.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
