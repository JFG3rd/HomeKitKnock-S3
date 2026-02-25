idf_component_register(
    SRCS "aac_encoder_pipe.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES audio_capture adf_pipeline adf_codec nvs_manager
)
