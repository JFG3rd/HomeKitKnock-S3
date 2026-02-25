idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_manager wifi_manager web_server dns_server log_buffer sip_client lwip button status_led camera mjpeg_server rtsp_server audio_capture audio_output aac_encoder_pipe
)

