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

// ---------------------------------------------------------------------------
// Constantes
// ---------------------------------------------------------------------------
#define STREAM_SECONDS      600                // 10 para probar, 600 para la grabacion larga
#define FRAME_SIZE          2                  // muestras por frame (L y R)
#define FRAMES_PER_BLOCK    1600               // frames por i2s_read (100 ms)
#define BYTES_PER_SAMPLE    2                  // 16 bit en el cable
#define RING_SIZE_BYTES     (FRAME_SIZE * BYTES_PER_SAMPLE * ORBITA_SAMPLE_RATE_HZ)       // sin margen= fs*2muestrasxframe*2bytesxmuestra
#define SEND_CHUNK_BYTES    4096               // maximo que pedimos al ring por vuelta

#define ORBITA_UART_DATA_NUM     UART_NUM_0
#define ORBITA_UART_DATA_BAUD    921600
#define ORBITA_UART_CONSOLE_BAUD 115200

// ---------------------------------------------------------------------------
// Estado compartido entre las dos tareas
// ---------------------------------------------------------------------------
// "volatile" le dice al compilador que estas variables pueden cambiar por
// fuera del codigo que esta mirando (porque las toca OTRA tarea). Sin eso
// podria guardarlas en un registro y nunca ver el cambio.
static RingbufHandle_t ring_handle;            // captura -> envio
static volatile bool capture_done;             // la captura ya termino
static volatile bool sender_done;              // el envio ya termino
static volatile uint32_t overruns;             // bloques que no entraron al ring
static volatile uint32_t bytes_sent;           // bytes de audio mandados por UART
static volatile size_t max_ocupado;            // maximo de bytes que llego a haber en el ring

// ---------------------------------------------------------------------------
// Tarea de envio: saca bytes del ring y los manda por UART0
// ---------------------------------------------------------------------------
static void tarea_envio(void *arg)
{
    while (1) {
        // Leemos la flag ANTES de pedir datos. Si ya estaba en true, es
        // porque la captura termino de escribir todo antes de este momento;
        // entonces, si el ring devuelve NULL (timeout), esta realmente vacio.
        // Si leyeramos la flag DESPUES, podria haberse escrito el ultimo
        // bloque justo en el medio y lo perderiamos.
        bool captura_termino = capture_done;

        // Pide hasta SEND_CHUNK_BYTES bytes. Espera como maximo 50 ms por
        // datos. Devuelve un PUNTERO adentro del ring (no copia nada) y en
        // n cuantos bytes hay realmente (puede ser menos de lo pedido).
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(ring_handle, &n, pdMS_TO_TICKS(50), SEND_CHUNK_BYTES);

        if (p == NULL) {
            // Timeout: no habia datos.
            if (captura_termino) {
                break; // la captura termino Y el ring esta vacio: terminamos
            }
            continue;  // la captura sigue; esperamos a que llegue mas
        }

        // Mandamos al UART. Si el buffer TX del driver esta lleno, esta
        // llamada BLOQUEA hasta que haya lugar: esa espera es la que frena
        // al envio y permite que el ring se llene (la "contrapresion").
        uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)p, n);

        // Se debe devolver el item
        vRingbufferReturnItem(ring_handle, p);

        bytes_sent += n;
    }

    sender_done = true;
    vTaskDelete(NULL); 
}

void app_main(void)
{
    // --- Setup UART0 ---
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

    ESP_ERROR_CHECK(orbita_audio_i2s_init());

    ring_handle = xRingbufferCreate(RING_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (ring_handle == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el ring buffer (%d bytes)", RING_SIZE_BYTES);
        return;
    }

    // block_buf: aca deja i2s_read cada bloque, tal cual sale del mic (32 bit por muestra).
    int32_t *block_buf = malloc(FRAMES_PER_BLOCK * FRAME_SIZE * sizeof(int32_t));
    if (block_buf == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar block_buf");
        return;
    }

    // pack_buf: el mismo bloque ya convertido a 16 bit (2 bytes por muestra), listo para el ring.
    uint8_t *pack_buf = malloc(FRAMES_PER_BLOCK * FRAME_SIZE * BYTES_PER_SAMPLE);
    if (pack_buf == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar pack_buf");
        return;
    }

    uint8_t *wav_header = malloc(WAV_HEADER_SIZE);
    if (wav_header == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar la memoria en la RAM para el header.");
        return;
    }

    uint8_t cmd;

    while (1) {
        int n = usb_serial_jtag_read_bytes(&cmd, sizeof(char), 0);

        if (n == 1 && cmd == 'g') {

            // --- Flags y variables de control compartidas ---
            capture_done = false;
            sender_done = false;
            overruns = 0;
            bytes_sent = 0;
            max_ocupado = 0;

            // --- Aviso a la PC + header + cambio de baud ---

            // Avisamos a la PC (por USB-Serial-JTAG) que el audio viene ahora por UART0.
            const char *wav_start_marker = "WAV_ON_UART0\n";
            usb_serial_jtag_write_bytes(wav_start_marker, strlen(wav_start_marker), portMAX_DELAY);

            // Header con el tamaño TOTAL declarado de antemano (en streaming no
            // sabemos cuanto se va a capturar de verdad; la PC compara bytes
            // recibidos contra este numero para detectar huecos).
            uint32_t wav_data_size = (uint32_t)(STREAM_SECONDS * ORBITA_SAMPLE_RATE_HZ * FRAME_SIZE * BYTES_PER_SAMPLE);
            wav_build_header(wav_header, ORBITA_SAMPLE_RATE_HZ, FRAME_SIZE, BYTES_PER_SAMPLE * 8, wav_data_size);

            // Desde aca y hasta el final de la grabacion NO puede salir ningun
            // log por UART0: se mezclaria texto con el audio binario y
            // corromperia el archivo. Por eso los apagamos y subimos el baud.
            esp_log_level_set("*", ESP_LOG_NONE);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_DATA_BAUD);

            // El header lo manda app_main ANTES de crear la tarea de envio:
            // asi esta garantizado que es lo primero que sale por el cable.
            uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)wav_header, WAV_HEADER_SIZE);

            // --- Lanzar la tarea de envio ---
            // Parametros: funcion, nombre, stack en bytes, argumento, prioridad, handle.
            // Prioridad 5 (mayor que la de app_main, que es 1): casi todo el
            // tiempo esta bloqueada esperando datos o esperando al UART, asi que
            // no le saca CPU a la captura; cuando hay algo para mandar, lo manda ya.
            xTaskCreate(tarea_envio, "envio", 4096, NULL, 5, NULL);

            // --- 4. Captura (corre aca, en app_main) ---
            size_t frames_totales = (size_t)STREAM_SECONDS * ORBITA_SAMPLE_RATE_HZ;
            size_t frames_capturados = 0;

            while (frames_capturados < frames_totales) {
                // Pedimos de a FRAMES_PER_BLOCK, pero en el ultimo bloque solo lo que falta,
                // para no pasarnos del total declarado en el header.
                size_t frames_a_pedir = frames_totales - frames_capturados;
                if (frames_a_pedir > FRAMES_PER_BLOCK) {
                    frames_a_pedir = FRAMES_PER_BLOCK;
                }

                size_t leidos = 0;
                esp_err_t err = orbita_audio_i2s_read(block_buf, frames_a_pedir, &leidos);
                if (err != ESP_OK) {
                    break; // sin logs (estan apagados): se nota porque bytes_sent < esperado
                }

                // 32 bit -> 16 bit. Devuelve cuantos bytes escribio en pack_buf.
                size_t bytes = wav_pack_block_16bit(block_buf, leidos, pack_buf);

                // Timeout 0 = no esperar. Si no hay lugar en el ring, el bloque se
                // pierde y lo contamos: esa grabacion tiene un hueco.
                if (xRingbufferSend(ring_handle, pack_buf, bytes, 0) != pdTRUE) {
                    overruns++;
                }

                // Que tan lleno llego a estar el ring (sirve para dimensionarlo).
                size_t ocupado = RING_SIZE_BYTES - xRingbufferGetCurFreeSize(ring_handle);
                if (ocupado > max_ocupado) {
                    max_ocupado = ocupado;
                }

                frames_capturados += leidos;
            }

            // --- 5. Cierre ---
            // Le avisamos a la tarea de envio que ya no va a llegar mas audio.
            capture_done = true;

            // Esperamos a que termine de vaciar el ring.
            while (!sender_done) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Esperar a que salgan FISICAMENTE los ultimos bytes del UART antes
            // de bajar el baud; si no, los ultimos quedan corruptos.
            uart_wait_tx_done(ORBITA_UART_DATA_NUM, portMAX_DELAY);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_CONSOLE_BAUD);
            esp_log_level_set("*", ESP_LOG_INFO);

            ESP_LOGI(TAG, "Fin: mandados %u de %u bytes, overruns=%u, ring maximo=%u de %u bytes",
                     (unsigned)bytes_sent, (unsigned)wav_data_size, (unsigned)overruns,
                     (unsigned)max_ocupado, (unsigned)RING_SIZE_BYTES);
        } else {
            // Esperando 'g': hay que seguir leyendo el I2S (si nadie lo vacia, el DMA
            // se pisa) pero los datos se tiran.
            size_t leidos = 0;
            orbita_audio_i2s_read(block_buf, FRAMES_PER_BLOCK, &leidos);
        }
    }
}
