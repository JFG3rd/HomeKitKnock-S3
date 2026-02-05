# ESP-IDF Project CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Include project directory for headers in include/
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-s3-doorbell)
