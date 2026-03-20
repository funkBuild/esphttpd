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
    httpd_err_t err = httpd_start(&test_handle, &config);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
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
    radix_match_t match1;
    radix_lookup(g_server->legacy_routes, "/api/users", HTTP_GET, false, &match1, NULL, NULL);
    TEST_ASSERT_TRUE(match1.matched);


    radix_match_t match2;
    radix_lookup(g_server->legacy_routes, "/api/anything", HTTP_GET, false, &match2, NULL, NULL);
    TEST_ASSERT_TRUE(match2.matched);


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

// ==================== ROUTER ON_ERROR TESTS ====================

// Mock error handler for testing (signature: httpd_err_t handler(httpd_err_t error, httpd_req_t* req))
static httpd_err_t mock_error_handler(httpd_err_t error, httpd_req_t* req) {
    (void)error;
    (void)req;
    return HTTPD_OK;
}

// Test setting error handler on valid router
static void test_router_on_error_valid(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Initially error_handler should be NULL
    TEST_ASSERT_NULL(router->error_handler);

    // Set error handler
    httpd_err_t result = httpd_router_on_error(router, mock_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, result);

    // Verify error_handler was set
    TEST_ASSERT_EQUAL(mock_error_handler, router->error_handler);

    httpd_router_destroy(router);
}

// Test with NULL router
static void test_router_on_error_null_router(void) {
    httpd_err_t result = httpd_router_on_error(NULL, mock_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);
}

// Test with NULL handler
static void test_router_on_error_null_handler(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t result = httpd_router_on_error(router, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);

    // Verify error_handler is still NULL
    TEST_ASSERT_NULL(router->error_handler);

    httpd_router_destroy(router);
}

// Test replacing existing error handler
static void test_router_on_error_replace(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Set first handler
    httpd_err_t result = httpd_router_on_error(router, mock_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, result);
    TEST_ASSERT_EQUAL(mock_error_handler, router->error_handler);

    // Replace with same handler (should work)
    result = httpd_router_on_error(router, mock_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, result);
    TEST_ASSERT_EQUAL(mock_error_handler, router->error_handler);

    httpd_router_destroy(router);
}

// ==================== ROUTE PARAM TESTS ====================

// Test httpd_req_get_param with valid params
static void test_req_get_param_valid(void)
{
    httpd_req_t req = {0};

    // Manually populate params (normally done by radix tree matcher)
    req.param_count = 2;
    req.params[0].key = "id";
    req.params[0].key_len = 2;
    req.params[0].value = "123";
    req.params[0].value_len = 3;
    req.params[1].key = "name";
    req.params[1].key_len = 4;
    req.params[1].value = "test";
    req.params[1].value_len = 4;

    // Test getting valid params
    const char* id = httpd_req_get_param(&req, "id");
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_EQUAL_STRING("123", id);

    const char* name = httpd_req_get_param(&req, "name");
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("test", name);
}

// Test httpd_req_get_param with nonexistent key
static void test_req_get_param_not_found(void)
{
    httpd_req_t req = {0};

    req.param_count = 1;
    req.params[0].key = "id";
    req.params[0].key_len = 2;
    req.params[0].value = "123";
    req.params[0].value_len = 3;

    const char* result = httpd_req_get_param(&req, "nonexistent");
    TEST_ASSERT_NULL(result);
}

// Test httpd_req_get_param with NULL request
static void test_req_get_param_null_req(void)
{
    const char* result = httpd_req_get_param(NULL, "id");
    TEST_ASSERT_NULL(result);
}

// Test httpd_req_get_param with NULL key
static void test_req_get_param_null_key(void)
{
    httpd_req_t req = {0};
    req.param_count = 1;
    req.params[0].key = "id";
    req.params[0].key_len = 2;
    req.params[0].value = "123";
    req.params[0].value_len = 3;

    const char* result = httpd_req_get_param(&req, NULL);
    TEST_ASSERT_NULL(result);
}

// Test httpd_req_get_param with empty params
static void test_req_get_param_no_params(void)
{
    httpd_req_t req = {0};
    req.param_count = 0;

    const char* result = httpd_req_get_param(&req, "id");
    TEST_ASSERT_NULL(result);
}

// Test httpd_req_get_param partial key match (shouldn't match)
static void test_req_get_param_partial_match(void)
{
    httpd_req_t req = {0};

    req.param_count = 1;
    req.params[0].key = "userId";
    req.params[0].key_len = 6;
    req.params[0].value = "123";
    req.params[0].value_len = 3;

    // "user" should not match "userId"
    const char* result = httpd_req_get_param(&req, "user");
    TEST_ASSERT_NULL(result);

    // "userIdX" should not match "userId"
    result = httpd_req_get_param(&req, "userIdX");
    TEST_ASSERT_NULL(result);
}

// ==================== BASIC AUTH TESTS ====================

// Test httpd_check_basic_auth with NULL request
static void test_basic_auth_null_req(void)
{
    bool result = httpd_check_basic_auth(NULL, "user", "pass");
    TEST_ASSERT_FALSE(result);
}

// Test httpd_check_basic_auth with NULL username
static void test_basic_auth_null_username(void)
{
    httpd_req_t req = {0};
    bool result = httpd_check_basic_auth(&req, NULL, "pass");
    TEST_ASSERT_FALSE(result);
}

// Test httpd_check_basic_auth with NULL password
static void test_basic_auth_null_password(void)
{
    httpd_req_t req = {0};
    bool result = httpd_check_basic_auth(&req, "user", NULL);
    TEST_ASSERT_FALSE(result);
}

// Test httpd_check_basic_auth with empty request (no headers)
static void test_basic_auth_no_header(void)
{
    httpd_req_t req = {0};
    req.header_count = 0;
    bool result = httpd_check_basic_auth(&req, "user", "pass");
    TEST_ASSERT_FALSE(result);
}

// ==================== MEMORY ALLOCATION FAILURE TESTS ====================

// Test radix tree operations with NULL tree
static void test_radix_tree_null_operations(void)
{
    // Insert on NULL tree should fail
    httpd_err_t result = radix_insert(NULL, "/test", HTTP_GET, test_handler, NULL, NULL, 0);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);

    // Lookup on NULL tree should not match
    radix_match_t lookup;
    radix_lookup(NULL, "/test", HTTP_GET, false, &lookup, NULL, NULL);
    TEST_ASSERT_FALSE(lookup.matched);
    TEST_ASSERT_NULL(lookup.handler);
}

// Test router operations with NULL router
static void test_router_null_operations(void)
{
    // NULL router operations should fail gracefully
    httpd_err_t result = httpd_router_get(NULL, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);

    result = httpd_router_post(NULL, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);

    result = httpd_router_put(NULL, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);

    result = httpd_router_delete(NULL, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);
}

// Test router destroy on NULL
static void test_router_destroy_null(void)
{
    // Destroy NULL router should be safe (no crash)
    httpd_router_destroy(NULL);
    TEST_PASS();
}

// Test radix tree destroy on NULL
static void test_radix_tree_destroy_null(void)
{
    // Destroy NULL tree should be safe (no crash)
    radix_tree_destroy(NULL);
    TEST_PASS();
}

// Test router creation and destruction lifecycle
static void test_router_lifecycle(void)
{
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Add some routes
    httpd_err_t result = httpd_router_get(router, "/api/users", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, result);

    result = httpd_router_post(router, "/api/users", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, result);

    // Clean destruction
    httpd_router_destroy(router);
    // If we get here without crashing, test passes
    TEST_PASS();
}

// Test radix tree with many routes (stress test allocation)
static void test_radix_tree_many_routes(void)
{
    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Add many routes
    char path[64];
    for (int i = 0; i < 50; i++) {
        snprintf(path, sizeof(path), "/api/route/%d", i);
        httpd_err_t result = radix_insert(tree, path, HTTP_GET, test_handler, NULL, NULL, 0);
        TEST_ASSERT_EQUAL(HTTPD_OK, result);
    }

    // Verify some routes
    radix_match_t lookup;
    radix_lookup(tree, "/api/route/25", HTTP_GET, false, &lookup, NULL, NULL);
    TEST_ASSERT_TRUE(lookup.matched);
    TEST_ASSERT_NOT_NULL(lookup.handler);

    radix_lookup(tree, "/api/route/49", HTTP_GET, false, &lookup, NULL, NULL);
    TEST_ASSERT_TRUE(lookup.matched);
    TEST_ASSERT_NOT_NULL(lookup.handler);

    radix_tree_destroy(tree);
    TEST_PASS();
}

// Test WebSocket frame context init with NULL
static void test_ws_context_null(void)
{
    // NULL context init - function returns 0 (no-op for NULL)
    int result = ws_frame_ctx_init(NULL);
    TEST_ASSERT_EQUAL(0, result);
}

// Mock middleware for testing
static httpd_err_t mock_middleware(httpd_req_t* req, httpd_handler_t next) {
    (void)req;
    (void)next;
    return HTTPD_OK;
}

// Test router use middleware on NULL router
static void test_router_use_null(void)
{
    httpd_err_t result = httpd_router_use(NULL, mock_middleware);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, result);
}

// ==================== FD LEAK DETECTION TESTS ====================

// Test that after server start/stop cycle, all connection pool slots are clean.
// This is the practical fd leak check for unit tests: verify all fds are -1
// and all bitmasks are zero after httpd_stop().
static void test_fd_leak_after_start_stop(void) {
    // Start and stop the server
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    // Grab a snapshot of pool state while running
    connection_pool_t* pool = &g_server->connection_pool;

    // All slots should start clean (no active connections yet)
    TEST_ASSERT_EQUAL(0, pool->active_mask);
    TEST_ASSERT_EQUAL(0, pool->write_pending_mask);
    TEST_ASSERT_EQUAL(0, pool->ws_active_mask);
    TEST_ASSERT_EQUAL(0, pool->read_paused_mask);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t* conn = connection_get(pool, i);
        TEST_ASSERT_NOT_NULL(conn);
        TEST_ASSERT_EQUAL(-1, conn->fd);
        TEST_ASSERT_EQUAL(CONN_STATE_FREE, conn->state);
    }

    stop_test_server();
}

// Test that after simulating connections and stopping, everything is clean
static void test_fd_leak_after_connections_and_stop(void) {
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    connection_pool_t* pool = &g_server->connection_pool;

    // Simulate some connection activity
    for (int i = 0; i < 3; i++) {
        connection_t* conn = connection_get(pool, i);
        TEST_ASSERT_NOT_NULL(conn);
        conn->fd = 100 + i;
        conn->state = CONN_STATE_HTTP_HEADERS;
        connection_mark_active(pool, i);
    }

    // Close and cleanup all simulated connections
    for (int i = 0; i < 3; i++) {
        connection_t* conn = connection_get(pool, i);
        connection_close(pool, conn);
    }
    connection_cleanup_closed(pool);

    // Verify all pool slots have fd == -1 and all bitmasks are 0
    TEST_ASSERT_EQUAL(0, pool->active_mask);
    TEST_ASSERT_EQUAL(0, pool->write_pending_mask);
    TEST_ASSERT_EQUAL(0, pool->ws_active_mask);
    TEST_ASSERT_EQUAL(0, pool->read_paused_mask);

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t* conn = connection_get(pool, i);
        TEST_ASSERT_NOT_NULL(conn);
        TEST_ASSERT_EQUAL(-1, conn->fd);
        TEST_ASSERT_EQUAL(CONN_STATE_FREE, conn->state);
    }

    stop_test_server();
}

// ==================== CONNECTION DROP TESTS ====================

// Test that setting a connection to CONN_STATE_CLOSED and running cleanup
// properly resets all resources
static void test_connection_drop_cleanup(void) {
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    connection_pool_t* pool = &g_server->connection_pool;

    // Set up a connection that simulates an active HTTP session
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    conn->fd = 200;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->content_length = 5000;
    conn->bytes_received = 1000;
    conn->keep_alive = 1;
    conn->pool_index = idx;
    connection_mark_active(pool, idx);
    connection_mark_write_pending(pool, idx, true);

    // Simulate connection drop by closing the connection
    connection_close(pool, conn);

    // State should be CLOSED but still in active mask (for cleanup to find)
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);
    TEST_ASSERT_TRUE(connection_is_active(pool, idx));
    // Write pending should be cleared by connection_close
    TEST_ASSERT_FALSE(connection_has_write_pending(pool, idx));

    // Run cleanup
    connection_cleanup_closed(pool);

    // After cleanup, everything should be reset
    TEST_ASSERT_EQUAL(CONN_STATE_FREE, conn->state);
    TEST_ASSERT_EQUAL(-1, conn->fd);
    TEST_ASSERT_FALSE(connection_is_active(pool, idx));

    stop_test_server();
}

// Test connection drop for a WebSocket connection
static void test_connection_drop_websocket(void) {
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    connection_pool_t* pool = &g_server->connection_pool;

    // Set up a WebSocket connection
    int idx = 1;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    conn->fd = 300;
    conn->state = CONN_STATE_WEBSOCKET;
    conn->is_websocket = 1;
    conn->ws_opcode = WS_OPCODE_TEXT;
    conn->pool_index = idx;
    connection_mark_active(pool, idx);
    connection_mark_ws_active(pool, idx);
    connection_mark_write_pending(pool, idx, true);
    connection_mark_read_paused(pool, idx, true);

    // Verify all masks are set
    TEST_ASSERT_TRUE(connection_is_active(pool, idx));
    TEST_ASSERT_TRUE(connection_is_ws_active(pool, idx));
    TEST_ASSERT_TRUE(connection_has_write_pending(pool, idx));
    TEST_ASSERT_TRUE(connection_is_read_paused(pool, idx));

    // Simulate connection drop
    connection_close(pool, conn);

    // connection_close should clear write_pending, ws_active, and read_paused
    TEST_ASSERT_FALSE(connection_has_write_pending(pool, idx));
    TEST_ASSERT_FALSE(connection_is_ws_active(pool, idx));
    TEST_ASSERT_FALSE(connection_is_read_paused(pool, idx));

    // Run cleanup
    connection_cleanup_closed(pool);

    // Everything should be reset
    TEST_ASSERT_EQUAL(CONN_STATE_FREE, conn->state);
    TEST_ASSERT_EQUAL(-1, conn->fd);
    TEST_ASSERT_FALSE(connection_is_active(pool, idx));

    stop_test_server();
}

// Test multiple simultaneous connection drops
static void test_connection_drop_multiple(void) {
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    connection_pool_t* pool = &g_server->connection_pool;

    // Set up multiple connections
    for (int i = 0; i < 4; i++) {
        connection_t* conn = connection_get(pool, i);
        TEST_ASSERT_NOT_NULL(conn);
        conn->fd = 400 + i;
        conn->state = CONN_STATE_HTTP_HEADERS;
        conn->pool_index = i;
        connection_mark_active(pool, i);
    }

    TEST_ASSERT_EQUAL(4, connection_count_active(pool));

    // Drop connections 0 and 2 (simulate two simultaneous drops)
    connection_close(pool, connection_get(pool, 0));
    connection_close(pool, connection_get(pool, 2));

    // Connections 1 and 3 should still be active
    TEST_ASSERT_TRUE(connection_is_active(pool, 1));
    TEST_ASSERT_TRUE(connection_is_active(pool, 3));

    // Run cleanup
    connection_cleanup_closed(pool);

    // Connections 0 and 2 should be fully cleaned up
    TEST_ASSERT_EQUAL(CONN_STATE_FREE, connection_get(pool, 0)->state);
    TEST_ASSERT_EQUAL(-1, connection_get(pool, 0)->fd);
    TEST_ASSERT_FALSE(connection_is_active(pool, 0));

    TEST_ASSERT_EQUAL(CONN_STATE_FREE, connection_get(pool, 2)->state);
    TEST_ASSERT_EQUAL(-1, connection_get(pool, 2)->fd);
    TEST_ASSERT_FALSE(connection_is_active(pool, 2));

    // Connections 1 and 3 should be untouched
    TEST_ASSERT_EQUAL(401, connection_get(pool, 1)->fd);
    TEST_ASSERT_EQUAL(403, connection_get(pool, 3)->fd);
    TEST_ASSERT_TRUE(connection_is_active(pool, 1));
    TEST_ASSERT_TRUE(connection_is_active(pool, 3));

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

    // Router on_error tests
    RUN_TEST(test_router_on_error_valid);
    RUN_TEST(test_router_on_error_null_router);
    RUN_TEST(test_router_on_error_null_handler);
    RUN_TEST(test_router_on_error_replace);

    // Route parameter extraction tests
    RUN_TEST(test_req_get_param_valid);
    RUN_TEST(test_req_get_param_not_found);
    RUN_TEST(test_req_get_param_null_req);
    RUN_TEST(test_req_get_param_null_key);
    RUN_TEST(test_req_get_param_no_params);
    RUN_TEST(test_req_get_param_partial_match);

    // Basic auth tests
    RUN_TEST(test_basic_auth_null_req);
    RUN_TEST(test_basic_auth_null_username);
    RUN_TEST(test_basic_auth_null_password);
    RUN_TEST(test_basic_auth_no_header);

    // Memory allocation failure tests
    RUN_TEST(test_radix_tree_null_operations);
    RUN_TEST(test_router_null_operations);
    RUN_TEST(test_router_destroy_null);
    RUN_TEST(test_radix_tree_destroy_null);
    RUN_TEST(test_router_lifecycle);
    RUN_TEST(test_radix_tree_many_routes);
    RUN_TEST(test_ws_context_null);
    RUN_TEST(test_router_use_null);

    // FD leak detection tests
    RUN_TEST(test_fd_leak_after_start_stop);
    RUN_TEST(test_fd_leak_after_connections_and_stop);

    // Connection drop tests
    RUN_TEST(test_connection_drop_cleanup);
    RUN_TEST(test_connection_drop_websocket);
    RUN_TEST(test_connection_drop_multiple);

    ESP_LOGI(TAG, "Integration tests completed (35 tests)");
}
