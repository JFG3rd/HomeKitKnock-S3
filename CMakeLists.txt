cmake_minimum_required(VERSION 3.16.0)

# Skip compiler ABI detection to avoid macOS cross-compilation issues
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Disable component manager so ESP-IDF 4.4 doesn't try to resolve esp-adf component.yml deps.
set(ENV{IDF_COMPONENT_MANAGER} 0)
set(IDF_COMPONENT_MANAGER 0 CACHE BOOL "Disable ESP-IDF component manager" FORCE)

# Phase 1 components (nvs_manager, wifi_manager, web_server)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src_idf/components")

# ESP-ADF components (commented out for Phase 1, will add back in Phase 5)
# Append minimal ADF components without overriding PlatformIO's EXTRA_COMPONENT_DIRS
# (which includes src/ and Arduino framework).
#list(APPEND EXTRA_COMPONENT_DIRS
#    "${CMAKE_SOURCE_DIR}/components/esp-adf/components/audio_pipeline"
#    "${CMAKE_SOURCE_DIR}/components/esp-adf/components/audio_sal"
#    "${CMAKE_SOURCE_DIR}/components/esp-adf/components/esp-adf-libs"
#    "${CMAKE_SOURCE_DIR}/components/adf_raw_stream"
#)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-s3-doorbell)
