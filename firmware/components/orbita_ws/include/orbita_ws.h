#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "orbita_ws_message.h"

typedef struct {
    const char *uri;          // ws://IP:puerto/ruta o wss://nombre/ruta
    const char *server_ca_pem; // Obligatorio con wss; PEM terminado en '\0'.
    const char *headers;      // Opcional; cada cabecera termina en \r\n.
    const char *subprotocol;  // Opcional; depende del contrato del backend.
} orbita_ws_config_t;

// Una instancia por arranque. Llamar después de inicializar la red.
// No llamar concurrentemente a start. Config/textos válidos durante start;
// el certificado debe permanecer válido durante toda la vida del cliente.
// ESP_OK significa cliente iniciado, no conexión establecida. Reconecta solo (backoff 1-2-4-8 s);
// sirve llamarla sin IP todavía.
// No ofrece stop/reconfiguración en caliente. Nunca llamar desde sus callbacks.
esp_err_t orbita_ws_start(const orbita_ws_config_t *config);
bool orbita_ws_is_connected(void);
esp_err_t orbita_ws_wait_connected(uint32_t timeout_ms);

// Envío síncrono: conservar data hasta el retorno. Texto debe ser UTF-8 válido.
// Tamaño 0 permitido. Sin cola TX ni reenvío automático de mensajes de usuario.
// Serializa emisores; los timeouts son por espera/escritura de la biblioteca,
// no una garantía de tiempo real para la duración total. Rango: 1..60000 ms.
// ESP_OK indica escritura, NO confirmación de recepción/procesamiento remoto.
// Usar desde una tarea de envío, nunca desde la captura de audio o un callback.
esp_err_t orbita_ws_send(orbita_ws_type_t type, const void *data,
                          size_t size, uint32_t timeout_ms);

// Un consumidor. Copia el siguiente mensaje completo en out (reservado por caller).
// Cola RX de 4 mensajes. Si se llena, descarta el nuevo y registra un contador.
// Mensajes >8 KiB o fragmentación inválida se rechazan. No entrega mensajes parciales.
// Texto NO termina en '\0'. Mensajes pendientes se descartan al reconectar.
// timeout_ms 0 consulta sin esperar. No enviar automáticamente lo recibido (eco infinito).
esp_err_t orbita_ws_receive(orbita_ws_message_t *out, uint32_t timeout_ms);

// ID local de conexión; 0 si no está conectado. No es el ID de toma del protocolo.
uint32_t orbita_ws_connection_id(void);

// Llamar periódicamente mientras no hay conexión (la usa el sink). Si la tarea de la
// biblioteca terminó (WEBSOCKET_EVENT_FINISH), la vuelve a arrancar. No bloquea.
void orbita_ws_keepalive(void);

// El adaptador la llama cuando el servidor confirma la sesión (primer ack): la conexión es
// estable y el backoff vuelve a 1 s. Si el servidor cierra apenas conectamos, el backoff sigue
// creciendo. Segura desde cualquier tarea.
void orbita_ws_backoff_reset(void);
