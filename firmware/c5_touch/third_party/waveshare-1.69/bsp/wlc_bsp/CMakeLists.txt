file(GLOB_RECURSE SRCS *.c)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_i2c  esp_driver_gpio esp_lcd 
    PRIV_REQUIRES esp_timer spiffs esp_psram fatfs
)
