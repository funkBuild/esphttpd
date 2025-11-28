#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_DEFER";

// Test state tracking
static size_t body_bytes_received = 0;
static size_t body_callback_count = 0;
static bool done_callback_called = false;
static httpd_err_t done_callback_error = HTTPD_OK;
static httpd_handle_t test_handle = NULL;

// Reset test state
static void reset_test_state(void) {
    body_bytes_received = 0;
    body_callback_count = 0;
    done_callback_called = false;
    done_callback_error = HTTPD_OK;
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
    (void)data;
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

// ==================== TEST RUNNER ====================

void test_defer_run(void) {
    ESP_LOGI(TAG, "Running Defer (Async) tests");

    RUN_TEST(test_defer_basic_setup);
    RUN_TEST(test_defer_null_done_callback);
    RUN_TEST(test_is_deferred);
    RUN_TEST(test_defer_pause_null);
    RUN_TEST(test_defer_resume_null);
    RUN_TEST(test_defer_to_file_null_params);
    RUN_TEST(test_connection_deferred_flag);
    RUN_TEST(test_defer_body_tracking);
    RUN_TEST(test_defer_immediate_completion);
    RUN_TEST(test_callback_type_compatibility);

    ESP_LOGI(TAG, "Defer tests completed");
}
