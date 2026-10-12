idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include" "src"
    REQUIRES brookesia_core wlc_bsp app_update esp_wifi esp_timer esp_driver_usb_serial_jtag esp_vfs_console mbedtls wlc_radio wlc_survey wlc_generator wlc_rf_approval nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
