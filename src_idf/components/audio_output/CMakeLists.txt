idf_component_register(
    SRCS "audio_output.c" "gong_data.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES esp_driver_i2s nvs_manager audio_capture i2s_shared_bus
)
