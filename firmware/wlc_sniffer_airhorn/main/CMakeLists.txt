if(NOT CMAKE_BUILD_EARLY_EXPANSION)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${COMPONENT_DIR}/../prepare_sources.py"
        --deps "${WLC_DEPS_DIR}" --output "${generated}"
    RESULT_VARIABLE prepare_result
)
if(NOT prepare_result EQUAL 0)
    message(FATAL_ERROR "Combined firmware source preparation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../wlc_sniffer/wlc_sniffer.ino"
    "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_sources.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/combined.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/command_policy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/generator_policy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/rf_buffers.ld"
    "${CMAKE_CURRENT_SOURCE_DIR}/../prepare_rf_archive.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/nimble_cpp/sdk_host.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdkconfig.defaults"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dependency-lock.json")
# Retune source changes must rerun the guarded generator, even though the C
# compiler consumes generated copies of those inputs.
file(GLOB_RECURSE backend_inputs CONFIGURE_DEPENDS
    "${WLC_DEPS_DIR}/esp-sdr/main/*.c" "${WLC_DEPS_DIR}/esp-sdr/main/*.h"
    "${WLC_DEPS_DIR}/esp-sdr/main/*.ld")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${backend_inputs})
endif()
idf_component_register(
    SRCS "combined.cpp" "${generated}/receiver.c" "${generated}/spectrum.c"
        "${WLC_DEPS_DIR}/esp-sdr/main/common/burst_serial.c"
        "${WLC_DEPS_DIR}/esp-sdr/main/common/spectrum_stats.c"
    INCLUDE_DIRS "." "${generated}"
    PRIV_INCLUDE_DIRS "${WLC_DEPS_DIR}/esp-sdr/main/common"
        "${WLC_DEPS_DIR}/esp-sdr/main/targets/esp32c5"
        "${WLC_DEPS_DIR}/esp-sdr/main/diagnostics"
    REQUIRES arduino nimble_cpp esp-dsp esp_wifi esp_phy esp_timer
        esp_driver_usb_serial_jtag esp_driver_uart nvs_flash
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WLC_FW_GIT="airhorn-dev" ARDUINO_USB_CDC_ON_BOOT=0
    CONFIG_BT_NIMBLE_ROLE_CENTRAL=0 CONFIG_BT_NIMBLE_ROLE_OBSERVER=0)
if(WLC_SDR_BOOT_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WLC_SDR_BOOT_TRACE=1)
endif()
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${WLC_DEPS_DIR}/esp-sdr/main/targets/esp32c5/sram_guard.ld"
    "-Wl,--require-defined=adctrig"
    "-T${CMAKE_CURRENT_SOURCE_DIR}/rf_buffers.ld")
