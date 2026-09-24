cmake_minimum_required(VERSION 3.16)

# Add the parent directory as a component directory (for esphttpd)
set(EXTRA_COMPONENT_DIRS "..")

# Include the ESP-IDF project
# Production memory layout (8 MB PSRAM, PSRAM task stacks) when built inside
# esp_node_firmware; see tools/qemu/production_memory.cmake there.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/qemu/production_memory.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/../../../tools/qemu/production_memory.cmake")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project name
project(esphttpd_e2e_test)

# Set QEMU-specific configurations
idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_IDF_TARGET_ESP32S3=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DESP_PLATFORM" APPEND)