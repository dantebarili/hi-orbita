idf_component_register(
    SRCS "pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES orbita_beamforming          # publico: pipeline.h lo incluye
    PRIV_REQUIRES orbita_audio esp_ringbuf esp_timer
)
