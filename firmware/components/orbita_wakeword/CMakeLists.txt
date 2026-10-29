# Solo se compila codigo si la wake word esta activa (banco, o prototipo WebSocket con
# ORBITA_WAKEWORD). Las dependencias van SIEMPRE: en la primera pasada de CMake (cuando se calculan los
# requisitos) todavia no hay CONFIG_*, y un if() aca las dejaria afuera.
set(fuentes "")
set(archivos "")
if(CONFIG_ORBITA_WAKEWORD)
    set(fuentes "wakeword.c" "wakeword_modelo.cc")
    # Se embebe SOLO el modelo elegido (y el WAV de la prueba bit a bit si esta activada).
    if(CONFIG_ORBITA_WW_ORBITA)
        list(APPEND archivos "modelos/orbita.tflite")
    else()
        list(APPEND archivos "modelos/okay_nabu.tflite")
    endif()
    if(CONFIG_ORBITA_WW_PRUEBA_WAV)
        list(APPEND archivos "modelos/prueba.wav")
    endif()
endif()

idf_component_register(
    SRCS ${fuentes}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES orbita_pipeline orbita_audio esp_ringbuf esp_timer
                  espressif__esp-tflite-micro esphome__esp-micro-speech-features
    EMBED_FILES ${archivos}
)
