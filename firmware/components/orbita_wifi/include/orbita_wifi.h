#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Interfaz del componente Wi-Fi del laboratorio.
 *
 * Este archivo DECLARA las funciones: su implementación está en orbita_wifi.c.
 * El resto del firmware incluirá este header sin conocer los detalles del driver.
 * Este componente conecta a la red; no abre WebSockets ni conoce el audio.
 */

/**
 * Inicializa el componente e inicia la conexión a una red Wi-Fi.
 *
 * ssid: nombre de la red (no la IP del servidor).
 * password: contraseña de la red. No debe escribirse en los logs.
 * Ambos son cadenas de C terminadas en '\0'; el componente copiará sus valores.
 *
 * ESP_OK significa que el arranque fue aceptado, NO que ya tenemos conexión.
 * La obtención de IP ocurre después; se espera con orbita_wifi_wait_for_ip().
 *
 * Una inicialización por arranque; conexión en modo
 * estación y reintentos con pausa ante caídas de Wi-Fi. No bloquea esperando IP.
 * Llamar desde app_main, sin llamadas concurrentes a start. Después de validar
 * argumentos y disponibilidad del driver, solo se permite un intento de arranque
 * por boot (incluso si falla al reservar recursos). Otra llamada devuelve
 * ESP_ERR_INVALID_STATE. Un argumento inválido no consume ese intento.
 *
 * Requisitos de app_main: inicializar NVS, esp_netif y el event loop por defecto
 * antes de llamar. Son servicios globales compartidos con otros componentes.
 * Este módulo administra el driver Wi-Fi durante toda la ejecución; otros
 * componentes no deben llamar directamente a esp_wifi_start/stop/connect.
 * Las consultas/esperas concurrentes están permitidas después de que start retorne.
 *
 * SSID: 1-32 bytes. Clave: 8-63 bytes; vacía para una red abierta.
 * Base prevista: Wi-Fi personal WPA2 (no aprovisionamiento ni WPA-Enterprise).
 * No se admite aquí una PSK hexadecimal de 64 caracteres.
 * Nunca imprime la clave ni la guarda como configuración Wi-Fi en NVS.
 */
esp_err_t orbita_wifi_start(const char *ssid, const char *password);

/**
 * Espera hasta tener conexión con una dirección IPv4, con un límite de tiempo.
 *
 * timeout_ms: espera máxima en milisegundos; 0 consulta sin esperar.
 * Devuelve ESP_OK si hay IP, ESP_ERR_TIMEOUT si vence la espera y
 * ESP_ERR_INVALID_STATE si todavía no se inicializó el componente.
 *
 * La espera suspenderá solo la tarea que llama, sin ocupar CPU en un bucle.
 * Tener IP no garantiza que el servidor esté disponible ni que un envío funcione.
 */
esp_err_t orbita_wifi_wait_for_ip(uint32_t timeout_ms);

/**
 * Consulta sin esperar si el componente tiene conexión Wi-Fi y una IPv4.
 * Devuelve false antes de inicializarse o si no hay conexión disponible.
 *
 * Es una fotografía del estado: la red puede caerse inmediatamente después.
 * El módulo WebSocket deberá manejar sus propios errores de conexión y envío.
 */
bool orbita_wifi_is_connected(void);
