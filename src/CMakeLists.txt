# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# Ensure Arduino's app_main is linked when using Arduino as an ESP-IDF component.
# Add ESP-ADF dependencies for AAC encoding.
idf_component_register(SRCS ${app_sources} 
    REQUIRES framework-arduinoespressif32 audio_pipeline audio_sal esp-adf-libs adf_raw_stream)
