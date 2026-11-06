#pragma once

#include "esp_err.h"

/* NFC: LOGIN DEL MEDICO. Opcional: sin lector (o con el lector fallando) el equipo anda igual y el servidor
 * responde como asistente general. El driver es orbita_nfc (banco de Juani, experiments/nfc-pn532-irq).
 *  Lector:    PN532 por I2C (I2C_NUM_0, 100 kHz, direccion 0x24) + IRQ activa baja (NXP UM0701-02, §6.2 y §6.3.3).
 *  Pines:     pines.md. SDA/SCL usan las pull-up del modulo: en la PCB hay que ponerlas.
 *  Tarea:     "nfc", core 0, prioridad 2, pila 4 KB. Busca tarjeta cada 150 ms (cada busqueda prende el campo RF).
 *  Toque:     una tarjeta que aparece despues de 3 busquedas sin nada (~0,5 s sin tarjeta).
 *  Sesion:    sin sesion + toque = login. Toque de la MISMA tarjeta = logout. Otra tarjeta = la reemplaza (login).
 *             La tarjeta se puede sacar: la sesion sigue.
 *  Servidor:  {"type":"nfc","event":"login"|"logout","uid":"04A1B2C3"} en cada cambio (si hay conexion; si no,
 *             se pierde el aviso pero no el dato) y "uid" (texto o null) en cada `hello` y `wake`: una reconexion
 *             o un aviso perdido no dejan al servidor con el medico equivocado.
 *  Fallas:    si el lector no responde, se reintenta begin() con espera creciente (10 s a 60 s); no frena nada mas.
 *             Sin el pin RSTPD_N cableado no hay reset por hardware: en la PCB conviene llevarlo a un GPIO.
 *  Ojo:       el UID no prueba identidad (una tarjeta se puede clonar). Sirve para asociar, no para autenticar. */
#define AUDIO_WS_NFC_SDA_GPIO   17
#define AUDIO_WS_NFC_SCL_GPIO   18
#define AUDIO_WS_NFC_IRQ_GPIO   21
#define AUDIO_WS_NFC_UID_CHARS  21      // hasta 10 bytes en hex + '\0'

/** Crea el bus I2C, abre el lector y lanza la tarea. ESP_OK aunque el lector no conteste (lo reintenta la tarea);
 *  error solo si no se pudo crear el bus, la IRQ o la tarea. Llamar UNA vez. */
esp_err_t audio_ws_nfc_start(void);

/** Copia el UID de la sesion actual en hex mayusculas, sin separadores ("" = sin sesion). No bloquea: la usa el
 *  sink para el `hello` y el `wake`. */
void audio_ws_nfc_uid(char out[AUDIO_WS_NFC_UID_CHARS]);
