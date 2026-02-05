idf_component_register(
    SRCS "sip_client.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_manager wifi_manager esp_netif lwip mbedtls esp_timer
)
