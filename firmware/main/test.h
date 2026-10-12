/**
 * @file    test.h
 * @brief   MODULO DE LABORATORIO (se borra cuando exista el sink de sockets): graba tomas de
 *          test por UART0 como WAV de 2 canales (ch0 = mic L crudo, ch1 = mic R crudo, o la
 *          salida del AFE si ORBITA_PIPELINE_USA_AFE=1) y atiende los comandos 'g' y 's' por
 *          USB-Serial-JTAG. Se engancha al pipeline por callbacks: no toca la captura.
 *
 * @details Para borrarlo: eliminar test.c/.h, sacarlo de main/CMakeLists.txt y de app_main.
 */

#pragma once

#include "pipeline.h"

/** Instala los drivers UART0 y USB-Serial-JTAG y crea los rings de test. Va ANTES de orbita_pipeline_start(). */
void test_init(void);

/** Callbacks que hay que pasarle al pipeline. Valido despues de test_init(). */
const orbita_pipeline_cfg_t *test_pipeline_cfg(void);

/** Loop de comandos: 'g' graba una toma, 's' devuelve la linea "Fin:" de la ultima. No vuelve. */
void test_loop(void);
