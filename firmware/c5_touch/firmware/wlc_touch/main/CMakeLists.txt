idf_component_register(
    SRCS "main.cpp" "dark/scr_bg.c"
    INCLUDE_DIRS "."
    REQUIRES wlc_bsp brookesia_core wlc_apps nvs_flash)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
