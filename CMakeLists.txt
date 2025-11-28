# esphttpd - High-performance HTTP server for ESP32
message(STATUS "Building esphttpd component")

# Check if esp_littlefs is available for filesystem support
idf_build_get_property(components BUILD_COMPONENTS)
if("esp_littlefs" IN_LIST components)
    set(FILESYSTEM_SOURCES "src/filesystem.c")
    set(FILESYSTEM_REQUIRES "esp_littlefs")
    message(STATUS "LittleFS support enabled")
else()
    set(FILESYSTEM_SOURCES "")
    set(FILESYSTEM_REQUIRES "")
    message(STATUS "LittleFS support disabled (component not found)")
endif()

idf_component_register(
    SRCS
        "src/esphttpd.c"
        "src/connection.c"
        "src/event_loop.c"
        "src/http_parser.c"
        "src/websocket.c"
        "src/radix_tree.c"
        "src/router_api.c"
        "src/template.c"
        ${FILESYSTEM_SOURCES}
    INCLUDE_DIRS
        "include"           # Public API
        "src/private"       # Internal headers (for tests only - do not use in application code)
    REQUIRES
        mbedtls
        lwip
        freertos
        ${FILESYSTEM_REQUIRES}
        unity  # For tests
)