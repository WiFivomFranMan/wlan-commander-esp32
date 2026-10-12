# SPDX-License-Identifier: MIT
if(COMMAND idf_component_register)
  # Pure C++17, no IDF headers, so no REQUIRES.
  idf_component_register(SRC_DIRS "src" INCLUDE_DIRS "include")
  target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
else()
  cmake_minimum_required(VERSION 3.16)
  project(wlc_survey CXX)
  file(GLOB WLC_SURVEY_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
  add_library(wlc_survey STATIC ${WLC_SURVEY_SRCS})
  target_include_directories(wlc_survey PUBLIC include)
  target_compile_features(wlc_survey PUBLIC cxx_std_17)
  target_compile_options(wlc_survey PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
endif()
