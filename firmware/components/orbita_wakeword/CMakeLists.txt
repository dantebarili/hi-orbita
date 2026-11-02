idf_component_register(
    SRCS "wakeword.c" "wakeword_modelo.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES orbita_pipeline orbita_audio esp_ringbuf esp_timer
                  espressif__esp-tflite-micro esphome__esp-micro-speech-features
    EMBED_FILES "modelos/orbita.tflite"
)
