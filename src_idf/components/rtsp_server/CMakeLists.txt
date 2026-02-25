idf_component_register(
    SRCS "rtsp_server.c"
    INCLUDE_DIRS "include"
    REQUIRES camera status_led audio_capture aac_encoder_pipe
    PRIV_REQUIRES esp_timer
)
