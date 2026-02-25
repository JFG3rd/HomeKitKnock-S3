idf_component_register(
    SRCS "i2s_shared_bus.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
    REQUIRES esp_driver_i2s
)
