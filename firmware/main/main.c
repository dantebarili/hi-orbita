#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_capture.h"
#include "wav_writer.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"

static const char *TAG = "orbita_main";

#define TEST_DURATION_SEC 10
#define FRAME_SIZE 2

// Margen maximo entre llamadas a orbita_audio_i2s_read() antes de arriesgar
// overrun: dma_desc_num(6) * dma_frame_num(240) = 1440 frames (~90ms @16kHz)
// de capacidad en el buffer interno del driver I2S.
#define MAX_MARGEN_LECTURA_US 90000

// Defino la cantidad de FRAMES por archivo TEST
static size_t frame_count_test = TEST_DURATION_SEC * ORBITA_SAMPLE_RATE_HZ;
static size_t frame_count_test_to_read = 1600; // cantidad de frames a leer en cada bloque (buffer temporal)

// Calcula RMS de un canal a partir del buffer entrelazado L/R.
// channel: 0 = izquierdo, 1 = derecho.
static double do_rms(const int32_t *data_buf, size_t frame_count, int channel)
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
    // Configuracion del driver USB-Serial-JTAG en modo lectura: define el
    // tamaño de los buffers internos de RX (bytes que llegan de la PC) y TX
    // (bytes que mandamos nosotros). Sin este paso, usb_serial_jtag_read_bytes
    // no tiene de donde leer.
    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = 256, // buffer chico: solo esperamos comandos de 1 byte
        .tx_buffer_size = 256,
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

                // Calculo RMS de ambos canales sobre el bloque recien leido
                // (el puntero de origen es el mismo que le pasamos a la
                // lectura de arriba, antes de sumarle frames_leidos_este_bloque).
                double rms_l = do_rms(buf_psram + frames_acumulados * FRAME_SIZE, frames_leidos_este_bloque, 0);
                double rms_r = do_rms(buf_psram + frames_acumulados * FRAME_SIZE, frames_leidos_este_bloque, 1);

                // Armo una linea de texto "rms_l,rms_r\n" y la mando por el
                // puerto USB-Serial-JTAG nativo para que la PC la grafique.
                char msg[32];
                int msg_len = snprintf(msg, sizeof(msg), "%.1f,%.1f\n", rms_l, rms_r);
                usb_serial_jtag_write_bytes(msg, msg_len, portMAX_DELAY);

                frames_acumulados += frames_leidos_este_bloque;
            }

            // hacer .wav y mandar a la compu

        } else {
            // Todavia no llego 'g': leo un bloque a un buffer descartable
            // (siempre a la misma direccion, monitor_buf, se pisa cada vez)
            // solo para mostrar amplitud en vivo, no se guarda nada.
            size_t frames_leidos_monitor = 0;
            esp_err_t lectura_actual = orbita_audio_i2s_read(monitor_buf, frame_count_test_to_read, &frames_leidos_monitor);
            if (lectura_actual != ESP_OK) {
                ESP_LOGE(TAG, "Error al leer bloque de monitoreo: %s", esp_err_to_name(lectura_actual));
            } else {
                double rms_l = do_rms(monitor_buf, frames_leidos_monitor, 0);
                double rms_r = do_rms(monitor_buf, frames_leidos_monitor, 1);

                char msg[32];
                int msg_len = snprintf(msg, sizeof(msg), "%.1f,%.1f\n", rms_l, rms_r);
                usb_serial_jtag_write_bytes(msg, msg_len, portMAX_DELAY);
            }
        }

    }

}
