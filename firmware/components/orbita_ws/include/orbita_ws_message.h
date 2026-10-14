#pragma once
#include <stddef.h>
#include <stdint.h>

// ENVÍO (y buffer de la biblioteca): límite por mensaje, no por conexión. 8 KiB alcanza para
// los paquetes de audio de ~4 KiB.
#define ORBITA_WS_MAX_MESSAGE_BYTES 8192

// RECEPCIÓN por la cola RX: solo mensajes de CONTROL (JSON: ready, ack, orbita, play...).
// La cola copia el mensaje entero por valor, así que este tamaño se paga 6 veces en RAM interna
// (cola de 4 + s_rx + reply). Un mensaje recibido más grande se rechaza (WS_RX_TOO_LARGE).
// OJO: los frames binarios PLAY (hasta 4 KiB, audio del parlante) NO caben acá: necesitan su
// propio camino de recepción, con buffer propio, sin pasar por la cola (a cargo de Juani).
#define ORBITA_WS_MAX_CONTROL_BYTES 512

typedef enum {
    ORBITA_WS_TEXT = 1,
    ORBITA_WS_BINARY = 2
} orbita_ws_type_t;

typedef struct {
    orbita_ws_type_t type;
    size_t size;
    uint32_t connection_id; // Generación local de conexión; NO es session_id del backend.
    uint8_t data[ORBITA_WS_MAX_CONTROL_BYTES]; // Texto también usa size, sin '\0'.
} orbita_ws_message_t;
