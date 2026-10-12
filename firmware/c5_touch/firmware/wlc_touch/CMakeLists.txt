# SPDX-License-Identifier: MIT
# WLC Touch: ESP-Brookesia launcher with WLAN Commander's survey tools (ota_0).
# -DWLC_BOARD=2_8 (default) or 1_69 selects the Waveshare board: its BSP (component wlc_bsp),
# flash size and screen height. Each board keeps its own sdkconfig and component lock.
cmake_minimum_required(VERSION 3.16)
set(WLC_BOARD "2_8" CACHE STRING "Waveshare ESP32-C5 touch board: 2_8 or 1_69")
if(NOT WLC_BOARD MATCHES "^(2_8|1_69)$")
    message(FATAL_ERROR "WLC_BOARD must be 2_8 or 1_69")
endif()
string(REPLACE "_" "." board_dir "${WLC_BOARD}")
set(WLC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(EXTRA_COMPONENT_DIRS
    "${WLC_ROOT}/third_party/waveshare-2.8/components"      # brookesia_core and shared drivers
    "${WLC_ROOT}/third_party/waveshare-${board_dir}/bsp"     # this board's wlc_bsp
    "${WLC_ROOT}/components")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${WLC_BOARD}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${WLC_BOARD}")
idf_build_set_property(COMPILE_DEFINITIONS "WLC_BOARD_${WLC_BOARD}=1" APPEND)
project(wlc_touch)
