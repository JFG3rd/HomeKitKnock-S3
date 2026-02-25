idf_component_register(
    SRCS "sip_client.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES nvs_manager wifi_manager esp_netif lwip mbedtls esp_timer audio_capture audio_output
)
