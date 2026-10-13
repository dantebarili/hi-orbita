/*
 * WI-FI DEL LABORATORIO: conecta la ESP a la red; no maneja WebSocket ni audio.
 * Nuestro componente organiza el uso del controlador que proporciona Espressif.
 * FreeRTOS es el sistema operativo que organiza la ejecución de las tareas.
 *
 * MAPA DEL RECORRIDO (no coincide con el orden de las funciones en el archivo):
 * 1. app_main prepara NVS, esp_netif y el event loop de ESP-IDF.
 * 2. app_main llama UNA VEZ a orbita_wifi_start(ssid, password): prepara recursos,
 *    registra on_network_event, arranca el controlador y crea connection_task.
 *    ESP_OK significa componente preparado, no conexión ni contraseña confirmada.
 * 3. El event loop ejecuta on_network_event ante eventos Wi-Fi/IP. Este callback
 *    actualiza banderas. connection_task espera peticiones y pide conectarse al
 *    controlador con esp_wifi_connect. El controlador realiza el intento real.
 * 4. De vuelta en app_main, orbita_wifi_wait_for_ip espera IP antes del WebSocket.
 *    Al terminar su espera consulta orbita_wifi_is_connected. Ninguna de estas
 *    dos funciones se llama dentro de orbita_wifi_start.
 * 5. Las desconexiones se resuelven entre callback y tarea, sin repetir start.
 *
 * NO CONFUNDIR:
 * - Event loop ESP-IDF: recibe eventos y llama a nuestro callback registrado.
 * - Grupo de eventos FreeRTOS (s_events): nuestras banderas compartidas.
 *   No guarda una cola de peticiones: varias pueden quedar reunidas en un bit.
 *   WaitBits bloquea al que llama si falta la señal. SetBits activa la señal y
 *   deja listas las tareas que esperaban por ella. No hay consulta continua:
 *   la propia función SetBits comprueba las esperas registradas en FreeRTOS.
 *   Una tarea lista continúa cuando FreeRTOS le asigna tiempo de procesador.
 */
// Interfaz propia: declara las funciones que puede usar app_main.
#include "orbita_wifi.h"

// Estado compartido sincronizado entre tareas y manejo de textos.
#include <stdatomic.h>
#include <string.h>

// FreeRTOS: tareas, esperas y banderas compartidas.
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
// Espressif: eventos, registros del monitor, interfaces de red y controlador Wi-Fi.
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

// Etiqueta de los registros de ESTE archivo; no se envía al servidor.
static const char *TAG = "orbita_wifi";

// Significados elegidos por nosotros, no por FreeRTOS:
// HAVE_IP_BIT: obtuvimos IP; CONNECT_REQUEST_BIT: queda un intento pendiente.
#define HAVE_IP_BIT          BIT0
#define CONNECT_REQUEST_BIT  BIT1

// Primer intento inmediato; los siguientes esperan 5 segundos.
#define RETRY_DELAY_MS       5000

// 4096 bytes para la pila de nuestra tarea, no toda la RAM Wi-Fi ni un buffer de audio.
#define CONNECT_TASK_STACK   4096

// Prioridad 3: entre tareas listas, las de mayor prioridad tienen preferencia.
#define CONNECT_TASK_PRIORITY 3

// Grupo creado en start con bits apagados. Lo comparten tarea y callback.
static EventGroupHandle_t s_events;
// Nuestra interfaz de red; permite reconocer a cuál interfaz se asignó la IP.
static esp_netif_t *s_netif;
// Inicialización completa, NO conexión confirmada. Se consulta desde otras tareas.
static atomic_bool s_started = false;
// Impide repetir la preparación una vez empezada, aunque un paso posterior falle.
static bool s_start_attempted;

/* CALLBACK: lo ejecuta la tarea del event loop, no app_main ni connection_task.
 * Se registra en start para dos familias. base indica familia; event_id, qué pasó;
 * event_data trae detalles. arg sería un dato adicional; aquí no lo necesitamos.
 * Actualiza banderas y logs sin esperar ni conectar: no debe frenar otros eventos.
 */
static void on_network_event(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg;

    // Controlador arrancado en modo estación: pedimos un intento, aún no hay conexión.
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
    // Conexión perdida o intento fallido. Quitamos IP, mostramos el motivo numérico
    // y pedimos reintento. ClearBits modifica nuestro estado, no borra una IP del driver.
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        xEventGroupClearBits(s_events, HAVE_IP_BIT);
        ESP_LOGW(TAG, "Wi-Fi desconectado (motivo=%u); se reintentara",
                 (unsigned)event->reason);
        xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
    // Controlador detenido, distinto de desconectado. Quitamos ambas señales.
    // En este módulo ocurre al limpiar un fallo; no ofrecemos parada/reinicio en uso normal.
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        xEventGroupClearBits(s_events, HAVE_IP_BIT | CONNECT_REQUEST_BIT);
    // DHCP consiguió IP: verificamos que sea NUESTRA interfaz y activamos HAVE_IP_BIT.
    // La IP mostrada es la de la ESP, no la del servidor. No garantiza acceso al servidor.
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        if (event->esp_netif == s_netif) {
            // IPSTR/IP2STR ayudan a imprimir la dirección en formato habitual.
            ESP_LOGI(TAG, "IPv4 obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(s_events, HAVE_IP_BIT);
        }
    // Sin IP utilizable; DHCP gestiona recuperarla. Aquí no pedimos otra conexión Wi-Fi.
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, HAVE_IP_BIT);
        ESP_LOGW(TAG, "IPv4 perdida; esperando renovacion por DHCP");
    }
}

/* TAREA: FreeRTOS ejecuta esta función tras xTaskCreate en start.
 * Tiene su propio recorrido mientras app_main y el event loop siguen trabajando.
 * Espera una petición, pide conectarse y vuelve a esperar. El for no ocupa CPU
 * continuamente: WaitBits y el retardo bloquean solamente esta tarea.
 */
static void connection_task(void *arg)
{
    (void)arg;
    bool first_attempt = true;

    for (;;) {
        // Si no hay petición, queda bloqueada; SetBits desde el callback la deja lista.
        // Si el bit ya estaba activo, continúa sin bloquearse. pdTRUE consume la
        // petición, sin borrar HAVE_IP_BIT. pdFALSE: basta cualquiera de los bits
        // solicitados (aquí uno). portMAX_DELAY: espera indefinida con la configuración
        // habitual de ESP-IDF. Continúa después de esta llamada, no reinicia la función.
        xEventGroupWaitBits(s_events, CONNECT_REQUEST_BIT,
                            pdTRUE, pdFALSE, portMAX_DELAY);

        // Desde el segundo intento, esperar 5 s; las demás tareas siguen ejecutándose.
        if (!first_attempt) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
        first_attempt = false;

        // La IP pudo llegar durante la espera. Si ya tenemos, no pedimos reconectar.
        if ((xEventGroupGetBits(s_events) & HAVE_IP_BIT) != 0) {
            continue;
        }

        ESP_LOGI(TAG, "Iniciando intento de conexion Wi-Fi");
        // AQUÍ pedimos conectarse, usando la red configurada en start. ESP_OK acepta
        // el intento; el resultado llegará después como eventos al callback.
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {

            // Falló la solicitud misma: quizá no llegue evento de desconexión.
            // Dejamos nosotros otra petición; el próximo intento tendrá su retardo.
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            xEventGroupSetBits(s_events, CONNECT_REQUEST_BIT);
        }

    }
}

/* INICIALIZACIÓN: la llama app_main con SSID/clave incorporados al firmware.
 * Requiere NVS, infraestructura de red y event loop preparados. No espera IP.
 * No llamar simultáneamente ni repetir para reconectar: de eso se encarga la tarea.
 */
esp_err_t orbita_wifi_start(const char *ssid, const char *password)
{

    // Evitamos duplicar recursos, callback y tarea durante este arranque.
    if (s_start_attempted) {
        return ESP_ERR_INVALID_STATE;
    }
    // Ambos textos deben existir. Clave vacía sí es válida para una red abierta.
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // SSID de 1..32 bytes; clave de 8..63 o vacía. Bytes no siempre son caracteres.
    // Medimos un byte extra para detectar exceso sin recorrer todo ni truncar textos.
    // Esto valida el formato admitido, no que la contraseña sea la correcta.
    size_t ssid_len = strnlen(ssid, 33);
    size_t password_len = strnlen(password, 64);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 63 ||
        (password_len != 0 && password_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Esperamos el resultado NOT_INIT: el driver todavía no está inicializado.
    // WIFI_STA_DEF identifica internamente la interfaz, no es el nombre de la red.
    // Si otro componente ya usa driver/interfaz, no tomamos control de ellos.
    wifi_mode_t existing_mode;
    esp_err_t err = esp_wifi_get_mode(&existing_mode);
    if (err != ESP_ERR_WIFI_NOT_INIT ||
        esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Desde aquí no permitimos repetir la inicialización, aunque falle más adelante.
    s_start_attempted = true;

    // Seguimiento para la limpieza: inicializar y arrancar son etapas distintas.
    // Los handlers identifican registros del callback para poder quitarlos; no son tareas.
    bool driver_initialized = false;
    bool driver_started = false;
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;

    // Creamos las banderas en RAM, todas apagadas. NULL indica falta de memoria.
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // main preparó la infraestructura general; aquí creamos nuestra interfaz estación.
    // Incluye DHCP para pedir una IP al router al conectarse. Todavía no conecta.
    // Si falla, fail también deberá liberar el grupo de banderas ya creado.
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    // Configuración interna del driver (recursos/buffers), no SSID ni contraseña.
    // Los valores predeterminados habilitan soporte NVS; main ya lo inicializó.
    // esp_wifi_init prepara el controlador; más adelante esp_wifi_start lo arranca.
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) goto fail;
    driver_initialized = true;

    // Mantenemos los ajustes del driver en RAM, sin guardarlos mediante NVS.
    // No deshabilita el soporte NVS ni quita la clave del firmware en flash.
    // Este componente no guarda audio ni mensajes en NVS.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;
    // Estación: cliente de un router, no creador de una red propia.
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) goto fail;

    // Ahora sí: entregamos nombre de red y contraseña recibidos desde main.
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, ssid, ssid_len);
    memcpy(wifi_config.sta.password, password, password_len);
    // Umbral de autenticación: abierto sin clave, mínimo WPA2 con clave.
    // No modifica el router ni configura acceso a redes empresariales.
    wifi_config.sta.threshold.authmode = password_len == 0
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    // El controlador conserva estos ajustes de estación en RAM; todavía no conecta.
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) goto fail;

    // Registramos NUESTRA on_network_event (arriba), para todos los eventos Wi-Fi.
    // Registrar no ejecuta: el event loop la llamará al procesar un evento.
    // ANY_ID: toda la familia; el callback ignora los casos que no contempla.
    // NULL llega como arg; wifi_handler permite quitar este registro si hay un fallo.
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_network_event, NULL, &wifi_handler);
    if (err != ESP_OK) goto fail;
    // La MISMA función atiende IP; dentro distingue la familia por base y event_id.
    err = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, on_network_event, NULL, &ip_handler);
    if (err != ESP_OK) goto fail;

    // Arranca y genera STA_START: el callback activa CONNECT_REQUEST_BIT.
    // No es esp_wifi_connect. Si la tarea aún no existe, la bandera conserva la petición.
    err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    driver_started = true;

    // Creamos la tarea que ejecuta connection_task, NO el callback. Nombre para
    // depurar, pila de 4096 bytes, sin argumento, prioridad 3, sin guardar handle.
    // FreeRTOS puede ejecutarla desde ahora; start no entra en su bucle.
    // La creamos al final: ante fallos anteriores aún no hay otra tarea nuestra
    // usando los recursos que necesitamos liberar.
    if (xTaskCreate(connection_task, "orbita_wifi", CONNECT_TASK_STACK,
                    NULL, CONNECT_TASK_PRIORITY, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    // Publicamos el estado de forma sincronizada: todo preparado, NO IP confirmada.
    // Volvemos a main; la tarea y el callback siguen trabajando por su cuenta.
    atomic_store(&s_started, true);
    return ESP_OK;

// Solo errores de preparación; el return ESP_OK evita entrar aquí en caso exitoso.
// Deshacemos lo creado: parar, quitar callbacks, liberar driver/interfaz y grupo.
// CHECK detecta fallos de limpieza y evita seguir liberando recursos potencialmente
// en uso. Esta sección no atiende las desconexiones normales del router.

fail:
    ESP_LOGE(TAG, "No se pudo iniciar Wi-Fi: %s", esp_err_to_name(err));

    if (driver_started) {
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    if (ip_handler != NULL) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
            IP_EVENT, ESP_EVENT_ANY_ID, ip_handler));
    }
    if (wifi_handler != NULL) {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler));
    }
    if (driver_initialized) {
        ESP_ERROR_CHECK(esp_wifi_deinit());
    }
    if (s_netif != NULL) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    vEventGroupDelete(s_events);
    s_events = NULL;
    return err;
}

/* ESPERA: app_main la llama DESPUÉS de start y antes de iniciar WebSocket.
 * No es otra tarea ni un callback; no se llama dentro de orbita_wifi_start.
 * Bloquea al que la llama (aquí la tarea de app_main), mientras connection_task
 * y el event loop siguen trabajando. Main repite si hay TIMEOUT, mostrando un aviso.
 * El timeout termina esta espera, NO cancela los intentos de conexión.
 */
esp_err_t orbita_wifi_wait_for_ip(uint32_t timeout_ms)
{
    if (!atomic_load(&s_started)) {
        return ESP_ERR_INVALID_STATE;
    }

    // Convertimos ms a ticks redondeando hacia arriba para no perder esperas pequeñas.
    // Limitamos el valor para no convertir una espera finita en indefinida.
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999) / 1000;
    if (ticks >= (uint64_t)portMAX_DELAY) {
        ticks = (uint64_t)portMAX_DELAY - 1;
    }
    // Esperamos HAVE_IP_BIT, activada por el callback al recibir GOT_IP.
    // Con IP continúa directamente; sin IP espera señal o timeout.
    // Primer pdFALSE: no consumimos este estado; otros también necesitan consultarlo.
    xEventGroupWaitBits(s_events, HAVE_IP_BIT, pdFALSE, pdFALSE, (TickType_t)ticks);

    // Volvemos a consultar: pudo perderse IP antes de que nos tocara ejecutar.
    // Sin IP devolvemos TIMEOUT, incluso tras una señal transitoria.
    // No garantiza que la conexión no se caiga después de devolver ESP_OK.
    return orbita_wifi_is_connected() ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* CONSULTA INMEDIATA: actualmente la usa wait_for_ip, justo arriba.
 * También queda disponible para otros usuarios del componente. No espera ni conecta.
 * Solo lee componente iniciado + bandera IP mantenida por el callback.
 * No envía paquetes ni comprueba acceso al router, Internet o servidor WebSocket.
 */
bool orbita_wifi_is_connected(void)
{
    if (!atomic_load(&s_started)) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & HAVE_IP_BIT) != 0;
}
