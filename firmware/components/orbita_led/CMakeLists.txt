idf_component_register(
    SRCS "orbita_led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES espressif__led_strip freertos esp_timer log
)
