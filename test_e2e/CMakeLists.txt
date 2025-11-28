cmake_minimum_required(VERSION 3.16)

# Add the parent directory as a component directory (for esphttpd)
set(EXTRA_COMPONENT_DIRS "..")

# Include the ESP-IDF project
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set the project name
project(esphttpd_e2e_test)

# Set QEMU-specific configurations
idf_build_set_property(COMPILE_OPTIONS "-DCONFIG_IDF_TARGET_ESP32S3=1" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-DESP_PLATFORM" APPEND)