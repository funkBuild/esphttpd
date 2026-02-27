#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

static const char* TAG = "TEST_DEFER";

// Test state tracking
static size_t body_bytes_received = 0;
static size_t body_callback_count = 0;
static bool done_callback_called = false;
static httpd_err_t done_callback_error = HTTPD_OK;
static httpd_handle_t test_handle = NULL;
static const uint8_t* last_body_data = NULL;
static size_t last_body_len = 0;

// Reset test state
static void reset_test_state(void) {
    body_bytes_received = 0;
    body_callback_count = 0;
    done_callback_called = false;
    done_callback_error = HTTPD_OK;
    last_body_data = NULL;
    last_body_len = 0;
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

// ==================== BODY CALLBACKS ====================

static httpd_err_t test_body_callback(httpd_req_t* req, const void* data, size_t len) {
    (void)req;
    last_body_data = (const uint8_t*)data;
    last_body_len = len;
    body_bytes_received += len;
    body_callback_count++;
    ESP_LOGD(TAG, "Body callback: received %zu bytes (total: %zu)", len, body_bytes_received);
    return HTTPD_OK;
}

static httpd_err_t test_body_callback_error(httpd_req_t* req, const void* data, size_t len) {
    (void)req;
    (void)data;
    (void)len;
    // Return error to test error handling
    return HTTPD_ERR_IO;
}

// ==================== DONE CALLBACKS ====================

static void test_done_callback(httpd_req_t* req, httpd_err_t err) {
    (void)req;
    done_callback_called = true;
    done_callback_error = err;
    ESP_LOGD(TAG, "Done callback: err=%d", err);
}

// ==================== TEST FUNCTIONS ====================

// Test httpd_req_defer basic setup
static void test_defer_basic_setup(void) {
    start_test_server();
    reset_test_state();

    // Create a mock request context
    connection_t conn = {0};
    conn.fd = -1;
    conn.state = CONN_STATE_HTTP_HEADERS;

    // Test with NULL req
    httpd_err_t err = httpd_req_defer(NULL, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    stop_test_server();
}

// Test httpd_req_defer with NULL done callback (should fail)
static void test_defer_null_done_callback(void) {
    start_test_server();
    reset_test_state();

    // Cannot test with actual request without full server setup
    // Just verify the API rejects NULL done callback
    httpd_err_t err = httpd_req_defer(NULL, test_body_callback, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    stop_test_server();
}

// Test httpd_req_is_deferred
static void test_is_deferred(void) {
    // NULL req should return false
    TEST_ASSERT_FALSE(httpd_req_is_deferred(NULL));
}

// Test httpd_req_defer_pause with NULL
static void test_defer_pause_null(void) {
    httpd_err_t err = httpd_req_defer_pause(NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// Test httpd_req_defer_resume with NULL
static void test_defer_resume_null(void) {
    httpd_err_t err = httpd_req_defer_resume(NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// Test httpd_req_defer_to_file with NULL parameters
static void test_defer_to_file_null_params(void) {
    httpd_err_t err;

    // NULL req
    err = httpd_req_defer_to_file(NULL, "/tmp/test.bin", test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    // NULL path - can't test easily without actual request

    // NULL callback - can't test easily without actual request
}

// Test connection deferred flag initialization
static void test_connection_deferred_flag(void) {
    connection_t conn;
    memset(&conn, 0, sizeof(conn));

    // Initial state should be not deferred
    TEST_ASSERT_EQUAL(0, conn.deferred);
    TEST_ASSERT_EQUAL(0, conn.defer_paused);

    // Set deferred
    conn.deferred = 1;
    TEST_ASSERT_EQUAL(1, conn.deferred);

    // Set paused
    conn.defer_paused = 1;
    TEST_ASSERT_EQUAL(1, conn.defer_paused);

    // Clear
    conn.deferred = 0;
    conn.defer_paused = 0;
    TEST_ASSERT_EQUAL(0, conn.deferred);
    TEST_ASSERT_EQUAL(0, conn.defer_paused);
}

// Test that defer properly tracks body_received
static void test_defer_body_tracking(void) {
    start_test_server();
    reset_test_state();

    // Get connection pool
    TEST_ASSERT_NOT_NULL(g_server);

    // Create a minimal mock setup to test body tracking
    // We can't fully test without actual socket I/O in QEMU,
    // but we can verify the data structures are properly set up

    connection_pool_t* pool = &g_server->connection_pool;

    // Mark a connection as deferred
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Initialize for test
    conn->fd = -1;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->deferred = 1;
    conn->defer_paused = 0;
    conn->content_length = 1000;
    conn->bytes_received = 0;

    // Verify state
    TEST_ASSERT_EQUAL(1, conn->deferred);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn->state);
    TEST_ASSERT_EQUAL(1000, conn->content_length);

    stop_test_server();
}

// Test defer with immediate completion (body already received)
static void test_defer_immediate_completion(void) {
    start_test_server();
    reset_test_state();

    // This tests the path where body is already complete when defer is called
    // We verify the data structures support this scenario

    TEST_ASSERT_NOT_NULL(g_server);
    connection_pool_t* pool = &g_server->connection_pool;

    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up as if body was already received with headers
    conn->fd = -1;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->content_length = 50;
    conn->bytes_received = 50;  // Already complete

    // The defer logic should handle this case
    // (actual test would need full request context)

    stop_test_server();
}

// Test callback type signatures
static void test_callback_type_compatibility(void) {
    // Verify callback types compile correctly with expected signatures
    httpd_body_cb_t body_cb = test_body_callback;
    httpd_done_cb_t done_cb = test_done_callback;

    TEST_ASSERT_NOT_NULL(body_cb);
    TEST_ASSERT_NOT_NULL(done_cb);

    // Verify error callback compiles
    httpd_body_cb_t error_cb = test_body_callback_error;
    TEST_ASSERT_NOT_NULL(error_cb);
}

// Helper to get request context by index
// g_test_request_contexts points to request_context_t* array[MAX_CONNECTIONS]
// (array of pointers, dynamically allocated per-connection in on_connect)
static test_request_context_t* get_test_ctx(int idx) {
    if (!g_test_request_contexts) return NULL;
    test_request_context_t** ptrs = (test_request_context_t**)g_test_request_contexts;
    // Allocate on demand for tests (since we don't go through on_connect)
    if (!ptrs[idx]) {
        ptrs[idx] = (test_request_context_t*)calloc(1, sizeof(test_request_context_t));
    }
    return ptrs[idx];
}

// Test httpd_req_defer with full request context
static void test_defer_with_request_context(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;  // Fake fd
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 100;
    conn->bytes_received = 0;

    // Set up request context
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 100;
    ctx->req.body_received = 0;
    ctx->body_buf_len = 0;
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;
    ctx->defer.paused = false;

    // Call httpd_req_defer
    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Verify deferred state
    TEST_ASSERT_TRUE(ctx->defer.active);
    TEST_ASSERT_FALSE(ctx->defer.paused);
    TEST_ASSERT_EQUAL(1, conn->deferred);
    TEST_ASSERT_EQUAL(0, conn->defer_paused);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn->state);

    // Body not complete yet, done_callback should not be called
    TEST_ASSERT_FALSE(done_callback_called);

    stop_test_server();
}

// Test httpd_req_defer delivers pre-received body data
static void test_defer_pre_received_body(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 50;
    conn->bytes_received = 0;

    // Set up request context with pre-received body data
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 50;
    ctx->req.body_received = 0;

    // Allocate and populate body buffer (simulating data received with headers)
    const uint8_t test_data[] = "Hello, World!";
    ctx->body_buf = (uint8_t*)malloc(sizeof(test_data));
    TEST_ASSERT_NOT_NULL(ctx->body_buf);
    memcpy(ctx->body_buf, test_data, sizeof(test_data));
    ctx->body_buf_len = sizeof(test_data);
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;
    ctx->defer.paused = false;

    // Call httpd_req_defer
    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Verify body callback was called with pre-received data
    TEST_ASSERT_EQUAL(1, body_callback_count);
    TEST_ASSERT_EQUAL(sizeof(test_data), body_bytes_received);
    TEST_ASSERT_EQUAL(sizeof(test_data), ctx->req.body_received);

    // body_buf_pos should be updated
    TEST_ASSERT_EQUAL(ctx->body_buf_len, ctx->body_buf_pos);

    // Body not yet complete (50 bytes expected, only 14 received)
    TEST_ASSERT_FALSE(done_callback_called);

    stop_test_server();
}

// Test httpd_req_defer with body already complete
static void test_defer_body_already_complete(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 10;  // Small body
    conn->bytes_received = 0;

    // Set up request context
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 10;
    ctx->req.body_received = 0;

    // Allocate and populate complete body in buffer
    const uint8_t test_data[] = "0123456789";  // 10 bytes
    ctx->body_buf = (uint8_t*)malloc(10);
    TEST_ASSERT_NOT_NULL(ctx->body_buf);
    memcpy(ctx->body_buf, test_data, 10);
    ctx->body_buf_len = 10;
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;
    ctx->defer.paused = false;

    // Call httpd_req_defer
    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Verify body callback was called
    TEST_ASSERT_EQUAL(1, body_callback_count);
    TEST_ASSERT_EQUAL(10, body_bytes_received);

    // Body complete, done_callback should be called
    TEST_ASSERT_TRUE(done_callback_called);
    TEST_ASSERT_EQUAL(HTTPD_OK, done_callback_error);

    // Defer should be deactivated
    TEST_ASSERT_FALSE(ctx->defer.active);
    TEST_ASSERT_EQUAL(0, conn->deferred);

    stop_test_server();
}

// Test httpd_req_defer_pause and resume
static void test_defer_pause_resume(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->deferred = 1;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 1000;
    conn->bytes_received = 0;

    // Set up request context in deferred mode
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 1000;
    ctx->defer.active = true;
    ctx->defer.paused = false;
    ctx->defer.on_body = test_body_callback;
    ctx->defer.on_done = test_done_callback;

    // Pause
    httpd_err_t err = httpd_req_defer_pause(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_TRUE(ctx->defer.paused);
    TEST_ASSERT_EQUAL(1, conn->defer_paused);

    // Resume
    err = httpd_req_defer_resume(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_FALSE(ctx->defer.paused);
    TEST_ASSERT_EQUAL(0, conn->defer_paused);

    stop_test_server();
}

// Test httpd_req_defer_pause fails when not in deferred mode
static void test_defer_pause_not_deferred(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection - not in deferred mode
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;

    // Set up request context - not in deferred mode
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->defer.active = false;

    // Pause should fail
    httpd_err_t err = httpd_req_defer_pause(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    stop_test_server();
}

// Test httpd_req_defer_resume fails when not in deferred mode
static void test_defer_resume_not_deferred(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection - not in deferred mode
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->deferred = 0;
    conn->pool_index = idx;

    // Set up request context - not in deferred mode
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->defer.active = false;

    // Resume should fail
    httpd_err_t err = httpd_req_defer_resume(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    stop_test_server();
}

// Test httpd_req_is_deferred returns correct state
static void test_is_deferred_with_context(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    conn->fd = 999;
    conn->pool_index = idx;

    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;

    // Not deferred
    ctx->defer.active = false;
    TEST_ASSERT_FALSE(httpd_req_is_deferred(&ctx->req));

    // Deferred
    ctx->defer.active = true;
    TEST_ASSERT_TRUE(httpd_req_is_deferred(&ctx->req));

    stop_test_server();
}

// Test body callback error stops defer and calls done with error
static void test_defer_body_callback_error(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 100;
    conn->bytes_received = 0;

    // Set up request context with body data
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 100;
    ctx->req.body_received = 0;

    const uint8_t test_data[] = "test";
    ctx->body_buf = (uint8_t*)malloc(sizeof(test_data));
    TEST_ASSERT_NOT_NULL(ctx->body_buf);
    memcpy(ctx->body_buf, test_data, sizeof(test_data));
    ctx->body_buf_len = sizeof(test_data);
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;

    // Call httpd_req_defer with error callback
    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback_error, test_done_callback);

    // Should fail because body callback returns error
    TEST_ASSERT_EQUAL(HTTPD_ERR_IO, err);

    // Done callback should be called with error
    TEST_ASSERT_TRUE(done_callback_called);
    TEST_ASSERT_EQUAL(HTTPD_ERR_IO, done_callback_error);

    // Defer should be deactivated
    TEST_ASSERT_FALSE(ctx->defer.active);
    TEST_ASSERT_EQUAL(0, conn->deferred);

    stop_test_server();
}

// Test httpd_req_defer with NULL body callback (valid case - no body data expected)
static void test_defer_null_body_callback(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 100;
    conn->bytes_received = 0;

    // Set up request context - no pre-received body
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 100;
    ctx->req.body_received = 0;
    ctx->body_buf_len = 0;
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;

    // Call httpd_req_defer with NULL body callback
    httpd_err_t err = httpd_req_defer(&ctx->req, NULL, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Should still enter deferred mode
    TEST_ASSERT_TRUE(ctx->defer.active);
    TEST_ASSERT_EQUAL(1, conn->deferred);

    stop_test_server();
}

// Test httpd_req_defer with zero content length
static void test_defer_zero_content_length(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_server);
    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    connection_pool_t* pool = &g_server->connection_pool;
    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Set up connection
    conn->fd = 999;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->deferred = 0;
    conn->defer_paused = 0;
    conn->pool_index = idx;
    conn->content_length = 0;
    conn->bytes_received = 0;

    // Set up request context
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.content_length = 0;
    ctx->req.body_received = 0;
    ctx->body_buf_len = 0;
    ctx->body_buf_pos = 0;
    ctx->defer.active = false;

    // Call httpd_req_defer
    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // With zero content length, body is "complete" but condition checks content_length > 0
    // So done callback should NOT be called immediately
    TEST_ASSERT_FALSE(done_callback_called);

    // Should still be in deferred mode (waiting for end of stream)
    TEST_ASSERT_TRUE(ctx->defer.active);

    stop_test_server();
}

// Test httpd_req_defer connection not available (NULL _internal)
static void test_defer_no_connection(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    int idx = 0;
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = NULL;  // No connection

    httpd_err_t err = httpd_req_defer(&ctx->req, test_body_callback, test_done_callback);
    TEST_ASSERT_EQUAL(HTTPD_ERR_CONN_CLOSED, err);

    stop_test_server();
}

// Test httpd_req_defer_pause with no connection
static void test_defer_pause_no_connection(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    int idx = 0;
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = NULL;

    httpd_err_t err = httpd_req_defer_pause(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_ERR_CONN_CLOSED, err);

    stop_test_server();
}

// Test httpd_req_defer_resume with no connection
static void test_defer_resume_no_connection(void) {
    start_test_server();
    reset_test_state();

    TEST_ASSERT_NOT_NULL(g_test_request_contexts);

    int idx = 0;
    test_request_context_t* ctx = get_test_ctx(idx);
    TEST_ASSERT_NOT_NULL(ctx);

    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = NULL;

    httpd_err_t err = httpd_req_defer_resume(&ctx->req);
    TEST_ASSERT_EQUAL(HTTPD_ERR_CONN_CLOSED, err);

    stop_test_server();
}

// ==================== TEST RUNNER ====================

void test_defer_run(void) {
    ESP_LOGI(TAG, "Running Defer (Async) tests");

    // NULL parameter tests
    RUN_TEST(test_defer_basic_setup);
    RUN_TEST(test_defer_null_done_callback);
    RUN_TEST(test_is_deferred);
    RUN_TEST(test_defer_pause_null);
    RUN_TEST(test_defer_resume_null);
    RUN_TEST(test_defer_to_file_null_params);

    // Connection flag tests
    RUN_TEST(test_connection_deferred_flag);
    RUN_TEST(test_defer_body_tracking);
    RUN_TEST(test_defer_immediate_completion);
    RUN_TEST(test_callback_type_compatibility);

    // Full request context tests
    RUN_TEST(test_defer_with_request_context);
    RUN_TEST(test_defer_pre_received_body);
    RUN_TEST(test_defer_body_already_complete);
    RUN_TEST(test_defer_pause_resume);
    RUN_TEST(test_defer_pause_not_deferred);
    RUN_TEST(test_defer_resume_not_deferred);
    RUN_TEST(test_is_deferred_with_context);
    RUN_TEST(test_defer_body_callback_error);
    RUN_TEST(test_defer_null_body_callback);
    RUN_TEST(test_defer_zero_content_length);
    RUN_TEST(test_defer_no_connection);
    RUN_TEST(test_defer_pause_no_connection);
    RUN_TEST(test_defer_resume_no_connection);

    ESP_LOGI(TAG, "Defer tests completed");
}
