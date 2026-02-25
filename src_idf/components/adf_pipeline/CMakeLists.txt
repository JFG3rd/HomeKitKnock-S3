# Thin wrapper around ESP-ADF audio_pipeline + raw_stream.
# Cherry-picks only the source files we need (no i2s_stream, http_stream, etc.).

set(ADF "${CMAKE_SOURCE_DIR}/components_phase5/esp-adf/components")

idf_component_register(
    SRCS
        "${ADF}/audio_pipeline/audio_element.c"
        "${ADF}/audio_pipeline/audio_event_iface.c"
        "${ADF}/audio_pipeline/audio_pipeline.c"
        "${ADF}/audio_pipeline/ringbuf.c"
        "${ADF}/audio_stream/raw_stream.c"
    INCLUDE_DIRS
        "${ADF}/audio_pipeline/include"
        "${ADF}/audio_stream/include"
        "${ADF}/esp-adf-libs/esp_codec/include/codec"
    REQUIRES audio_sal
)
