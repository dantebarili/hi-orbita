#pragma once
#include "audio_sink.h"

// Un solo consumidor del ring: registrar este sink EN LUGAR de test_sink().
// Lo invoca tarea_sink; nunca llamar desde captura/on_crudo/on_afe.
const orbita_sink_t *orbita_audio_ws_sink(void);
