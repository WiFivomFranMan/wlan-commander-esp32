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
set(EXTRA_COMPONENT_DIRS "${WLC_DEPS_DIR}/arduino" "${WLC_DEPS_DIR}/esp-sdr/components/esp-dsp")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wlc_sniffer_sdr)
