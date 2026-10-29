# Solo se compila en el prototipo WebSocket (ahi la wake word va siempre). Las dependencias van
# SIEMPRE: en la primera pasada de CMake (cuando se calculan los requisitos) todavia no hay
# CONFIG_*, y un if() aca las dejaria afuera.
set(fuentes "")
set(archivos "")
if(CONFIG_ORBITA_LAB_WEBSOCKET)
    set(fuentes "wakeword.c" "wakeword_modelo.cc")
    set(archivos "modelos/orbita.tflite")
endif()

idf_component_register(
    SRCS ${fuentes}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES orbita_pipeline orbita_audio esp_ringbuf esp_timer
                  espressif__esp-tflite-micro esphome__esp-micro-speech-features
    EMBED_FILES ${archivos}
)
