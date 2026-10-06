file(GLOB nimble_sources "${WLC_DEPS_DIR}/nimble/src/*.cpp")
list(FILTER nimble_sources EXCLUDE REGEX "/NimBLEDevice\\.cpp$")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${COMPONENT_DIR}/../../prepare_sources.py"
            --deps "${WLC_DEPS_DIR}" --output "${generated}"
        RESULT_VARIABLE prepare_result)
    if(NOT prepare_result EQUAL 0)
        message(FATAL_ERROR "NimBLE SDK integration source preparation failed")
    endif()
endif()
# Use ESP-IDF's NimBLE host, not the Arduino library's bundled second host.
idf_component_register(SRCS ${nimble_sources} "${generated}/nimble_device.cpp"
    INCLUDE_DIRS "${WLC_DEPS_DIR}/nimble/src" REQUIRES bt arduino nvs_flash)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CONFIG_BT_NIMBLE_ROLE_CENTRAL=0 CONFIG_BT_NIMBLE_ROLE_OBSERVER=0)
target_compile_options(${COMPONENT_LIB} PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/sdk_host.h>")
