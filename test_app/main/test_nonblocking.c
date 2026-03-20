#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "send_buffer.h"
#include "test_exports.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_NONBLOCK";

// ============================================================================
// TEST HELPERS
// ============================================================================

static httpd_handle_t test_handle = NULL;

static void start_test_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;
    httpd_err_t err = httpd_start(&test_handle, &config);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
}

static void stop_test_server(void) {
    if (test_handle) {
        httpd_stop(test_handle);
        test_handle = NULL;
    }
}

// ============================================================================
// CHUNKED TRANSFER ENCODING TESTS
// ============================================================================

// Test that chunk frame is built atomically for small chunks
static void test_chunk_frame_atomic_small(void)
{
    // Test that a small chunk (< 512 bytes) is built as a complete frame
    // before being sent. This ensures the chunk format is not corrupted.

    // We can't easily test the actual send behavior without sockets,
    // but we can verify the send_buffer correctly handles atomic writes.

    send_buffer_t sb;
    send_buffer_init(&sb);
    TEST_ASSERT_TRUE(send_buffer_alloc(&sb));

    // Simulate building a chunked frame: "a\r\nHello World\r\n" for 10 bytes
    const char* test_data = "Hello Wrld";  // 10 bytes
    char frame_buf[64];
    int header_len = snprintf(frame_buf, sizeof(frame_buf), "%zx\r\n", strlen(test_data));
    memcpy(frame_buf + header_len, test_data, strlen(test_data));
    memcpy(frame_buf + header_len + strlen(test_data), "\r\n", 2);
    size_t frame_size = header_len + strlen(test_data) + 2;

    // Queue atomically
    ssize_t queued = send_buffer_queue(&sb, frame_buf, frame_size);
    TEST_ASSERT_EQUAL(frame_size, queued);

    // Verify data integrity
    const uint8_t* peek;
    size_t peek_len = send_buffer_peek(&sb, &peek);
    TEST_ASSERT_EQUAL(frame_size, peek_len);
    TEST_ASSERT_EQUAL_STRING_LEN(frame_buf, (const char*)peek, frame_size);

    send_buffer_free(&sb);
}

// Test chunk frame format correctness
static void test_chunk_frame_format(void)
{
    // Verify chunk frame format: "size\r\ndata\r\n"

    const char* data = "Test chunk data";
    size_t data_len = strlen(data);

    char frame[256];
    int header_len = snprintf(frame, sizeof(frame), "%zx\r\n", data_len);
    memcpy(frame + header_len, data, data_len);
    memcpy(frame + header_len + data_len, "\r\n", 2);
    size_t frame_len = header_len + data_len + 2;

    // Parse the frame back
    char* crlf = strstr(frame, "\r\n");
    TEST_ASSERT_NOT_NULL(crlf);

    // Verify size
    int parsed_size;
    TEST_ASSERT_EQUAL(1, sscanf(frame, "%x", &parsed_size));
    TEST_ASSERT_EQUAL(data_len, parsed_size);

    // Verify data
    char* chunk_data = crlf + 2;
    TEST_ASSERT_EQUAL_STRING_LEN(data, chunk_data, data_len);

    // Verify terminator
    TEST_ASSERT_EQUAL_STRING_LEN("\r\n", chunk_data + data_len, 2);

    ESP_LOGI(TAG, "Chunk frame verified: size=%d, total_len=%zu", parsed_size, frame_len);
}

// Test final chunk format
static void test_final_chunk_format(void)
{
    // Final chunk is "0\r\n\r\n" (5 bytes)
    const char* final_chunk = "0\r\n\r\n";

    TEST_ASSERT_EQUAL(5, strlen(final_chunk));
    TEST_ASSERT_EQUAL('0', final_chunk[0]);
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n", final_chunk + 1);
}

// Test large chunk handling (> 512 bytes)
static void test_chunk_frame_large(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);
    TEST_ASSERT_TRUE(send_buffer_alloc(&sb));

    // Create a 1KB chunk
    size_t data_len = 1024;
    char* large_data = malloc(data_len);
    TEST_ASSERT_NOT_NULL(large_data);
    memset(large_data, 'X', data_len);

    // Build chunk header
    char header[16];
    int header_len = snprintf(header, sizeof(header), "%zx\r\n", data_len);

    // For large chunks, we queue header + data + terminator separately
    // but the send_buffer should be able to hold them atomically

    ssize_t q1 = send_buffer_queue(&sb, header, header_len);
    TEST_ASSERT_EQUAL(header_len, q1);

    ssize_t q2 = send_buffer_queue(&sb, large_data, data_len);
    TEST_ASSERT_EQUAL(data_len, q2);

    ssize_t q3 = send_buffer_queue(&sb, "\r\n", 2);
    TEST_ASSERT_EQUAL(2, q3);

    // Total queued
    size_t total = send_buffer_pending(&sb);
    TEST_ASSERT_EQUAL(header_len + data_len + 2, total);

    ESP_LOGI(TAG, "Large chunk queued: header=%d, data=%zu, total=%zu",
             header_len, data_len, total);

    free(large_data);
    send_buffer_free(&sb);
}

// Test multiple sequential chunks
static void test_multiple_chunks_sequential(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);
    TEST_ASSERT_TRUE(send_buffer_alloc(&sb));

    // Queue multiple small chunks
    const char* chunks[] = {"Chunk1", "Chunk2", "Chunk3"};
    size_t total_queued = 0;

    for (int i = 0; i < 3; i++) {
        size_t data_len = strlen(chunks[i]);
        char frame[64];
        int header_len = snprintf(frame, sizeof(frame), "%zx\r\n", data_len);
        memcpy(frame + header_len, chunks[i], data_len);
        memcpy(frame + header_len + data_len, "\r\n", 2);
        size_t frame_len = header_len + data_len + 2;

        ssize_t queued = send_buffer_queue(&sb, frame, frame_len);
        TEST_ASSERT_EQUAL(frame_len, queued);
        total_queued += frame_len;
    }

    // Queue final chunk
    send_buffer_queue(&sb, "0\r\n\r\n", 5);
    total_queued += 5;

    TEST_ASSERT_EQUAL(total_queued, send_buffer_pending(&sb));

    ESP_LOGI(TAG, "Sequential chunks queued: %zu bytes total", total_queued);

    send_buffer_free(&sb);
}

// ============================================================================
// CONTINUATION API TESTS
// ============================================================================

// Test state tracking
static size_t cont_bytes_received = 0;
static int cont_callback_count = 0;
static bool cont_completed = false;

static void reset_continuation_state(void) {
    cont_bytes_received = 0;
    cont_callback_count = 0;
    cont_completed = false;
}

// Test httpd_req_continue with NULL parameters
static void test_continue_null_params(void)
{
    httpd_err_t err;

    // NULL req
    err = httpd_req_continue(NULL, (httpd_continuation_t)0x1234, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// Test httpd_req_is_continuation with NULL
static void test_is_continuation_null(void)
{
    bool result = httpd_req_is_continuation(NULL);
    TEST_ASSERT_FALSE(result);
}

// Test connection continuation flag
static void test_connection_continuation_flag(void)
{
    connection_t conn;
    memset(&conn, 0, sizeof(conn));

    // Initial state
    TEST_ASSERT_EQUAL(0, conn.continuation);

    // Set flag
    conn.continuation = 1;
    TEST_ASSERT_EQUAL(1, conn.continuation);

    // Clear flag
    conn.continuation = 0;
    TEST_ASSERT_EQUAL(0, conn.continuation);
}

// Test continuation state structure
static void test_continuation_state_struct(void)
{
    httpd_req_continuation_t cont = {0};

    // Initial state
    TEST_ASSERT_NULL(cont.state);
    TEST_ASSERT_EQUAL(0, cont.phase);
    TEST_ASSERT_EQUAL(0, cont.expected_bytes);
    TEST_ASSERT_EQUAL(0, cont.received_bytes);

    // Set values
    int dummy_state = 42;
    cont.state = &dummy_state;
    cont.phase = 3;
    cont.expected_bytes = 1024;
    cont.received_bytes = 512;

    TEST_ASSERT_EQUAL_PTR(&dummy_state, cont.state);
    TEST_ASSERT_EQUAL(3, cont.phase);
    TEST_ASSERT_EQUAL(1024, cont.expected_bytes);
    TEST_ASSERT_EQUAL(512, cont.received_bytes);
}

// Test continuation handler signature
static httpd_err_t test_continuation_handler(httpd_req_t* req, const void* data,
                                             size_t len, httpd_req_continuation_t* cont) {
    (void)req;
    cont_callback_count++;

    if (data && len > 0) {
        cont_bytes_received += len;
        // Note: cont->received_bytes is updated automatically by the server
    }

    // Simulate processing - complete after 100 bytes
    if (cont_bytes_received >= 100) {
        cont_completed = true;
        return HTTPD_OK;
    }

    return HTTPD_ERR_WOULD_BLOCK;
}

// Test continuation handler type compatibility
static void test_continuation_handler_type(void)
{
    httpd_continuation_t handler = test_continuation_handler;
    TEST_ASSERT_NOT_NULL(handler);

    // Verify function pointer is callable
    reset_continuation_state();
    httpd_req_continuation_t cont = {0};

    // Initial call (data=NULL)
    httpd_err_t err = handler(NULL, NULL, 0, &cont);
    TEST_ASSERT_EQUAL(HTTPD_ERR_WOULD_BLOCK, err);
    TEST_ASSERT_EQUAL(1, cont_callback_count);

    // Data call
    const char* data = "Test data for continuation";
    err = handler(NULL, data, strlen(data), &cont);
    TEST_ASSERT_EQUAL(HTTPD_ERR_WOULD_BLOCK, err);
    TEST_ASSERT_EQUAL(2, cont_callback_count);
    TEST_ASSERT_EQUAL(strlen(data), cont_bytes_received);
}

// Test WOULD_BLOCK error code
static void test_would_block_error_code(void)
{
    TEST_ASSERT_EQUAL(-13, HTTPD_ERR_WOULD_BLOCK);

    // Verify it's distinguishable from other errors
    TEST_ASSERT_NOT_EQUAL(HTTPD_OK, HTTPD_ERR_WOULD_BLOCK);
    TEST_ASSERT_NOT_EQUAL(HTTPD_ERR_IO, HTTPD_ERR_WOULD_BLOCK);
    TEST_ASSERT_NOT_EQUAL(HTTPD_ERR_TIMEOUT, HTTPD_ERR_WOULD_BLOCK);
}

// Test continuation with data accumulation
static void test_continuation_data_accumulation(void)
{
    reset_continuation_state();
    httpd_req_continuation_t cont = {0};
    httpd_continuation_t handler = test_continuation_handler;

    // Initial call
    httpd_err_t err = handler(NULL, NULL, 0, &cont);
    TEST_ASSERT_EQUAL(HTTPD_ERR_WOULD_BLOCK, err);

    // Send data in chunks until complete
    const char chunk[] = "0123456789";  // 10 bytes per chunk
    int chunks_sent = 0;

    while (!cont_completed && chunks_sent < 20) {
        err = handler(NULL, chunk, sizeof(chunk) - 1, &cont);
        chunks_sent++;

        if (cont_completed) {
            TEST_ASSERT_EQUAL(HTTPD_OK, err);
            break;
        } else {
            TEST_ASSERT_EQUAL(HTTPD_ERR_WOULD_BLOCK, err);
        }
    }

    TEST_ASSERT_TRUE(cont_completed);
    TEST_ASSERT_GREATER_OR_EQUAL(100, cont_bytes_received);

    ESP_LOGI(TAG, "Continuation completed after %d chunks, %zu bytes",
             chunks_sent, cont_bytes_received);
}

// Test continuation phase tracking
static void test_continuation_phase_tracking(void)
{
    httpd_req_continuation_t cont = {0};

    // Phase 0: Initial
    TEST_ASSERT_EQUAL(0, cont.phase);

    // Phase 1: Reading header
    cont.phase = 1;
    cont.expected_bytes = 32;
    TEST_ASSERT_EQUAL(1, cont.phase);

    // Phase 2: Reading body
    cont.phase = 2;
    cont.expected_bytes = 1024;
    cont.received_bytes = 0;
    TEST_ASSERT_EQUAL(2, cont.phase);

    // Simulate receiving
    cont.received_bytes = 512;
    TEST_ASSERT_EQUAL(512, cont.received_bytes);
    TEST_ASSERT_TRUE(cont.received_bytes < cont.expected_bytes);

    cont.received_bytes = 1024;
    TEST_ASSERT_TRUE(cont.received_bytes >= cont.expected_bytes);
}

// Test continuation with server context
static void test_continuation_with_server(void)
{
    start_test_server();
    reset_continuation_state();

    TEST_ASSERT_NOT_NULL(g_server);

    // Verify connection pool has continuation flag support
    connection_pool_t* pool = &g_server->connection_pool;
    TEST_ASSERT_NOT_NULL(pool);

    int idx = 0;
    connection_t* conn = connection_get(pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    // Initialize for test
    conn->fd = -1;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->continuation = 1;
    conn->content_length = 1000;
    conn->bytes_received = 0;

    // Verify state
    TEST_ASSERT_EQUAL(1, conn->continuation);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn->state);

    // Clear
    conn->continuation = 0;
    conn->state = CONN_STATE_FREE;

    stop_test_server();
}

// ============================================================================
// INTEGRATION TESTS
// ============================================================================

// Test that deferred and continuation modes are mutually exclusive
static void test_deferred_continuation_exclusive(void)
{
    connection_t conn;
    memset(&conn, 0, sizeof(conn));

    // Set deferred
    conn.deferred = 1;
    TEST_ASSERT_EQUAL(1, conn.deferred);
    TEST_ASSERT_EQUAL(0, conn.continuation);

    // Clear and set continuation
    conn.deferred = 0;
    conn.continuation = 1;
    TEST_ASSERT_EQUAL(0, conn.deferred);
    TEST_ASSERT_EQUAL(1, conn.continuation);

    // Both should not be set simultaneously in normal operation
    // (the API prevents this, but we test the flag independence)
}

// ============================================================================
// PARTIAL CHUNK FAILURE TESTS
// ============================================================================

// Test that a partial chunk send marks the connection as CLOSED.
// When the chunk header is sent/queued but the chunk data send fails,
// the stream is irrecoverably corrupted, so the connection must be closed.
static void test_partial_chunk_send_marks_conn_closed(void)
{
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    // Get connection and send buffer from the server's pool
    int idx = 0;
    connection_t* conn = connection_get(&g_server->connection_pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
    TEST_ASSERT_NOT_NULL(bufs);
    send_buffer_t* sb = bufs[idx];
    TEST_ASSERT_NOT_NULL(sb);

    // Initialize connection for test
    conn->fd = -1;  // Invalid fd so drain/send fails
    conn->state = CONN_STATE_HTTP_BODY;
    conn->pool_index = idx;

    // Allocate and seed the send buffer so send_nonblocking takes the queue path
    // (skips send() syscall, queues directly)
    send_buffer_init(sb);
    TEST_ASSERT_TRUE(send_buffer_alloc(sb));
    uint8_t dummy = 0;
    send_buffer_queue(sb, &dummy, 1);

    // Set up request with body_started=true so it skips header finalization
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req._internal = conn;
    req.status_code = 200;
    req.body_started = true;
    req.headers_sent = true;
    sb->chunked = 1;

    // Create a large chunk that will overflow the send buffer.
    // Buffer is SEND_BUFFER_SIZE bytes, 1 byte seeded, space = SIZE-2.
    // Chunk header queues OK, but data exceeds remaining space.
    // send_nonblocking will fail trying to queue the data portion,
    // triggering the partial-chunk-on-wire protection.
    size_t chunk_len = SEND_BUFFER_SIZE - 6;
    char* large_chunk = malloc(chunk_len);
    TEST_ASSERT_NOT_NULL(large_chunk);
    memset(large_chunk, 'A', chunk_len);

    httpd_err_t err = httpd_resp_send_chunk(&req, large_chunk, (ssize_t)chunk_len);

    // Should return IO error
    TEST_ASSERT_EQUAL(HTTPD_ERR_IO, err);

    // Connection must be marked CLOSED because partial chunk data was queued
    // (header was queued successfully before data send failed)
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);

    ESP_LOGI(TAG, "Partial chunk send correctly marked connection as CLOSED");

    free(large_chunk);

    // Clean up - reset connection state
    conn->state = CONN_STATE_FREE;
    conn->fd = -1;
    send_buffer_free(sb);
    send_buffer_init(sb);

    stop_test_server();
}

// Test that when the first send (chunk header) fails, conn state is NOT changed.
// Nothing is on the wire yet, so the stream is not corrupted.
static void test_chunk_header_fail_no_close(void)
{
    start_test_server();
    TEST_ASSERT_NOT_NULL(g_server);

    int idx = 0;
    connection_t* conn = connection_get(&g_server->connection_pool, idx);
    TEST_ASSERT_NOT_NULL(conn);

    send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
    send_buffer_t* sb = bufs[idx];
    TEST_ASSERT_NOT_NULL(sb);

    conn->fd = -1;
    conn->state = CONN_STATE_HTTP_BODY;
    conn->pool_index = idx;

    // Allocate send buffer and fill it almost completely so even the
    // chunk header (5-6 bytes) cannot be queued.
    send_buffer_init(sb);
    TEST_ASSERT_TRUE(send_buffer_alloc(sb));

    // Fill buffer to capacity (leave only 1 byte of space, which is the
    // minimum gap to distinguish full from empty).
    size_t space = send_buffer_space(sb);
    char* filler = malloc(space);
    TEST_ASSERT_NOT_NULL(filler);
    memset(filler, 'X', space);
    send_buffer_queue(sb, filler, space);
    free(filler);

    // Verify buffer is full
    TEST_ASSERT_EQUAL(0, send_buffer_space(sb));

    // Set up request
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req._internal = conn;
    req.status_code = 200;
    req.body_started = true;
    req.headers_sent = true;
    sb->chunked = 1;

    // Try to send a large chunk - the header send should fail
    // because the buffer is full and drain fails (fd=-1).
    char data[300];
    memset(data, 'B', sizeof(data));

    httpd_err_t err = httpd_resp_send_chunk(&req, data, sizeof(data));
    TEST_ASSERT_EQUAL(HTTPD_ERR_IO, err);

    // Connection should NOT be marked CLOSED - nothing was sent/queued
    TEST_ASSERT_NOT_EQUAL(CONN_STATE_CLOSED, conn->state);

    ESP_LOGI(TAG, "Chunk header failure correctly left connection state unchanged");

    // Clean up
    conn->state = CONN_STATE_FREE;
    conn->fd = -1;
    send_buffer_free(sb);
    send_buffer_init(sb);

    stop_test_server();
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void test_nonblocking_run(void)
{
    ESP_LOGI(TAG, "Running Non-blocking I/O tests");

    // Chunked transfer encoding tests
    ESP_LOGI(TAG, "Chunked Transfer Encoding tests...");
    RUN_TEST(test_chunk_frame_atomic_small);
    RUN_TEST(test_chunk_frame_format);
    RUN_TEST(test_final_chunk_format);
    RUN_TEST(test_chunk_frame_large);
    RUN_TEST(test_multiple_chunks_sequential);

    // Continuation API tests
    ESP_LOGI(TAG, "Continuation API tests...");
    RUN_TEST(test_continue_null_params);
    RUN_TEST(test_is_continuation_null);
    RUN_TEST(test_connection_continuation_flag);
    RUN_TEST(test_continuation_state_struct);
    RUN_TEST(test_continuation_handler_type);
    RUN_TEST(test_would_block_error_code);
    RUN_TEST(test_continuation_data_accumulation);
    RUN_TEST(test_continuation_phase_tracking);
    RUN_TEST(test_continuation_with_server);

    // Partial chunk failure tests
    ESP_LOGI(TAG, "Partial chunk failure tests...");
    RUN_TEST(test_partial_chunk_send_marks_conn_closed);
    RUN_TEST(test_chunk_header_fail_no_close);

    // Integration tests
    ESP_LOGI(TAG, "Integration tests...");
    RUN_TEST(test_deferred_continuation_exclusive);

    ESP_LOGI(TAG, "Non-blocking tests completed");
}
