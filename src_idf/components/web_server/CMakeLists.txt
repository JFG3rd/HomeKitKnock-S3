idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES esp_http_server wifi_manager app_update log_buffer sip_client nvs_manager esp_netif camera mjpeg_server
)
