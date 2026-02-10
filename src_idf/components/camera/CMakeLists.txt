idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES esp32-camera driver nvs_manager
)
