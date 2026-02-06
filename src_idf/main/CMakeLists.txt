idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_manager wifi_manager web_server dns_server log_buffer sip_client lwip button status_led
)

