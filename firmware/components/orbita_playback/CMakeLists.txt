idf_component_register(
    SRCS "orbita_playback.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2s esp_ringbuf
)
