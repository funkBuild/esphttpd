# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Include the components directory (parent esphttpd)
set(EXTRA_COMPONENT_DIRS "../")

# Define test mode flag globally for all components
add_compile_definitions(CONFIG_ESPHTTPD_TEST_MODE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esphttpd_test_app)