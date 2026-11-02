idf_component_register(
    SRCS "pipeline.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES orbita_audio orbita_beamforming esp_ringbuf esp_timer
)
