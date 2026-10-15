idf_component_register(
    SRCS "orbita_ws.c" "ws_rx.c" "ws_downlink.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_common
    PRIV_REQUIRES esp_websocket_client esp_event freertos log heap espressif__cjson
)
