cmake_minimum_required(VERSION 3.16)
# Experimental GPL combined image; the stable Arduino sketch is not rewritten.
set(ENV{IDF_COMPONENT_MANAGER} "0")
if(NOT WLC_DEPS_DIR)
    message(FATAL_ERROR "Pass -DWLC_DEPS_DIR=<prepared Arduino/NimBLE/ESP-SDR directory>")
endif()
get_filename_component(WLC_DEPS_DIR "${WLC_DEPS_DIR}" ABSOLUTE)
execute_process(COMMAND git -C "$ENV{IDF_PATH}" rev-parse HEAD
    OUTPUT_VARIABLE idf_commit OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE idf_git_result)
if(NOT idf_git_result EQUAL 0 OR NOT idf_commit STREQUAL "b774170ff46c393eeb5e495ea37936038d3f4f4f")
    message(FATAL_ERROR "Use the pinned ESP-IDF v5.5.5 commit; Arduino 3.3.11 cannot use upstream's 6.2 SDK")
endif()
# -DWLC_BOARD=2_8 (default) or 1_69: LCD wiring and flash size of the Waveshare touch board.
set(WLC_BOARD "2_8" CACHE STRING "Waveshare ESP32-C5 touch board: 2_8 or 1_69")
if(NOT WLC_BOARD MATCHES "^(2_8|1_69)$")
    message(FATAL_ERROR "WLC_BOARD must be 2_8 or 1_69")
endif()
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${WLC_BOARD}")
set(EXTRA_COMPONENT_DIRS "${WLC_DEPS_DIR}/arduino" "${WLC_DEPS_DIR}/esp-sdr/components/esp-dsp")
# Release identity the app's firmware update check reads (product=/version= in the BLE status
# line). A build host gets the tree without .git, so the publisher passes -DWLC_SOURCE_REVISION.
string(TIMESTAMP WLC_BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP WLC_BUILD_VERSION "%Y.%m.%d" UTC)
if(NOT WLC_SOURCE_REVISION)
    execute_process(COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --short=7 HEAD
        OUTPUT_VARIABLE WLC_SOURCE_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT WLC_SOURCE_REVISION)
    set(WLC_SOURCE_REVISION "source-archive")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "WLC_BOARD_${WLC_BOARD}=1" APPEND)
set(PROJECT_VER "touch-airhorn-v4")
project(wlc_sensor)
