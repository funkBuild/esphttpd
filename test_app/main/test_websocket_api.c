/**
 * @file test_websocket_api.c
 * @brief Unit tests for WebSocket high-level API
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "websocket.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_WEBSOCKET_API";

// Test server handle
static httpd_handle_t test_server = NULL;

// Helper to start test server
static void start_test_server(void) {
    if (test_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;
    httpd_start(&test_server, &config);
}

// Helper to stop test server
static void stop_test_server(void) {
    if (test_server) {
        httpd_stop(test_server);
        test_server = NULL;
    }
}

// WebSocket test handler
static httpd_err_t ws_test_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
    (void)ws;
    (void)event;
    return HTTPD_OK;
}

// Helper to setup mock WebSocket context
static void setup_mock_ws(httpd_ws_t* ws, connection_t* conn) {
    memset(ws, 0, sizeof(*ws));
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1; // Invalid FD for testing
    conn->state = CONN_STATE_WEBSOCKET;
    conn->is_websocket = true;
    ws->fd = conn->fd;
    ws->connected = true;
    ws->_internal = conn;
}

// ==================== WebSocket Send Tests ====================

static void test_ws_send_null_ws(void) {
    // Returns HTTPD_ERR_CONN_CLOSED when ws is NULL or not connected
    httpd_err_t err = httpd_ws_send(NULL, "test", 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(HTTPD_ERR_CONN_CLOSED, err);
}

static void test_ws_send_null_data(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    // Returns HTTPD_ERR_IO when the actual send fails (NULL data with len > 0)
    httpd_err_t err = httpd_ws_send(&ws, NULL, 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(HTTPD_ERR_IO, err);
}

static void test_ws_send_text_null_ws(void) {
    // Returns HTTPD_ERR_CONN_CLOSED when ws is NULL
    httpd_err_t err = httpd_ws_send_text(NULL, "test");
    TEST_ASSERT_EQUAL(HTTPD_ERR_CONN_CLOSED, err);
}

static void test_ws_send_text_null_text(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    // Now properly returns HTTPD_ERR_INVALID_ARG for NULL text
    httpd_err_t err = httpd_ws_send_text(&ws, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== WebSocket Close Tests ====================

static void test_ws_close_null_ws(void) {
    httpd_err_t err = httpd_ws_close(NULL, 1000, "normal");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== WebSocket Broadcast Tests ====================

static void test_ws_broadcast_null_server(void) {
    int result = httpd_ws_broadcast(NULL, "/ws", "test", 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_ws_broadcast_null_pattern(void) {
    start_test_server();

    int result = httpd_ws_broadcast(test_server, NULL, "test", 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);

    stop_test_server();
}

static void test_ws_broadcast_null_data(void) {
    start_test_server();

    int result = httpd_ws_broadcast(test_server, "/ws", NULL, 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);

    stop_test_server();
}

// ==================== WebSocket Channel Tests ====================

static void test_ws_join_null_ws(void) {
    httpd_err_t err = httpd_ws_join(NULL, "channel");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_ws_join_null_channel(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    httpd_err_t err = httpd_ws_join(&ws, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_ws_join_empty_channel(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    httpd_err_t err = httpd_ws_join(&ws, "");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_ws_leave_null_ws(void) {
    httpd_err_t err = httpd_ws_leave(NULL, "channel");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_ws_leave_null_channel(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    httpd_err_t err = httpd_ws_leave(&ws, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_ws_leave_all_null_ws(void) {
    // httpd_ws_leave_all returns void, just verify it doesn't crash
    httpd_ws_leave_all(NULL);
    TEST_PASS();
}

static void test_ws_in_channel_null_ws(void) {
    bool result = httpd_ws_in_channel(NULL, "channel");
    TEST_ASSERT_FALSE(result);
}

static void test_ws_in_channel_null_channel(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    bool result = httpd_ws_in_channel(&ws, NULL);
    TEST_ASSERT_FALSE(result);
}

// ==================== WebSocket Publish Tests ====================

static void test_ws_publish_null_server(void) {
    int result = httpd_ws_publish(NULL, "channel", "test", 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_ws_publish_null_channel(void) {
    start_test_server();

    int result = httpd_ws_publish(test_server, NULL, "test", 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);

    stop_test_server();
}

static void test_ws_publish_null_data(void) {
    start_test_server();

    int result = httpd_ws_publish(test_server, "channel", NULL, 4, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(-1, result);

    stop_test_server();
}

// ==================== WebSocket Channel Size Tests ====================

static void test_ws_channel_size_null_server(void) {
    unsigned int size = httpd_ws_channel_size(NULL, "channel");
    TEST_ASSERT_EQUAL(0, size);
}

static void test_ws_channel_size_null_channel(void) {
    start_test_server();

    unsigned int size = httpd_ws_channel_size(test_server, NULL);
    TEST_ASSERT_EQUAL(0, size);

    stop_test_server();
}

static void test_ws_channel_size_nonexistent(void) {
    start_test_server();

    unsigned int size = httpd_ws_channel_size(test_server, "nonexistent");
    TEST_ASSERT_EQUAL(0, size);

    stop_test_server();
}

// ==================== WebSocket Connection Count Tests ====================

static void test_ws_get_connection_count_null_server(void) {
    unsigned int count = httpd_ws_get_connection_count(NULL);
    TEST_ASSERT_EQUAL(0, count);
}

static void test_ws_get_connection_count_no_connections(void) {
    start_test_server();

    unsigned int count = httpd_ws_get_connection_count(test_server);
    TEST_ASSERT_EQUAL(0, count);

    stop_test_server();
}

// ==================== WebSocket Route Registration Tests ====================

static void test_register_ws_route(void) {
    start_test_server();

    httpd_ws_route_t route = {
        .pattern = "/ws/test",
        .handler = ws_test_handler,
        .ping_interval_ms = 0
    };

    httpd_err_t err = httpd_register_ws_route(test_server, &route);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    stop_test_server();
}

static void test_register_ws_route_null_server(void) {
    httpd_ws_route_t route = {
        .pattern = "/ws/test",
        .handler = ws_test_handler
    };

    httpd_err_t err = httpd_register_ws_route(NULL, &route);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_register_ws_route_null_route(void) {
    start_test_server();

    httpd_err_t err = httpd_register_ws_route(test_server, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    stop_test_server();
}

static void test_register_ws_route_with_ping(void) {
    start_test_server();

    httpd_ws_route_t route = {
        .pattern = "/ws/ping",
        .handler = ws_test_handler,
        .ping_interval_ms = 30000
    };

    httpd_err_t err = httpd_register_ws_route(test_server, &route);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    stop_test_server();
}

// ==================== WebSocket User Data Tests ====================

static void test_ws_set_user_data(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    int user_data = 42;
    httpd_ws_set_user_data(&ws, &user_data);
    TEST_ASSERT_EQUAL_PTR(&user_data, ws.user_data);
}

static void test_ws_get_user_data(void) {
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);

    int user_data = 42;
    ws.user_data = &user_data;

    void* result = httpd_ws_get_user_data(&ws);
    TEST_ASSERT_EQUAL_PTR(&user_data, result);
}

static void test_ws_get_user_data_null_ws(void) {
    void* result = httpd_ws_get_user_data(NULL);
    TEST_ASSERT_NULL(result);
}

// ==================== WebSocket Accept/Reject Tests ====================

static void test_ws_reject_null_req(void) {
    httpd_err_t err = httpd_ws_reject(NULL, 403, "Forbidden");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== WebSocket Frame Type Tests ====================

static void test_ws_frame_types(void) {
    // Verify frame type values match RFC 6455
    TEST_ASSERT_EQUAL(0x1, WS_TYPE_TEXT);
    TEST_ASSERT_EQUAL(0x2, WS_TYPE_BINARY);
    TEST_ASSERT_EQUAL(0x8, WS_TYPE_CLOSE);
    TEST_ASSERT_EQUAL(0x9, WS_TYPE_PING);
    TEST_ASSERT_EQUAL(0xA, WS_TYPE_PONG);
}

// ==================== Test Runner ====================

void test_websocket_api_run(void) {
    ESP_LOGI(TAG, "Running WebSocket API tests");

    // Send tests
    RUN_TEST(test_ws_send_null_ws);
    RUN_TEST(test_ws_send_null_data);
    RUN_TEST(test_ws_send_text_null_ws);
    RUN_TEST(test_ws_send_text_null_text);

    // Close tests
    RUN_TEST(test_ws_close_null_ws);

    // Broadcast tests
    RUN_TEST(test_ws_broadcast_null_server);
    RUN_TEST(test_ws_broadcast_null_pattern);
    RUN_TEST(test_ws_broadcast_null_data);

    // Channel tests
    RUN_TEST(test_ws_join_null_ws);
    RUN_TEST(test_ws_join_null_channel);
    RUN_TEST(test_ws_join_empty_channel);
    RUN_TEST(test_ws_leave_null_ws);
    RUN_TEST(test_ws_leave_null_channel);
    RUN_TEST(test_ws_leave_all_null_ws);
    RUN_TEST(test_ws_in_channel_null_ws);
    RUN_TEST(test_ws_in_channel_null_channel);

    // Publish tests
    RUN_TEST(test_ws_publish_null_server);
    RUN_TEST(test_ws_publish_null_channel);
    RUN_TEST(test_ws_publish_null_data);

    // Channel size tests
    RUN_TEST(test_ws_channel_size_null_server);
    RUN_TEST(test_ws_channel_size_null_channel);
    RUN_TEST(test_ws_channel_size_nonexistent);

    // Connection count tests
    RUN_TEST(test_ws_get_connection_count_null_server);
    RUN_TEST(test_ws_get_connection_count_no_connections);

    // Route registration tests
    RUN_TEST(test_register_ws_route);
    RUN_TEST(test_register_ws_route_null_server);
    RUN_TEST(test_register_ws_route_null_route);
    RUN_TEST(test_register_ws_route_with_ping);

    // User data tests
    RUN_TEST(test_ws_set_user_data);
    RUN_TEST(test_ws_get_user_data);
    RUN_TEST(test_ws_get_user_data_null_ws);

    // Accept/Reject tests
    RUN_TEST(test_ws_reject_null_req);

    // Frame type tests
    RUN_TEST(test_ws_frame_types);

    ESP_LOGI(TAG, "WebSocket API tests completed");
}
