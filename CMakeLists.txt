cmake_minimum_required(VERSION 3.16.0)

# Skip Xtensa cross-compiler ABI detection (avoids host/target mismatch)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Disable component manager to prevent esp-adf component.yml deps from auto-resolving.
set(ENV{IDF_COMPONENT_MANAGER} 0)
set(IDF_COMPONENT_MANAGER 0 CACHE BOOL "Disable ESP-IDF component manager" FORCE)

# Phase 1 components (nvs_manager, wifi_manager, web_server)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src_idf/components")

# esp32-camera library (from PlatformIO lib_deps)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/.pio/libdeps/seeed_xiao_esp32s3_idf/esp32-camera")

# ESP-ADF: only audio_sal imported directly (uses old register_component(), backward-compatible).
# audio_pipeline and esp_codec are wrapped in our own thin components under src_idf/components/.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/components_phase5/esp-adf/components/audio_sal"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-s3-doorbell)
