idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include"
    REQUIRES esp_http_server wifi_manager app_update log_buffer
)
