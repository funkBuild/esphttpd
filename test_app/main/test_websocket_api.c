/**
 * @file test_websocket_api.c
 * @brief Unit tests for WebSocket high-level API
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "websocket.h"
#include "send_buffer.h"
#include "esp_log.h"
#include <stdio.h>
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

// ==================== WS Lifecycle Tests (frame error / stop) ====================

// Mock send buffer at pool slot 0 (same pattern as test_http_api.c): seeded
// with one byte so send paths queue into the ring instead of calling send()
// on the mock fd=-1.
static send_buffer_t ws_mock_send_buf;
static bool ws_mock_send_buf_installed = false;

static void ws_install_mock_send_buffer(void) {
    if (!g_test_send_buffers) return;
    send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
    send_buffer_init(&ws_mock_send_buf);
    send_buffer_alloc(&ws_mock_send_buf);
    uint8_t dummy = 0;
    send_buffer_queue(&ws_mock_send_buf, &dummy, 1);
    bufs[0] = &ws_mock_send_buf;
    ws_mock_send_buf_installed = true;
}

static void ws_remove_mock_send_buffer(void) {
    if (!ws_mock_send_buf_installed) return;
    if (g_test_send_buffers) {
        send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
        bufs[0] = NULL;
    }
    // Safe even if httpd_stop already freed it via the slot-0 pointer:
    // send_buffer_free re-inits the struct, so a second free is a no-op.
    send_buffer_free(&ws_mock_send_buf);
    ws_mock_send_buf_installed = false;
}

static int ws_lifecycle_connect_count = 0;
static int ws_lifecycle_disconnect_count = 0;

static httpd_err_t ws_lifecycle_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
    (void)ws;
    if (event->type == WS_EVENT_CONNECT) ws_lifecycle_connect_count++;
    if (event->type == WS_EVENT_DISCONNECT) ws_lifecycle_disconnect_count++;
    return HTTPD_OK;
}

// Run a real WebSocket upgrade on pool slot 0 through on_http_request so the
// server sets up ws_contexts[0] (route, frame context, active mask) exactly
// as production does. Requires the mock send buffer (handshake bytes queue
// into it).
static connection_t* ws_upgrade_slot0(const char* pattern) {
    httpd_ws_route_t route = {
        .pattern = pattern,
        .handler = ws_lifecycle_handler,
        .ping_interval_ms = 0
    };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_ws_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n", pattern);
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(req));
    g_server->handlers.on_http_request(conn, (uint8_t*)req, (size_t)n);
    return conn;
}

// Fix 1: a protocol-invalid frame (here: unmasked - RFC 6455 requires client
// frames to be masked) must close the connection instead of leaving it
// wedged. Before the fix, WS_FRAME_ERROR fell through to the "need more
// data" path, the parser stayed desynced, and - because WS connections are
// exempt from the idle timeout - the slot was leaked forever (16 bad frames
// exhausted the pool).
static void test_ws_invalid_frame_closes_connection(void) {
    start_test_server();
    ws_install_mock_send_buffer();
    ws_lifecycle_connect_count = 0;
    ws_lifecycle_disconnect_count = 0;

    connection_t* conn = ws_upgrade_slot0("/ws-err");
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn->state);
    TEST_ASSERT_EQUAL(1, ws_lifecycle_connect_count);
    TEST_ASSERT_EQUAL(1u, g_server->connection_pool.ws_active_mask & 1u);

    // Unmasked text frame "Hello"
    uint8_t bad_frame[] = { 0x81, 0x05, 'H', 'e', 'l', 'l', 'o' };
    g_server->handlers.on_ws_frame(conn, bad_frame, sizeof(bad_frame));

    // Connection handed to the reaper, not wedged in CONN_STATE_WEBSOCKET
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);
    // WS disconnect event delivered exactly once
    TEST_ASSERT_EQUAL(1, ws_lifecycle_disconnect_count);
    // Removed from the active-WS mask
    TEST_ASSERT_EQUAL(0u, g_server->connection_pool.ws_active_mask & 1u);

    ws_remove_mock_send_buffer();
    stop_test_server();
    // Stop must not double-fire the disconnect (connection already closed)
    TEST_ASSERT_EQUAL(1, ws_lifecycle_disconnect_count);
}

// Fix 3a: httpd_stop with a live WebSocket connection must fire
// WS_EVENT_DISCONNECT to the route handler before teardown - app per-socket
// state (allocated on WS_EVENT_CONNECT) leaked on every stop otherwise.
static void test_ws_stop_fires_disconnect_event(void) {
    start_test_server();
    ws_install_mock_send_buffer();
    ws_lifecycle_connect_count = 0;
    ws_lifecycle_disconnect_count = 0;

    connection_t* conn = ws_upgrade_slot0("/ws-stop");
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn->state);
    TEST_ASSERT_EQUAL(1, ws_lifecycle_connect_count);
    TEST_ASSERT_EQUAL(1u, g_server->connection_pool.ws_active_mask & 1u);

    // Stop with the WS connection still active
    stop_test_server();
    TEST_ASSERT_EQUAL(1, ws_lifecycle_disconnect_count);

    // httpd_stop already ran send_buffer_free on the installed mock (it was
    // wired in as slot 0's buffer); this just clears the bookkeeping.
    ws_remove_mock_send_buffer();
}

// In-place header retention across a WebSocket upgrade: the upgrade request's
// headers live NUL-terminated inside recv_buf, which is retained for the
// WHOLE WebSocket connection lifetime (freed at disconnect, not at upgrade),
// so WS handlers can read upgrade-request headers on any later event.
static bool ws_hdr_msg_seen = false;
static char ws_hdr_version[16];
static char ws_hdr_host[32];

static httpd_err_t ws_hdr_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
    (void)ws;
    if (event->type == WS_EVENT_MESSAGE) {
        ws_hdr_msg_seen = true;
        // The upgrade request context persists at the connection's pool slot
        test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
        const char* v = ctxs && ctxs[0] ?
            httpd_req_get_header(&ctxs[0]->req, "Sec-WebSocket-Version") : NULL;
        snprintf(ws_hdr_version, sizeof(ws_hdr_version), "%s", v ? v : "(null)");
        const char* h = ctxs && ctxs[0] ?
            httpd_req_get_header(&ctxs[0]->req, "Host") : NULL;
        snprintf(ws_hdr_host, sizeof(ws_hdr_host), "%s", h ? h : "(null)");
    }
    return HTTPD_OK;
}

static void test_ws_headers_readable_after_upgrade(void) {
    start_test_server();
    ws_install_mock_send_buffer();
    ws_hdr_msg_seen = false;
    ws_hdr_version[0] = '\0';
    ws_hdr_host[0] = '\0';

    httpd_ws_route_t route = {
        .pattern = "/ws-hdr",
        .handler = ws_hdr_handler,
        .ping_interval_ms = 0
    };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_ws_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    char req[] = "GET /ws-hdr HTTP/1.1\r\n"
                 "Host: localhost\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n";
    g_server->handlers.on_http_request(conn, (uint8_t*)req, sizeof(req) - 1);
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn->state);

    // Upgrade headers readable AFTER on_http_request returned (recv_buf was
    // NOT freed at upgrade)
    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs);
    TEST_ASSERT_NOT_NULL(ctxs[0]);
    const char* v = httpd_req_get_header(&ctxs[0]->req, "Sec-WebSocket-Version");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("13", v);

    // ...and from within the WS handler on a later frame event.
    // Masked text frame "hi" (all-zero mask key leaves the payload as-is).
    uint8_t frame[] = { 0x81, 0x82, 0x00, 0x00, 0x00, 0x00, 'h', 'i' };
    g_server->handlers.on_ws_frame(conn, frame, sizeof(frame));
    TEST_ASSERT_TRUE(ws_hdr_msg_seen);
    TEST_ASSERT_EQUAL_STRING("13", ws_hdr_version);
    TEST_ASSERT_EQUAL_STRING("localhost", ws_hdr_host);

    ws_remove_mock_send_buffer();
    stop_test_server();
}

// ==================== Test Runner ====================

// Fix 3: in socket mode the WS send entry points (send/broadcast/publish/close)
// take the recursive send mutex, which httpd_start() creates. Verify the mutex
// is present (the APIs run their now-lock-guarded bodies to completion without
// deadlock or crash) and that httpd_ws_close clamps an oversized reason (RFC
// 6455: reason text <= 123 bytes) rather than overflowing.
static void test_ws_send_apis_lock_guarded_after_init(void) {
    start_test_server();
    TEST_ASSERT_NOT_NULL(test_server);

    // Broadcast/publish with no active WS connections: their SEND_LOCK-wrapped
    // bodies execute and return 0.
    TEST_ASSERT_EQUAL(0, httpd_ws_broadcast(test_server, "*", "hi", 2, WS_TYPE_TEXT));
    TEST_ASSERT_EQUAL(0, httpd_ws_publish(test_server, "no-such-channel", "hi", 2, WS_TYPE_TEXT));

    // httpd_ws_close with a 200-byte reason must clamp to <=123 and complete
    // under the send lock without overrunning its 128-byte frame buffer.
    httpd_ws_t ws;
    connection_t conn;
    setup_mock_ws(&ws, &conn);
    conn.pool_index = 0;
    char big_reason[200];
    memset(big_reason, 'A', sizeof(big_reason) - 1);
    big_reason[sizeof(big_reason) - 1] = '\0';
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_ws_close(&ws, 1000, big_reason));

    stop_test_server();
}

void test_websocket_api_run(void) {
    ESP_LOGI(TAG, "Running WebSocket API tests");

    // Send tests
    RUN_TEST(test_ws_send_null_ws);
    RUN_TEST(test_ws_send_null_data);
    RUN_TEST(test_ws_send_text_null_ws);
    RUN_TEST(test_ws_send_text_null_text);

    // Send-path locking / reason clamp
    RUN_TEST(test_ws_send_apis_lock_guarded_after_init);

    // Close tests
    RUN_TEST(test_ws_close_null_ws);

    // Lifecycle tests (frame error handling / stop-time disconnect events)
    RUN_TEST(test_ws_invalid_frame_closes_connection);
    RUN_TEST(test_ws_stop_fires_disconnect_event);
    RUN_TEST(test_ws_headers_readable_after_upgrade);

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
