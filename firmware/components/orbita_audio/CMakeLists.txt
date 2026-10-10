idf_component_register(
    SRCS "audio_capture.c" "wav_writer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s
)
