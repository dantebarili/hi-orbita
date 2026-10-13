idf_component_register(
    SRCS "audio_ws_sink.c" "audio_ws_wire.c"
    INCLUDE_DIRS "include"
    REQUIRES orbita_audio
    PRIV_REQUIRES orbita_ws orbita_pipeline esp_hw_support heap log freertos
)
