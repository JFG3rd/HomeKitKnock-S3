# Thin wrapper around ESP-ADF esp_codec + media_lib_sal.
# Compiles only the support files we need and links the precompiled AAC encoder.

set(ADF_LIBS "${CMAKE_SOURCE_DIR}/components_phase5/esp-adf/components/esp-adf-libs")

idf_component_register(
    SRCS
        "${ADF_LIBS}/esp_codec/audio_alc.c"
        "${ADF_LIBS}/esp_codec/filter_resample.c"
        "${ADF_LIBS}/media_lib_sal/media_lib_common.c"
        "${ADF_LIBS}/media_lib_sal/media_lib_os.c"
        "${ADF_LIBS}/media_lib_sal/mem_trace/media_lib_mem_trace.c"
        "${ADF_LIBS}/media_lib_sal/mem_trace/media_lib_mem_his.c"
        "${ADF_LIBS}/media_lib_sal/port/media_lib_os_freertos.c"
        "${ADF_LIBS}/media_lib_sal/media_lib_adapter.c"
    INCLUDE_DIRS
        "${ADF_LIBS}/esp_codec/include/codec"
        "${ADF_LIBS}/esp_codec/include/processing"
        "${ADF_LIBS}/media_lib_sal/include"
        "${ADF_LIBS}/media_lib_sal/include/port"
        "${CMAKE_SOURCE_DIR}/components_phase5/esp-adf/components/audio_pipeline/include"
        "${CMAKE_SOURCE_DIR}/components_phase5/esp-adf/components/audio_stream/include"
    REQUIRES audio_sal
)

# Link precompiled ESP32-S3 AAC encoder + audio processing libraries
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-L${ADF_LIBS}/esp_codec/lib/esp32s3"
    "-Wl,--start-group" "-lesp_codec" "-lesp_processing" "-Wl,--end-group")
