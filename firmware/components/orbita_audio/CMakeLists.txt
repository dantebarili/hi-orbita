idf_component_register(
    SRCS "audio_afe.c" "audio_capture.c" "audio_ring_out.c" "audio_sink.c" "wav_writer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s esp_timer espressif__esp-sr
)
