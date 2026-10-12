idf_component_register(
    SRCS "src/wlc_radio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_timer esp_ringbuf nvs_flash)
