# esphttpd hot-path benchmark. ESP32-S3 image run under QEMU with -icount, so the
# figures are deterministic guest instruction counts of the real component at the
# production optimisation level (-O2) - host load does not move them.
# Build + run: bench/run.sh [filter]   (see main/bench.c)
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/..")
set(COMPONENTS main)
add_compile_definitions(CONFIG_ESPHTTPD_TEST_MODE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esphttpd_bench)
