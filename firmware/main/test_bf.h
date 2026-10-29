/**
 * @file    test_bf.h
 * @brief   PRUEBA DE BEAMFORMING (rama prueba-beamforming): graba ~5 s estereo en PSRAM, les pasa el
 *          beamformer propio (orbita_beamforming) y manda por UART0 un WAV de 3 canales:
 *          ch0 = mic L crudo, ch1 = mic R crudo, ch2 = beamformado. Mismo protocolo que test.c
 *          ('g' graba, 's' repite la linea "Fin:"), asi que sirve tools/orbita_serial.py tal cual.
 *
 * @details Primero se graba y despues se procesa/envia: el envio de 3 canales (96 KB/s) no entra en
 *          vivo por la UART a 921600 baud (~92 KB/s). El beamformer es causal, asi que el resultado es
 *          el mismo que si corriera en la captura. Se elige en menuconfig (Orbita integration ->
 *          Modo del firmware -> Prueba de beamforming). Necesita el AFE apagado.
 */

#pragma once

#include "pipeline.h"

/** Drivers UART0 / USB-Serial-JTAG y buffer de la toma en PSRAM. Va ANTES de orbita_pipeline_start(). */
void test_bf_init(void);

/** Callbacks para el pipeline (solo on_crudo). */
const orbita_pipeline_cfg_t *test_bf_pipeline_cfg(void);

/** Loop de comandos: 'g' graba + procesa + envia, 's' repite la linea "Fin:". No vuelve. */
void test_bf_loop(void);
