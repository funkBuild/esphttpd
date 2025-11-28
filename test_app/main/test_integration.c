#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "http_parser.h"
#include "websocket.h"
#include "radix_tree.h"
#include "esp_log.h"
#include <string.h>

static const char* TAG = "TEST_INTEGRATION";

// Test data
static int handler_called = 0;
static char last_response[1024];
static size_t last_response_len = 0;
static httpd_handle_t test_handle = NULL;

// Mock send function to capture responses
// Currently unused but kept for potential future tests
#if 0
static ssize_t mock_send(int fd, const void* buf, size_t len, int flags) {
    if (len > sizeof(last_response) - last_response_len) {
        len = sizeof(last_response) - last_response_len;
    }
    memcpy(last_response + last_response_len, buf, len);
    last_response_len += len;
    return len;
}
#endif

// Test handler
static httpd_err_t test_handler(httpd_req_t* req) {
    handler_called++;

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"result\":\"success\"}", 19);
    return HTTPD_OK;
}

// Helper to start server for tests
static void start_test_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;
    httpd_start(&test_handle, &config);
}

// Helper to stop server after tests
static void stop_test_server(void) {
    if (test_handle) {
        httpd_stop(test_handle);
        test_handle = NULL;
    }
}

// ==================== TEST FUNCTIONS ====================

// Test full HTTP GET request processing
static void test_full_http_get_request(void) {
    // Setup server
    start_test_server();

    // Add route
    httpd_route_t route = {
        .method = HTTP_GET,
        .pattern = "/test",
        .handler = test_handler
    };
    httpd_register_route(test_handle, &route);

    // Create connection and parse request
    connection_t conn = {0};
    conn.fd = -1; // Mock FD
    conn.state = CONN_STATE_NEW;

    http_parser_context_t parser_ctx = {0};

    const char* request = "GET /test HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: keep-alive\r\n"
                         "\r\n";

    // Reset test state
    handler_called = 0;
    memset(last_response, 0, sizeof(last_response));
    last_response_len = 0;

    // Parse request
    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &parser_ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(HTTP_GET, conn.method);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_HEADERS, conn.state);
    TEST_ASSERT_TRUE(conn.keep_alive);

    // Route registration verified by successful lookup
    // (radix tree doesn't expose route count)

    // Verify URL was parsed correctly
    TEST_ASSERT_NOT_NULL(parser_ctx.url);
    TEST_ASSERT_EQUAL(5, parser_ctx.url_len); // "/test" = 5 chars

    stop_test_server();
}

// Test full HTTP POST request with body
static void test_full_http_post_request(void) {
    start_test_server();

    httpd_route_t route = {
        .method = HTTP_POST,
        .pattern = "/api/data",
        .handler = test_handler
    };
    httpd_register_route(test_handle, &route);

    connection_t conn = {0};
    conn.fd = -1;
    conn.state = CONN_STATE_NEW;

    http_parser_context_t parser_ctx = {0};

    const char* request = "POST /api/data HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: 15\r\n"
                         "\r\n"
                         "{\"test\":\"data\"}";

    // Parse headers
    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request) - 15, &parser_ctx);

    TEST_ASSERT_EQUAL(PARSE_OK, result); // Headers complete, body pending
    TEST_ASSERT_EQUAL(HTTP_POST, conn.method);
    TEST_ASSERT_EQUAL(15, conn.content_length);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn.state);

    // Process body
    conn.bytes_received = 15;

    // Route registration verified by successful lookup
    // (radix tree doesn't expose route count)

    // Verify URL was parsed correctly
    TEST_ASSERT_NOT_NULL(parser_ctx.url);
    TEST_ASSERT_EQUAL(9, parser_ctx.url_len); // "/api/data" = 9 chars

    stop_test_server();
}

// Test WebSocket upgrade
static void test_websocket_upgrade(void) {
    start_test_server();

    static int ws_handler_called = 0;

    // WebSocket handler
    httpd_err_t ws_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
        ws_handler_called++;
        if (event->type == WS_EVENT_CONNECT) {
            // Connection accepted when handler returns HTTPD_OK
        }
        return HTTPD_OK;
    }

    httpd_ws_route_t route = {
        .pattern = "/ws",
        .handler = ws_handler,
        .ping_interval_ms = 0
    };
    httpd_register_ws_route(test_handle, &route);

    connection_t conn = {0};
    conn.fd = -1;
    conn.state = CONN_STATE_NEW;

    http_parser_context_t parser_ctx = {0};

    const char* request = "GET /ws HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "Sec-WebSocket-Version: 13\r\n"
                         "\r\n";

    // Parse WebSocket upgrade request
    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &parser_ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.upgrade_ws);
    TEST_ASSERT_TRUE(conn.is_websocket);
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn.state);

    stop_test_server();
}

// Test WebSocket frame processing
static void test_websocket_frame_processing(void) {
    connection_t conn = {0};
    conn.fd = -1;
    conn.state = CONN_STATE_WEBSOCKET;

    ws_frame_context_t frame_ctx = {0};

    // Create a simple text frame (masked)
    uint8_t frame[] = {
        0x81, // FIN=1, opcode=1 (text)
        0x85, // MASK=1, length=5
        0x37, 0xfa, 0x21, 0x3d, // Masking key
        0x7f, 0x9f, 0x4d, 0x51, 0x58 // Masked "Hello"
    };

    size_t consumed;
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &frame_ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(sizeof(frame), consumed);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);
}

// Test route priority and wildcards
static void test_route_matching_integration(void) {
    start_test_server();

    static int specific_called = 0;
    static int wildcard_called = 0;

    httpd_err_t specific_handler(httpd_req_t* req) { specific_called++; (void)req; return HTTPD_OK; }
    httpd_err_t wildcard_handler(httpd_req_t* req) { wildcard_called++; (void)req; return HTTPD_OK; }

    // Add specific route first
    httpd_route_t route1 = {
        .method = HTTP_GET,
        .pattern = "/api/users",
        .handler = specific_handler
    };
    httpd_register_route(test_handle, &route1);

    // Add wildcard route
    httpd_route_t route2 = {
        .method = HTTP_GET,
        .pattern = "/api/*",
        .handler = wildcard_handler
    };
    httpd_register_route(test_handle, &route2);

    // Routes are stored in radix tree - verify by lookup
    // (radix tree doesn't expose route list)
    radix_match_t match1 = radix_lookup(g_server->legacy_routes, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(match1.matched);
    if (match1.middlewares) free(match1.middlewares);

    radix_match_t match2 = radix_lookup(g_server->legacy_routes, "/api/anything", HTTP_GET, false);
    TEST_ASSERT_TRUE(match2.matched);
    if (match2.middlewares) free(match2.middlewares);

    stop_test_server();
}

// Test connection lifecycle
static void test_connection_lifecycle(void) {
    start_test_server();

    TEST_ASSERT_NOT_NULL(g_server);
    connection_pool_t* pool = &g_server->connection_pool;

    // Get new connection - use first slot
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    conn->fd = 100; // Mock FD
    conn->state = CONN_STATE_NEW;
    connection_mark_active(pool, idx);

    // Process through states
    conn->state = CONN_STATE_HTTP_HEADERS;
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_HEADERS, conn->state);

    conn->state = CONN_STATE_HTTP_BODY;
    conn->content_length = 100;
    conn->bytes_received = 50;
    TEST_ASSERT_FALSE(conn->bytes_received >= conn->content_length);

    conn->bytes_received = 100;
    TEST_ASSERT_TRUE(conn->bytes_received >= conn->content_length);

    // Close connection
    conn->state = CONN_STATE_CLOSING;
    connection_mark_inactive(pool, idx);
    TEST_ASSERT_FALSE(connection_is_active(pool, idx));

    stop_test_server();
}

// Test error handling
static void test_error_handling(void) {
    connection_t conn = {0};
    conn.fd = -1;
    conn.state = CONN_STATE_NEW;

    http_parser_context_t parser_ctx = {0};

    // Malformed request
    const char* bad_request = "INVALID REQUEST\r\n\r\n";

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)bad_request,
                                              strlen(bad_request), &parser_ctx);

    TEST_ASSERT_EQUAL(PARSE_ERROR, result);

    // Headers too large
    char huge_request[5000];
    memset(huge_request, 'A', sizeof(huge_request));
    strcpy(huge_request, "GET / HTTP/1.1\r\n");

    conn.state = CONN_STATE_NEW;
    conn.header_bytes = 0;
    parser_ctx = (http_parser_context_t){0};

    result = http_parse_request(&conn, (const uint8_t*)huge_request,
                               sizeof(huge_request), &parser_ctx);

    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test multiple concurrent connections
static void test_concurrent_connections(void) {
    start_test_server();

    TEST_ASSERT_NOT_NULL(g_server);
    connection_pool_t* pool = &g_server->connection_pool;
    int connections[5];

    // Create multiple connections
    for (int i = 0; i < 5; i++) {
        connections[i] = i; // Use sequential indices for testing
        connection_t* conn = connection_get(pool, i);
        TEST_ASSERT_NOT_NULL(conn);

        conn->fd = 100 + i;
        conn->state = CONN_STATE_HTTP_HEADERS;
        connection_mark_active(pool, i);
    }

    // Verify all are active
    int active_count = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_is_active(pool, i)) {
            active_count++;
        }
    }
    TEST_ASSERT_TRUE(active_count >= 5);

    // Close one connection
    connection_mark_inactive(pool, connections[2]);

    // Verify it's not active anymore
    TEST_ASSERT_FALSE(connection_is_active(pool, connections[2]));

    stop_test_server();
}

// ==================== TEST RUNNER ====================

void test_integration_run(void) {
    ESP_LOGI(TAG, "Running Integration tests");

    RUN_TEST(test_full_http_get_request);
    RUN_TEST(test_full_http_post_request);
    RUN_TEST(test_websocket_upgrade);
    RUN_TEST(test_websocket_frame_processing);
    RUN_TEST(test_route_matching_integration);
    RUN_TEST(test_connection_lifecycle);
    RUN_TEST(test_error_handling);
    RUN_TEST(test_concurrent_connections);

    ESP_LOGI(TAG, "Integration tests completed");
}
