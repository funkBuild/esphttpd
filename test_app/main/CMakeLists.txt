idf_component_register(
    SRCS
        "test_app_main.c"
        "test_http_parser.c"
        "test_websocket_frame.c"
        "test_connection.c"
        "test_event_loop.c"
        "test_template.c"
        "test_integration.c"
        "test_radix_tree.c"
        "test_defer.c"
        "test_performance.c"
        "test_send_buffer.c"
        "test_filesystem.c"
    INCLUDE_DIRS "."
    REQUIRES unity esphttpd
    PRIV_REQUIRES esp_timer
)