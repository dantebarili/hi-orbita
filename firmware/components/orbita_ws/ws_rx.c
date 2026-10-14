/* RECONSTRUCTOR INTERNO: on_ws_event (orbita_ws.c) llama a feed por cada trozo.
 * No crea tareas ni usa red/FreeRTOS; solo organiza bytes en RAM. Se prueba en PC.
 * Un mensaje puede tener varios frames WebSocket; un frame puede llegar en varios
 * eventos de la biblioteca. COMPLETE permite encolar, MORE espera más datos,
 * CONTROL no es contenido de aplicación; INVALID/TOO_LARGE descartan el armado.
 */
#include "ws_rx.h"
#include <string.h>

// Lo usa orbita_ws al cambiar conexión o rechazar datos. Reinicia el armado;
// no necesita borrar el buffer completo: size determina qué bytes son válidos.
void ws_rx_reset(ws_rx_t *rx)
{

    rx->message.size = 0;
    rx->active = false;
    rx->frame_open = false;
    rx->frame_received = 0;
}

// Ayuda interna: descarta el armado parcial y devuelve su causa, sin entregar datos incompletos.
static ws_rx_result_t reject(ws_rx_t *rx, ws_rx_result_t reason)
{
    ws_rx_reset(rx);
    return reason;
}

// Llamada desde el callback, nunca desde app_main. opcode: 1 texto, 2 binario,
// 0 continuación; fin indica último frame del mensaje. frame_size/offset describen
// el frame, data/size el trozo actual. Los bytes se copian antes de salir del callback.
ws_rx_result_t ws_rx_feed(ws_rx_t *rx, uint8_t opcode, bool fin,
                         size_t frame_size, size_t offset,
                         const void *data, size_t size)
{

    // Close/Ping/Pong los atiende la biblioteca; pueden intercalarse sin romper el armado.
    if (opcode == 8 || opcode == 9 || opcode == 10) return WS_RX_CONTROL;
    // Rechazamos punteros/metadatos imposibles antes de copiar memoria.
    if (opcode > 2 || (data == NULL && size != 0) ||
        offset > frame_size || size > frame_size - offset ||
        (size == 0 && frame_size != 0)) {
        return reject(rx, WS_RX_INVALID);
    }

    // Empieza un frame: no debe quedar otro incompleto. Texto/binario abre mensaje;
    // una continuación requiere que ya exista un mensaje abierto.
    if (offset == 0) {
        if (rx->frame_open) return reject(rx, WS_RX_INVALID);
        if (opcode == 1 || opcode == 2) {
            if (rx->active) return reject(rx, WS_RX_INVALID);
            rx->message.type = (orbita_ws_type_t)opcode;
            rx->message.size = 0;
            rx->active = true;
        } else if (!rx->active) {
            return reject(rx, WS_RX_INVALID);
        }
        // Límite del MENSAJE completo: ORBITA_WS_MAX_CONTROL_BYTES (512), sumando todos sus frames.
        if (frame_size > ORBITA_WS_MAX_CONTROL_BYTES - rx->message.size) {
            return reject(rx, WS_RX_TOO_LARGE);
        }
        // Guardamos datos del frame para comprobar los siguientes trozos.
        rx->frame_open = true;
        rx->frame_size = frame_size;
        rx->frame_received = 0;
        rx->frame_opcode = opcode;
        rx->frame_fin = fin;
    }

    // Los trozos deben llegar contiguos y mantener tamaño, opcode y fin del mismo frame.
    if (!rx->active || !rx->frame_open || offset != rx->frame_received ||
        frame_size != rx->frame_size || opcode != rx->frame_opcode ||
        fin != rx->frame_fin) {
        return reject(rx, WS_RX_INVALID);
    }
    // Copiamos al final del mensaje y contamos el avance. No validamos UTF-8 aquí.
    if (size != 0) memcpy(rx->message.data + rx->message.size, data, size);
    rx->message.size += size;
    rx->frame_received += size;
    if (rx->frame_received != frame_size) return WS_RX_MORE;

    rx->frame_open = false;
    // Frame terminado: si no es el último, faltan frames de continuación del mensaje.
    if (!fin) return WS_RX_MORE;
    rx->active = false;
    // El callback puede copiar el mensaje completo a la cola antes del siguiente feed.
    return WS_RX_COMPLETE;
}
