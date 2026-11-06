idf_component_register(SRCS "orbita_nfc.c" "pn532_frame.c" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "." REQUIRES esp_driver_i2c esp_driver_gpio esp_timer)
