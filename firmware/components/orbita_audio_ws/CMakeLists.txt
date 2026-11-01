idf_component_register(
    SRCS "audio_ws_sink.c" "audio_ws_wire.c" "audio_ws_playback.c" "audio_ws_conversacion.c" "audio_ws_led.c"
    INCLUDE_DIRS "include"
    REQUIRES orbita_audio orbita_ws
    PRIV_REQUIRES orbita_playback orbita_pipeline orbita_led esp_hw_support heap log freertos esp_timer
)
