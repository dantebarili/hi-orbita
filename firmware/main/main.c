#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "esp_timer.h"

static const char *TAG = "orbita_main";

#define TEST_DURATION_SEC 3 // bajado de 10 a 3 para iterar mas rapido mientras probamos el pipeline (el cuello de botella real es el throughput de USB-Serial-JTAG, ver notas)
#define FRAME_SIZE 2

// El audio (header + datos) se manda por UART0 -- el mismo cable/puerto
// que ya usabamos solo para logs -- en vez de por USB-Serial-JTAG, que
// resulto tener throughput real de ~170 B/s para transferencias grandes
// (medido: el driver acepta el audio casi instantaneo pero el envio real
// por USB queda muy por detras, desacoplado -- inviable para un .wav de
// cientos de KB). Un UART de verdad sostiene 921600 baud sin problema.
//
// ORBITA_UART_CONSOLE_BAUD tiene que coincidir con el baudrate de consola
// real (CONFIG_ESP_CONSOLE_UART_BAUDRATE en sdkconfig, default 115200) --
// es al que volvemos despues de mandar el audio, para que idf.py monitor
// siga viendo los logs bien.
#define ORBITA_UART_DATA_NUM UART_NUM_0
#define ORBITA_UART_DATA_BAUD 921600
#define ORBITA_UART_CONSOLE_BAUD 115200

// Margen maximo entre llamadas a orbita_audio_i2s_read() antes de arriesgar
// overrun: dma_desc_num(6) * dma_frame_num(240) = 1440 frames (~90ms @16kHz)
// de capacidad en el buffer interno del driver I2S.
#define MAX_MARGEN_LECTURA_US 90000

// Defino la cantidad de FRAMES por archivo TEST
static size_t frame_count_test = TEST_DURATION_SEC * ORBITA_SAMPLE_RATE_HZ;
static size_t frame_count_test_to_read = 1600; // cantidad de frames a leer en cada bloque (buffer temporal)

// Calcula RMS de un canal a partir del buffer entrelazado L/R.
// channel: 0 = izquierdo, 1 = derecho.
// Sin uso por ahora (RMS en vivo desactivado, ver app_main) -- __attribute__
// unused para que no tire warning de "funcion definida pero no usada".
static double __attribute__((unused)) do_rms(const int32_t *data_buf, size_t frame_count, int channel)
{
    double sum_rms = 0.0;
    for (size_t i = 0; i < frame_count; i++) {
        // Se descartan los 8 bits menos significativos (relleno/basura del
        // slot de 32 bits; el mic solo entrega 24 bits utiles alineados a la
        // izquierda). El shift preserva el signo porque sample es int32_t.
        int32_t sample = data_buf[i * 2 + channel] >> 8;
        // Cast a double ANTES de multiplicar: sample al cuadrado puede superar
        // el rango de int32_t (overflow), asi que la multiplicacion tiene que
        // hacerse ya en double, no despues de sumarla.
        sum_rms += (double)sample * (double)sample;
    }

    return sqrt(sum_rms / frame_count);
}

void app_main(void)
{
    // Instalamos el driver de UART0 (el mismo cable/puerto que hoy usa
    // Puerto A para logs) para poder mandar el audio del WAV por ahi a
    // alto baudrate. Mientras no estemos mandando audio, UART0 se sigue
    // comportando igual que siempre (logs a 115200) -- ver el bloque de
    // envio mas abajo, donde subimos el baudrate solo durante esa ventana.
    // rx_buffer_size=256: no usamos RX (no leemos nada por este UART), pero
    // el driver EXIGE un buffer RX mayor al FIFO de hardware (128 bytes)
    // para instalarse -- pasar 0 tira "uart rx buffer length error" y
    // aborta (visto en pruebas reales). 256 alcanza de sobra.
    //
    // OJO: la consola de ESP-IDF (CONFIG_ESP_CONSOLE_UART_NUM=0) puede
    // haber instalado YA su propio driver sobre este mismo UART0 antes de
    // que arranquemos -- instalarlo de nuevo devuelve ESP_ERR_INVALID_STATE,
    // que NO es un error real (solo significa "ya hay uno, no hace falta
    // instalarlo de nuevo"). Si lo tratamos como error fatal (como hacia
    // antes con ESP_ERROR_CHECK a secas), abortaba el arranque entero --
    // eso es lo que rompio todo la vez pasada.
    esp_err_t uart_install_err = uart_driver_install(ORBITA_UART_DATA_NUM, 256, 8192, 0, NULL, 0);
    if (uart_install_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "UART0 ya tenia un driver instalado (consola) -- lo reusamos.");
    } else {
        ESP_ERROR_CHECK(uart_install_err);
    }

    // Configuracion del driver USB-Serial-JTAG en modo lectura: define el
    // tamaño de los buffers internos de RX (bytes que llegan de la PC) y TX
    // (bytes que mandamos nosotros). Sin este paso, usb_serial_jtag_read_bytes
    // no tiene de donde leer.
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 256, // buffer chico: solo esperamos comandos de 1 byte
        // TX mas grande: mandamos hasta 9600 bytes de un saque por bloque de
        // audio (pack_buf); con 256 bytes el driver tenia que trocear cada
        // bloque en ~38 vueltas internas de espera, penalizando el throughput.
        .tx_buffer_size = 4096,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    // Inicializaciòn del protocolo I2S.
    ESP_ERROR_CHECK(orbita_audio_i2s_init());

    // Reservo memoria en la PSRAM para la cantidad de frames que ocupa un archivo de salida
    int32_t *buf_psram = heap_caps_malloc(frame_count_test * FRAME_SIZE * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (buf_psram == NULL) {
        // No hay memoria disponible: logueamos el motivo y cortamos aca.
        // return sale de app_main() -> FreeRTOS borra esta tarea sola, no hace
        // falta (ni corresponde) un "return -1" como en un main() de PC.
        ESP_LOGE(TAG, "No se pudo reservar %d bytes en PSRAM", (int)(frame_count_test * FRAME_SIZE * sizeof(int32_t)));
        return;
    }

    ESP_LOGI(TAG, "Buffer en PSRAM reservado: %d frames (%d seg)", (int)frame_count_test, TEST_DURATION_SEC);

    // Buffer chico y descartable para el modo "monitoreo" (cuando todavia no
    // llego el comando 'g'): no hace falta PSRAM aca, se pisa en cada vuelta.
    int32_t *monitor_buf = malloc(frame_count_test_to_read * FRAME_SIZE * sizeof(int32_t));
    if (monitor_buf == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar el buffer de monitoreo");
        return;
    }

    uint8_t cmd;
    uint8_t *wav_header = malloc(WAV_HEADER_SIZE); // 44 bytes del header WAV
    if (wav_header == NULL)
    {
        ESP_LOGE(TAG, "No se pudo reservar la memoria en la RAM para el header.");
        return;
    }

    // Buffer chico y reusable para empaquetar de a un bloque (32->24 bit)
    // justo antes de mandarlo — evita duplicar en PSRAM los ~1.28MB de
    // buf_psram en formato empaquetado.
    uint8_t *pack_buf = malloc(frame_count_test_to_read * FRAME_SIZE * 3);
    if (pack_buf == NULL) {
        ESP_LOGE(TAG, "No se pudo reservar el buffer de empaquetado");
        return;
    }

    while (1) {
        
        // Lee 1 byte de la PC (sin bloquear, timeout=0). Si no hay nada, n=0 y cmd[0] queda sin modificar.
        int n = usb_serial_jtag_read_bytes(&cmd, sizeof(char), 0);

        if (n == 1 && cmd == 'g') {
            size_t frames_acumulados = 0;
            // t_fin_lectura_anterior en 0 = "todavia no hay lectura previa
            // con la cual comparar" (recien vamos a arrancar la primera).
            int64_t t_fin_lectura_anterior = 0;

            while (frames_acumulados < frame_count_test) {
                // Mido cuanto paso desde que termino la lectura anterior
                // hasta que voy a pedir la proxima: ese es el tiempo que
                // "gaste" haciendo do_rms/snprintf/write_bytes, y es lo que
                // se compara contra el margen maximo antes de overrun.
                if (t_fin_lectura_anterior != 0) {
                    int64_t margen_us = esp_timer_get_time() - t_fin_lectura_anterior;
                    if (margen_us > MAX_MARGEN_LECTURA_US) {
                        ESP_LOGE(TAG, "Posible overrun: %lld us entre lecturas (max %d)", margen_us, MAX_MARGEN_LECTURA_US);
                    } else {
                        ESP_LOGI(TAG, "Margen entre lecturas: %lld us", margen_us);
                    }
                }

                // Leer un bloque de frames y lo guardo en el buffer de PSRAM.
                size_t frames_leidos_este_bloque = 0;
                esp_err_t lectura_actual = orbita_audio_i2s_read(buf_psram + frames_acumulados * FRAME_SIZE, frame_count_test_to_read, &frames_leidos_este_bloque);
                if(lectura_actual != ESP_OK){
                    ESP_LOGE(TAG, "Error al leer frames [%d; %d + %d]: %s", (int)frames_acumulados, (int)frames_acumulados, (int)frame_count_test, esp_err_to_name(lectura_actual));
                    break;
                }
                t_fin_lectura_anterior = esp_timer_get_time();

                // RMS en vivo desactivado a proposito (simplificacion
                // temporal): no lo necesitamos para descargar el .wav, y
                // sacarlo reduce trafico/puntos de falla en el puerto de
                // comandos mientras depuramos el envio por UART0. Se puede
                // reactivar despues llamando do_rms() aca de nuevo.

                frames_acumulados += frames_leidos_este_bloque;
            }

            // Armo el header con el tamaño REAL capturado (frames_acumulados
            // puede ser menor a frame_count_test si alguna lectura fallo a
            // mitad de la grabacion) — asi el .wav queda consistente con lo
            // que realmente se grabo, en vez de declarar un tamaño mayor al
            // de los datos que van a seguir.
            // Le avisamos a la PC por Puerto B (USB-Serial-JTAG, como
            // siempre -- ahi sigue viviendo el comando 'g' y el RMS en
            // vivo) que el audio en si viene por Puerto A (UART0) ahora,
            // no por aca. Sin este marcador, Python no sabe cuando dejar
            // de esperar lineas de RMS y pasar a escuchar el otro puerto.
            const char *wav_start_marker = "WAV_ON_UART0\n";
            usb_serial_jtag_write_bytes(wav_start_marker, strlen(wav_start_marker), portMAX_DELAY);

            uint32_t wav_data_size = (uint32_t)(frames_acumulados * FRAME_SIZE * 3);
            wav_build_header(wav_header, ORBITA_SAMPLE_RATE_HZ, FRAME_SIZE, 24, wav_data_size);

            // A partir de aca mandamos TODO (header + audio) por UART0 a
            // baudrate alto. Silenciamos los logs mientras dure esta
            // ventana: si un ESP_LOGI se cuela en medio, corrompe el
            // archivo (texto de log mezclado con bytes binarios en el
            // mismo cable). Los restauramos apenas terminamos.
            esp_log_level_set("*", ESP_LOG_NONE);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_DATA_BAUD);

            uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)wav_header, WAV_HEADER_SIZE);

            // Empaqueto y mando el audio de a bloques (32->24 bit), reusando
            // el mismo pack_buf chico en cada vuelta.
            size_t frames_enviados = 0;
            while (frames_enviados < frames_acumulados) {
                size_t frames_este_bloque = frame_count_test_to_read;
                if (frames_enviados + frames_este_bloque > frames_acumulados) {
                    frames_este_bloque = frames_acumulados - frames_enviados;
                }

                size_t bytes_empaquetados = wav_pack_block_24bit(
                    buf_psram + frames_enviados * FRAME_SIZE, frames_este_bloque, pack_buf);
                uart_write_bytes(ORBITA_UART_DATA_NUM, (const char *)pack_buf, bytes_empaquetados);

                frames_enviados += frames_este_bloque;
            }

            // Esperamos a que salgan FISICAMENTE todos los bytes del FIFO
            // antes de bajar el baudrate -- si no, los ultimos bytes que
            // todavia estan "en vuelo" saldrian a un baudrate distinto del
            // que se armaron, y quedarian corridos/corruptos.
            uart_wait_tx_done(ORBITA_UART_DATA_NUM, portMAX_DELAY);
            uart_set_baudrate(ORBITA_UART_DATA_NUM, ORBITA_UART_CONSOLE_BAUD);
            esp_log_level_set("*", ESP_LOG_INFO);

            ESP_LOGI(TAG, "Envio de audio por UART0 completo: %d bytes", (int)(frames_enviados * FRAME_SIZE * 3));

        } else {
            // Todavia no llego 'g': leo un bloque a un buffer descartable
            // (siempre a la misma direccion, monitor_buf, se pisa cada vez)
            // solo para mantener el pipeline I2S drenado (evitar overrun
            // mientras esperamos el comando). RMS en vivo desactivado a
            // proposito -- ver comentario en la rama de grabacion.
            size_t frames_leidos_monitor = 0;
            esp_err_t lectura_actual = orbita_audio_i2s_read(monitor_buf, frame_count_test_to_read, &frames_leidos_monitor);
            if (lectura_actual != ESP_OK) {
                ESP_LOGE(TAG, "Error al leer bloque de monitoreo: %s", esp_err_to_name(lectura_actual));
            }
        }

    }

}
