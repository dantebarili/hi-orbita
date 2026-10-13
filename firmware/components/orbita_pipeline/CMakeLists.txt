idf_component_register(
    SRCS "pipeline.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES orbita_audio esp_ringbuf esp_timer
)
