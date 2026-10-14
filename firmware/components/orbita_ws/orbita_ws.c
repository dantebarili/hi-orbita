/*
 * TRANSPORTE WEBSOCKET: nuestro módulo usa esp_websocket_client de Espressif.
 * No genera datos ni interpreta audio. El adaptador está en orbita_audio_ws/audio_ws_sink.c.
 *
 * MAPA: app_main -> orbita_ws_start, una vez (no hace falta tener IP: reconecta sola).
 * La biblioteca crea su tarea para conectar, recibir y manejar el protocolo.
 * Desde ella entrega eventos a nuestro CALLBACK on_ws_event. Este no es una tarea
 * creada por nosotros ni el callback Wi-Fi del event loop predeterminado.
 *
 * ENVÍO: tarea sink -> adaptador audio_ws_sink -> send -> biblioteca -> servidor.
 * RECEPCIÓN: biblioteca -> on_ws_event -> ws_rx_feed (ws_rx.c) -> cola RX -> receive,
 * llamada por el adaptador para esperar ready/saved. Los callbacks de red siguen
 * recibiendo mientras la tarea sink espera. No se hace eco del audio.
 *
 * ESP_OK al iniciar no significa conectado. ESP_OK al enviar no significa que el
 * servidor procesó los datos. Reconectar no reenvía mensajes que pudieron llegar.
 * Una instancia por arranque; no ofrecemos parada/reconfiguración en uso normal.
 */
// API pública propia y reconstructor interno de mensajes (ws_rx.h).
#include "orbita_ws.h"
#include "ws_rx.h"
// Estado sincronizado entre tareas y manejo de textos de configuración.
#include <stdatomic.h>
#include <string.h>
// FreeRTOS provee banderas, cola de mensajes y mutex (permiso exclusivo para enviar).
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
// Logs del monitor y biblioteca cliente WebSocket de Espressif.
#include "esp_log.h"
#include "esp_websocket_client.h"

// Bandera de ESTE grupo, distinta de HAVE_IP_BIT del Wi-Fi: WebSocket conectado.
#define CONNECTED_BIT BIT0
// Hasta cuatro mensajes completos pendientes; si se llena, descartamos el nuevo.
#define RX_QUEUE_LENGTH 4
// Etiqueta de los registros del módulo, no se transmite al servidor.
static const char *TAG = "orbita_ws";
// Referencia al cliente creado en start; se usa para las operaciones de transporte.
static esp_websocket_client_handle_t s_client;
// El callback modifica CONNECTED_BIT; los emisores pueden esperar esa señal.
static EventGroupHandle_t s_events;
// Cola en RAM: el callback copia mensajes y la tarea receptora los retira.
static QueueHandle_t s_rx_queue;
// Mutex para que dos emisores no intercalen envíos. No es una cola TX
// ni impide por sí mismo que el receptor trabaje mientras enviamos.
static SemaphoreHandle_t s_tx_lock;
// Inicialización completa, no conexión confirmada.
static atomic_bool s_started;
// Número LOCAL que cambia al conectar: permite rechazar respuestas de otra conexión.
// No se transmite ni representa el ID de sesión/segmento del protocolo del backend.
static atomic_uint s_generation;
// Evita repetir la inicialización después de empezar a crear recursos.
static bool s_start_attempted;
// Buffer/estado de reconstrucción en RAM estática, fuera de la pila del callback.
static ws_rx_t s_rx;
// Espera ANTES del próximo intento de reconexión: 1, 2, 4, 8 s y se queda en 8 s.
// Vuelve a 1 s al conectar. La usa solo el callback (tarea de la biblioteca).
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 8000
static atomic_int s_backoff_ms = BACKOFF_MIN_MS;     // lo tocan el callback y el sink (backoff_reset)
// La biblioteca terminó su tarea: lo anota el callback, lo atiende orbita_ws_keepalive.
static atomic_bool s_needs_restart;
// Contadores de cola llena y datos rechazados, para diagnóstico en los logs.
static unsigned s_dropped;
static unsigned s_rejected;

// Ayuda de wait_connected/send/receive: ms a ticks, redondeo hacia arriba y límite finito.
static TickType_t to_ticks(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1;
    return (TickType_t)ticks;
}

// Usada al conectar/desconectar: descarta armado parcial y cola para no mezclar conexiones.
static void reset_received(void)
{
    ws_rx_reset(&s_rx);
    xQueueReset(s_rx_queue);
}

/* CALLBACK: registrado por start con esp_websocket_register_events.
 * id indica qué pasó; event_data trae los detalles. arg/base no se usan aquí.
 * No esperar a consumidores ni enviar desde aquí: devolver pronto a la biblioteca.
 */
static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *event = event_data;
    // Conectó: limpiar recepción vieja, avanzar ID local y despertar a quienes esperan conexión.
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        reset_received();
        unsigned generation = atomic_fetch_add(&s_generation, 1) + 1;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        ESP_LOGI(TAG, "WebSocket conectado; conexion local=%u", generation);
    // Apagamos el estado y descartamos pendientes. FINISH es tarea terminada,
    // no una desconexión para la que podamos afirmar que continuará reconectando.
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED ||
               id == WEBSOCKET_EVENT_FINISH) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        reset_received();
        if (id == WEBSOCKET_EVENT_FINISH) {
            // Sin tarea no hay reconexión automática: el sink la reinicia (keepalive).
            atomic_store(&s_needs_restart, true);
            ESP_LOGE(TAG, "La tarea WebSocket termino; se reiniciara desde keepalive");
        } else {
            // Backoff: la biblioteca espera este valor antes del próximo intento. OJO: este callback
            // corre en el hilo que provoca el corte, que puede ser la tarea de la biblioteca o la
            // tarea que llamó a send (el sink, con su pila chica). set_reconnect_timeout no bloquea.
            int espera = atomic_load(&s_backoff_ms);
            esp_websocket_client_set_reconnect_timeout(s_client, espera);
            ESP_LOGW(TAG, "WebSocket desconectado; reintento en %d ms", espera);
            atomic_store(&s_backoff_ms, espera * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : espera * 2);
        }
    // Evitamos imprimir URI/cabeceras: podrían contener credenciales.
    } else if (id == WEBSOCKET_EVENT_ERROR) {

        ESP_LOGW(TAG, "Error de transporte/negociacion WebSocket");
    // DATA puede traer solo un trozo. Validamos longitudes antes de convertirlas.
    // ws_rx_feed arma el mensaje: op_code indica tipo; fin, último frame;
    // payload_len/offset describen el frame; data_ptr/data_len describen ESTE trozo.
    } else if (id == WEBSOCKET_EVENT_DATA) {
        if (event->data_len < 0 || event->payload_len < 0 || event->payload_offset < 0) {
            ws_rx_reset(&s_rx);
            ESP_LOGW(TAG, "Metadatos RX invalidos; rechazados=%u", ++s_rejected);
            return;
        }
        ws_rx_result_t result = ws_rx_feed(&s_rx, event->op_code, event->fin,
            (size_t)event->payload_len, (size_t)event->payload_offset,
            event->data_ptr, (size_t)event->data_len);
        // Solo mensajes completos van a la cola. MORE espera; CONTROL no es contenido de aplicación.
        if (result == WS_RX_COMPLETE) {
            s_rx.message.connection_id = atomic_load(&s_generation);

            // Copia el mensaje sin esperar (0); s_rx podrá reutilizarse. Si no hay
            // lugar, descartamos el nuevo en vez de frenar el procesamiento de red.
            if (xQueueSend(s_rx_queue, &s_rx.message, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Cola RX llena; descartados=%u", ++s_dropped);
            }
        } else if (result == WS_RX_INVALID || result == WS_RX_TOO_LARGE) {
            ESP_LOGW(TAG, "Mensaje RX rechazado (causa=%d); total=%u",
                     (int)result, ++s_rejected);
        }
    }
}

// INICIALIZACIÓN llamada por app_main, no por las tareas para reconectar.
// Prepara cliente/callback/recursos. Todavía no espera conexión con el servidor.
esp_err_t orbita_ws_start(const orbita_ws_config_t *config)
{
    // Validación básica de instancia, configuración y esquema. No analiza toda la URL.
    if (s_start_attempted) return ESP_ERR_INVALID_STATE;
    if (config == NULL || config->uri == NULL) return ESP_ERR_INVALID_ARG;
    bool tls = strncmp(config->uri, "wss://", 6) == 0;
    if (!tls && strncmp(config->uri, "ws://", 5) != 0) return ESP_ERR_INVALID_ARG;
    if (strlen(config->uri) <= (tls ? 6u : 5u)) return ESP_ERR_INVALID_ARG;
    // WSS exige CA. main hoy usa WS local: para TLS hay que proporcionar certificado
    // y reloj adecuados; nunca saltar validación para hacerlo funcionar.
    if (tls && (config->server_ca_pem == NULL || config->server_ca_pem[0] == '\0')) {
        ESP_LOGE(TAG, "WSS requiere el certificado CA del servidor");
        return ESP_ERR_INVALID_ARG;
    }
    s_start_attempted = true;
    esp_err_t err = ESP_ERR_NO_MEM;
    // Recursos FreeRTOS: bandera inicialmente apagada, cola vacía y mutex.
    // La cola reserva memoria para 4 estructuras completas, incluidos sus buffers.
    s_events = xEventGroupCreate();
    s_rx_queue = xQueueCreate(RX_QUEUE_LENGTH, sizeof(orbita_ws_message_t));
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_events == NULL || s_rx_queue == NULL || s_tx_lock == NULL) goto fail;

    // Ajustes de la biblioteca. Headers/subprotocol adaptan el contrato del servidor.
    // El certificado debe mantenerse válido durante toda la vida del cliente.
    esp_websocket_client_config_t client_config = {
        .uri = config->uri,
        .cert_pem = config->server_ca_pem,
        .headers = config->headers,
        .subprotocol = config->subprotocol,
        // Conservamos verificación de identidad del servidor en TLS.
        .skip_cert_common_name_check = false,
        // Reintentar conexión también ante cierre normal; la espera la fija el backoff (1 s al inicio).
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = BACKOFF_MIN_MS,   // el callback lo sube hasta BACKOFF_MAX_MS
        // Tiempo de operaciones de red, no duración máxima de toda nuestra API.
        .network_timeout_ms = 5000,
        // Ping/Pong detecta conexión sin respuesta. La biblioteca lo gestiona;
        // no son nuestros mensajes de eco ni se entregan como texto/binario.
        // Heartbeat de la FSM del vault: ping cada 10 s, sin pong en 3 s = conexión caída.
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 3,
        .disable_pingpong_discon = false,
        // Buffer interno de 8192 bytes (envío y lectura de la biblioteca); ws_rx limita el mensaje
        // RECIBIDO por la cola a ORBITA_WS_MAX_CONTROL_BYTES (512).
        // La tarea de la biblioteca tiene pila propia de 6144 bytes y prioridad 4.
        .buffer_size = ORBITA_WS_MAX_MESSAGE_BYTES,
        .task_stack = 6144,
        .task_prio = 4,
        // Toda la red en el core 0 (captura y AFE en el core 1).
        .task_core_id_set = true,
        .task_core_id = 0,
        .task_name = "orbita_ws",
    };
    // Crea el cliente; aún falta arrancar su tarea.
    s_client = esp_websocket_client_init(&client_config);
    if (s_client == NULL) goto fail;
    // Aquí registramos NUESTRA on_ws_event para todos los eventos del cliente.
    // No ejecuta el callback ahora; NULL es el argumento adicional no utilizado.
    err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    if (err != ESP_OK) goto fail;
    // Arranca el trabajo de conexión/recepción en la tarea de la biblioteca.
    err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) goto fail;
    // Publicamos componente preparado; app_main podrá crear las tareas de prueba.
    atomic_store(&s_started, true);
    return ESP_OK;

// Solo fallo de preparación, no caída normal: liberar lo creado antes de publicar
// el cliente. CHECK detecta fallo interno de destrucción antes de seguir liberando.

fail:

    if (s_client != NULL) {
        ESP_ERROR_CHECK(esp_websocket_client_destroy(s_client));
        s_client = NULL;
    }
    if (s_tx_lock != NULL) vSemaphoreDelete(s_tx_lock);
    if (s_rx_queue != NULL) vQueueDelete(s_rx_queue);
    if (s_events != NULL) vEventGroupDelete(s_events);
    s_tx_lock = NULL;
    s_rx_queue = NULL;
    s_events = NULL;
    return err;
}

// CONSULTA inmediata usada por send/receive/wait_connected y el adaptador.
// Lee la bandera mantenida por el callback, no envía paquetes para comprobar la red.
bool orbita_ws_is_connected(void)
{
    return atomic_load(&s_started) &&
        (xEventGroupGetBits(s_events) & CONNECTED_BIT) != 0;
}

// ESPERA opcional antes de enviar: bloquea al llamador, no al cliente.
// pdFALSE conserva CONNECTED_BIT para otros lectores; timeout no cancela reconexión.
esp_err_t orbita_ws_wait_connected(uint32_t timeout_ms)
{
    if (!atomic_load(&s_started)) return ESP_ERR_INVALID_STATE;
    xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdFALSE, to_ticks(timeout_ms));
    return orbita_ws_is_connected() ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ENVÍO llamado por el adaptador con control JSON o audio binario.
 * Síncrono: conservar data hasta que termine. No hay cola TX ni reenvío automático.
 * Texto requiere UTF-8 válido; tamaño cero permitido; máximo 8192 bytes.
 * No usar desde captura de audio ni callbacks porque puede bloquear al llamador.
 * Timeout 1..60000 ms por espera/escritura, no garantía de plazo total estricto.
 */
esp_err_t orbita_ws_send(orbita_ws_type_t type, const void *data,
                          size_t size, uint32_t timeout_ms)
{
    if ((type != ORBITA_WS_TEXT && type != ORBITA_WS_BINARY) ||
        (data == NULL && size != 0) || size > ORBITA_WS_MAX_MESSAGE_BYTES ||
        timeout_ms == 0 || timeout_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (!orbita_ws_is_connected()) return ESP_ERR_INVALID_STATE;
    // Esperamos permiso del mutex si otro emisor está usando el transporte.
    TickType_t timeout = to_ticks(timeout_ms);
    if (xSemaphoreTake(s_tx_lock, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    // Pudo caer la conexión mientras esperábamos: consultar de nuevo antes de escribir.
    if (orbita_ws_is_connected()) {
        unsigned generation = atomic_load(&s_generation);

        // Puntero válido incluso para mensaje vacío; texto/binario se envían con distinta API.
        const char *payload = size == 0 ? "" : (const char *)data;
        int sent = type == ORBITA_WS_TEXT
            ? esp_websocket_client_send_text(s_client, payload, (int)size, timeout)
            : esp_websocket_client_send_bin(s_client, payload, (int)size, timeout);
        // Éxito local exige tamaño esperado y misma generación de conexión.
        // No prueba procesamiento remoto. Ante fallo no repetir: pudo llegar al servidor.
        err = sent == (int)size && generation == atomic_load(&s_generation)
            ? ESP_OK : ESP_FAIL;

    }
    // Liberamos el permiso para otros emisores, también ante error.
    xSemaphoreGive(s_tx_lock);
    return err;
}

/* RECEPCIÓN llamada por el adaptador para retirar controles del servidor.
 * Retira una COPIA de la cola que llena el callback, no lee directamente del socket.
 * Un consumidor; out reserva una estructura completa. Texto no agrega terminador NUL.
 * Si no hay mensajes, espera hasta el timeout; 0 consulta sin esperar.
 */
esp_err_t orbita_ws_receive(orbita_ws_message_t *out, uint32_t timeout_ms)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_started)) return ESP_ERR_INVALID_STATE;
    if (xQueueReceive(s_rx_queue, out, to_ticks(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    // No presentar una respuesta de otra conexión como actual tras una reconexión.
    if (!orbita_ws_is_connected() || out->connection_id != atomic_load(&s_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// Si el callback anotó FINISH, intenta arrancar de nuevo la tarea de la biblioteca.
// start puede fallar si aún está terminando: la bandera queda y se reintenta en la
// próxima llamada.
void orbita_ws_keepalive(void)
{
    if (!atomic_load(&s_started) || !atomic_load(&s_needs_restart)) return;
    if (esp_websocket_client_start(s_client) == ESP_OK) {
        atomic_store(&s_needs_restart, false);
        ESP_LOGW(TAG, "Cliente WebSocket reiniciado");
    }
}

void orbita_ws_backoff_reset(void)
{
    atomic_store(&s_backoff_ms, BACKOFF_MIN_MS);
}

uint32_t orbita_ws_connection_id(void)
{
    return orbita_ws_is_connected() ? atomic_load(&s_generation) : 0;
}
