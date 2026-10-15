/**
 * @file    test_playback.h
 * @brief   PRUEBA LOCAL DEL PARLANTE (se borra cuando funcione): sin mics, sin red, sin servidor.
 *          Genera un tono de 1 kHz y lo pasa por orbita_playback (begin/push/end/stop).
 *
 * @details Para borrarlo: eliminar test_playback.c/.h y sdkconfig.playback, sacar la rama
 *          ORBITA_LAB_PLAYBACK de main/CMakeLists.txt, main/Kconfig.projbuild y main/main.c
 *          (todas marcadas con "PRUEBA PARLANTE").
 */

#pragma once

#include "esp_err.h"

/** Inicia el parlante y repite en bucle 3 pruebas (tono completo, stop, underrun). Solo vuelve si hay un error. */
esp_err_t test_playback_run(void);
