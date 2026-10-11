#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"

static const char *TAG = "orbita_main";

// =============================================================================
//  CONSTANTES
// =============================================================================

// --- Audio (se quedan en el producto) ---
#define CHANNELS            2                  // mics (L y R); tras el AFE la salida sera mono
#define BYTES_PER_SAMPLE    2                  // 16 bit en el cable
#define FRAME_BYTES         (CHANNELS * BYTES_PER_SAMPLE)       // un frame = una muestra por canal
#define FRAMES_PER_CHUNK    1600               // frames por i2s_read = chunk AFE
#define CHUNK_BYTES         (FRAMES_PER_CHUNK * FRAME_BYTES)    // un chunk ya en 16 bit

// --- Ring (el tamaño se redefine segun la politica de perdida) ---
#define RING_SECONDS        1                  // audio que absorbe, sin margen
#define RING_SIZE_BYTES     (RING_SECONDS * ORBITA_SAMPLE_RATE_HZ * FRAME_BYTES)

// --- TEST por UART (desaparecen o pasan al sink UART) ---
#define STREAM_SECONDS      10                // 10 para probar, 600 para la grabacion larga
#define UART_SEND_BYTES    4096               // maximo que se pide al ring por vuelta
#define ORBITA_UART_DATA_NUM     UART_NUM_0
#define ORBITA_UART_DATA_BAUD    921600        // durante la grabacion (audio)
#define ORBITA_UART_CONSOLE_BAUD 115200        // el resto del tiempo (logs)

// --- Verificaciones en compilacion ---
// Si se cambia un numero y se rompe una relacion, no compila.
// El ring debe ser multiplo del chunk: si no, un chunk se parte en el borde
// y una lectura puede cortar una muestra a la mitad (se cruzan L y R).
_Static_assert(RING_SIZE_BYTES % CHUNK_BYTES == 0, "RING_SIZE_BYTES debe ser multiplo de CHUNK_BYTES");
// Lo que se lee del ring tiene que ser un numero entero de frames.
_Static_assert(UART_SEND_BYTES % FRAME_BYTES == 0, "UART_SEND_BYTES debe ser multiplo de FRAME_BYTES");

// =============================================================================
//  ESTADO COMPARTIDO ENTRE TAREAS
// =============================================================================
static RingbufHandle_t ring_handle;            // captura -> envio
static TaskHandle_t captura_handle;            // para medir el stack de la captura

// Control de la toma
static volatile bool grabando;                 // app_main -> captura: hay toma en curso (si es false, la captura tira los datos)
static volatile size_t frames_objetivo;        // app_main -> captura: frames a grabar en esta toma
static volatile bool capture_done;             // captura -> envio / app_main: la captura termino
static volatile bool sender_done;              // envio -> app_main: el envio termino

// Metricas de la toma
static volatile uint32_t overruns;             // chunks que no entraron al ring
static volatile uint32_t bytes_sent;           // bytes de audio mandados por UART
static volatile size_t max_ocupado;            // maximo de bytes que llego a haber en el ring

// =============================================================================
//  TAREA DE ENVIO  -  ring -> UART0
// =============================================================================
static void tarea_envio(void *arg)
{
    while (1) {
        // La flag se lee ANTES de pedir datos. Si ya era true, la captura habia
        // terminado de escribir; entonces un ring vacio (NULL) es realmente el
        // final. Leerla despues podria perder un ultimo chunk escrito en el medio.
        bool captura_termino = capture_done;

        // Pide hasta UART_SEND_BYTES y espera como mucho 50 ms. Devuelve un
        // puntero DENTRO del ring (no copia) y en `n` cuantos bytes hay (<= lo pedido).
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(ring_handle, &n, pdMS_TO_TICKS(50), UART_SEND_BYTES);

        if (p == NULL) {
            if (captura_termino) {
                break;     // captura terminada y ring vacio: fin
            }
            continue;      // la captura sigue; esperar mas datos
        }

        // Si el buffer TX del UART esta lleno, esto bloquea. Esa espera frena
        // el envio y deja que el ring se llene (contrapresion).
        uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)p, n);

        vRingbufferReturnItem(ring_handle, p);  // obligatorio devolver el item
        bytes_sent += n;
    }

    sender_done = true;
    vTaskDelete(NULL);
}

// =============================================================================
//  TAREA DE CAPTURA  -  I2S -> ring
// =============================================================================
// Corre siempre y nunca termina. Con toma en curso manda los chunks al ring;
// sin toma los tira, pero igual hay que seguir leyendo para vaciar el DMA.
static void tarea_captura(void *arg)
{
    // --- Buffers ---
    // chunk_buf: chunk tal cual sale del mic (32 bit por muestra).
    // pack_buf:  el mismo chunk en 16 bit, listo para el ring.
    int32_t *chunk_buf = malloc(FRAMES_PER_CHUNK * CHANNELS * sizeof(int32_t));
    uint8_t *pack_buf = malloc(CHUNK_BYTES);
    if (chunk_buf == NULL || pack_buf == NULL) {
        ESP_LOGE(TAG, "No se pudieron reservar los buffers de captura");
        vTaskDelete(NULL);  // una tarea nunca hace return: se borra a si misma
    }

    size_t frames_capturados = 0;  // de la toma actual
    bool grabando_antes = false;   // para detectar el instante en que arranca una toma

    while (1) {
        // --- Estado de la toma ---
        bool graba = grabando;     // una sola lectura por vuelta
        if (graba && !grabando_antes) {
            frames_capturados = 0; // toma nueva
        }
        grabando_antes = graba;

        // --- Lectura del I2S ---
        // De a FRAMES_PER_CHUNK; el ultimo chunk de la toma solo pide lo que
        // falta, para no pasarse del total declarado en el header.
        size_t frames_a_pedir = FRAMES_PER_CHUNK;
        if (graba && frames_objetivo - frames_capturados < frames_a_pedir) {
            frames_a_pedir = frames_objetivo - frames_capturados;
        }

        size_t frames_leidos = 0;
        esp_err_t err = orbita_audio_i2s_read(chunk_buf, frames_a_pedir, &frames_leidos);
        if (err != ESP_OK) {
            if (graba) {
                // Sin log (estan apagados durante la toma): se nota porque bytes_sent < esperado.
                grabando = false;
                capture_done = true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // no girar al maximo si el error persiste
            continue;
        }
        //vTaskDelay(pdMS_TO_TICKS(200));

        if (!graba) {
            continue;  // sin toma: el chunk se descarta
        }

        // --- Conversion y envio al ring ---
        size_t bytes = wav_pack_chunk_16bit(chunk_buf, frames_leidos, pack_buf);  // 32 -> 16 bit

        // Timeout 0: si no hay lugar, el chunk se pierde y queda contado (hueco en la toma).
        if (xRingbufferSend(ring_handle, pack_buf, bytes, 0) != pdTRUE) {
            overruns++;
        }

        // --- Metricas ---
        size_t ocupado = RING_SIZE_BYTES - xRingbufferGetCurFreeSize(ring_handle);
        if (ocupado > max_ocupado) {
            max_ocupado = ocupado;  // sirve para dimensionar el ring
        }

        // --- Fin de la toma ---
        frames_capturados += frames_leidos;
        if (frames_capturados >= frames_objetivo) {
            grabando = false;
            capture_done = true;    // va ultimo: es la señal para el envio
        }
    }
}

// =============================================================================
//  APP_MAIN  -  inicializacion y comandos
// =============================================================================
void app_main(void)
{
    // --- Drivers de comunicacion ---
    esp_err_t uart_install_err = uart_driver_install(ORBITA_UART_DATA_NUM, 256, 8192, 0, NULL, 0);
    if (uart_install_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART0 ya tenia un driver instalado (consola) -- lo reusamos.");
    } else {
        ESP_ERROR_CHECK(uart_install_err);
    }

    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 256,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    // --- Audio ---
    ESP_ERROR_CHECK(orbita_audio_i2s_init());

    ring_handle = xRingbufferCreate(RING_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (ring_handle == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring buffer (%d bytes)", RING_SIZE_BYTES);
        return;
    }

    // Captura desde el arranque, fija al core 1 (WiFi usa el 0 por defecto).
    // Parametros: funcion, nombre, stack (bytes), argumento, prioridad, handle, core.
    // Prioridad 6: mayor que envio (5) y que app_main (1).
    xTaskCreatePinnedToCore(tarea_captura, "captura", 4096, NULL, 6, &captura_handle, 1);

    uint8_t *wav_header = malloc(WAV_HEADER_SIZE);
    if (wav_header == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar la memoria en la RAM para el header.");
        return;
    }

    // --- Loop de comandos ---
    uint8_t cmd;

    while (1) {
        int n = usb_serial_jtag_read_bytes(&cmd, sizeof(char), 0);

        if (n == 1 && cmd == 'g') {

            // ---------- 1. Reinicio de contadores ----------
            capture_done = false;
            sender_done = false;
            overruns = 0;
            orbita_audio_reset_dma_overflows();
            bytes_sent = 0;
            max_ocupado = 0;

            // ---------- 2. Aviso a la PC y header del WAV ----------
            // Marcador por USB-Serial-JTAG: desde aca el audio viene por UART0.
            const char *wav_start_marker = "WAV_ON_UART0\n";
            usb_serial_jtag_write_bytes(wav_start_marker, strlen(wav_start_marker), portMAX_DELAY);

            // El header declara el tamaño TOTAL de antemano (en streaming no se
            // sabe cuanto va a llegar de verdad; la PC compara bytes recibidos
            // contra este numero para detectar huecos).
            size_t frames_totales_rec_test = (size_t)STREAM_SECONDS * ORBITA_SAMPLE_RATE_HZ;
            uint32_t wav_data_size = (uint32_t)(frames_totales_rec_test * FRAME_BYTES);
            wav_build_header(wav_header, ORBITA_SAMPLE_RATE_HZ, CHANNELS, BYTES_PER_SAMPLE * 8, wav_data_size);

            // ---------- 3. UART en modo audio ----------
            // Hasta el final de la toma no puede salir ningun log por UART0: se
            // mezclaria texto con el audio y se corrompe el archivo.
            esp_log_level_set("*", ESP_LOG_NONE);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_DATA_BAUD);

            // El header sale ANTES de crear la tarea de envio: asi es lo primero en el cable.
            uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)wav_header, WAV_HEADER_SIZE);

            // ---------- 4. Arranque de la toma ----------
            xTaskCreate(tarea_envio, "envio", 4096, NULL, 5, NULL);

            // `grabando` va ULTIMO: asi la captura nunca arranca con valores viejos.
            frames_objetivo = frames_totales_rec_test;
            grabando = true;

            // ---------- 5. Espera y cierre ----------
            while (!sender_done) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Esperar a que salgan fisicamente los ultimos bytes antes de bajar
            // el baud; si no, los ultimos quedan corruptos.
            uart_wait_tx_done(ORBITA_UART_DATA_NUM, portMAX_DELAY);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_CONSOLE_BAUD);
            esp_log_level_set("*", ESP_LOG_INFO);

            ESP_LOGI(TAG, "Fin: mandados %u de %u bytes, overruns=%u, dma_overflows=%u, ring maximo=%u de %u bytes, stack libre captura=%u bytes",
                     (unsigned)bytes_sent, (unsigned)wav_data_size, (unsigned)overruns,
                     (unsigned)orbita_audio_get_dma_overflows(),
                     (unsigned)max_ocupado, (unsigned)RING_SIZE_BYTES,
                     (unsigned)uxTaskGetStackHighWaterMark(captura_handle));
        } else {
            // Sin comando: la captura ya vacia el I2S; aca solo se espera para no girar al maximo.
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
