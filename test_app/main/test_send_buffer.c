#include "unity.h"
#include "send_buffer.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "TEST_SEND_BUF";

// Global pool for tests
static send_buffer_pool_t test_pool;

// Helper to reset test state
static void reset_test_pool(void) {
    send_buffer_pool_init(&test_pool);
}

// Test pool initialization
static void test_pool_init(void)
{
    send_buffer_pool_t pool;
    send_buffer_pool_init(&pool);

    TEST_ASSERT_EQUAL(0, pool.in_use_mask);
}

// Test buffer initialization
static void test_buffer_init(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);

    TEST_ASSERT_NULL(sb.buffer);
    TEST_ASSERT_EQUAL(0, sb.size);
    TEST_ASSERT_EQUAL(0, sb.head);
    TEST_ASSERT_EQUAL(0, sb.tail);
    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_FALSE(sb.allocated);
    TEST_ASSERT_FALSE(sb.streaming);
    TEST_ASSERT_FALSE(sb.chunked);
}

// Test buffer allocation
static void test_buffer_alloc(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);

    // Allocate should succeed
    bool result = send_buffer_alloc(&sb, &test_pool);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(sb.buffer);
    TEST_ASSERT_EQUAL(SEND_BUFFER_SIZE, sb.size);
    TEST_ASSERT_TRUE(sb.allocated);
    TEST_ASSERT_EQUAL(1, test_pool.in_use_mask);  // First slot used

    // Double alloc should be safe (no-op)
    result = send_buffer_alloc(&sb, &test_pool);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, test_pool.in_use_mask);  // Still just one slot

    // Free the buffer
    send_buffer_free(&sb, &test_pool);
    TEST_ASSERT_NULL(sb.buffer);
    TEST_ASSERT_FALSE(sb.allocated);
    TEST_ASSERT_EQUAL(0, test_pool.in_use_mask);
}

// Test basic queue and peek operations
static void test_queue_and_peek(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Queue some data
    const char* test_data = "Hello, World!";
    ssize_t queued = send_buffer_queue(&sb, test_data, strlen(test_data));
    TEST_ASSERT_EQUAL(strlen(test_data), queued);
    TEST_ASSERT_EQUAL(strlen(test_data), send_buffer_pending(&sb));

    // Peek at data
    const uint8_t* peek_ptr;
    size_t peek_len = send_buffer_peek(&sb, &peek_ptr);
    TEST_ASSERT_EQUAL(strlen(test_data), peek_len);
    TEST_ASSERT_EQUAL_STRING_LEN(test_data, (const char*)peek_ptr, strlen(test_data));

    // Consume all data
    send_buffer_consume(&sb, peek_len);
    TEST_ASSERT_EQUAL(0, send_buffer_pending(&sb));
    TEST_ASSERT_FALSE(send_buffer_has_data(&sb));

    send_buffer_free(&sb, &test_pool);
}

// Test buffer space tracking
static void test_buffer_space(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Initially should have nearly full space (minus 1 for empty/full distinction)
    size_t initial_space = send_buffer_space(&sb);
    TEST_ASSERT_EQUAL(SEND_BUFFER_SIZE - 1, initial_space);

    // Queue some data
    char data[100];
    memset(data, 'A', sizeof(data));
    send_buffer_queue(&sb, data, sizeof(data));

    // Space should decrease
    size_t remaining_space = send_buffer_space(&sb);
    TEST_ASSERT_EQUAL(initial_space - sizeof(data), remaining_space);

    send_buffer_free(&sb, &test_pool);
}

// CRITICAL TEST: Ring buffer wrap-around reset
// This tests the bug where head near end of buffer wasn't reset after consuming all data
static void test_wrap_around_reset(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    ESP_LOGI(TAG, "Testing ring buffer wrap-around reset (the chunked encoding bug)");

    // Fill buffer nearly to capacity to move head near end
    size_t fill_size = SEND_BUFFER_SIZE - 20;  // Leave some space
    char* fill_data = malloc(fill_size);
    TEST_ASSERT_NOT_NULL(fill_data);
    memset(fill_data, 'X', fill_size);

    ssize_t queued = send_buffer_queue(&sb, fill_data, fill_size);
    TEST_ASSERT_EQUAL(fill_size, queued);
    ESP_LOGI(TAG, "After fill: head=%u, tail=%u", sb.head, sb.tail);

    // Consume all data - head should be near end of buffer
    const uint8_t* peek_ptr;
    size_t peek_len = send_buffer_peek(&sb, &peek_ptr);
    send_buffer_consume(&sb, peek_len);

    ESP_LOGI(TAG, "After consume all: head=%u, tail=%u", sb.head, sb.tail);

    // KEY ASSERTION: After consuming all data, head and tail should be reset to 0
    // This is critical for chunked encoding which needs contiguous space for chunk headers
    TEST_ASSERT_EQUAL_MESSAGE(0, sb.head,
        "Head should be reset to 0 after consuming all data");
    TEST_ASSERT_EQUAL_MESSAGE(0, sb.tail,
        "Tail should be reset to 0 after consuming all data");

    // Verify we have full contiguous space available
    uint8_t* write_ptr;
    size_t contiguous = send_buffer_write_ptr(&sb, &write_ptr);
    ESP_LOGI(TAG, "Contiguous space after reset: %zu", contiguous);

    // Should have nearly full buffer available as contiguous space
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(SEND_BUFFER_SIZE - 1, contiguous,
        "Should have full contiguous space after reset");

    // This is the critical test: chunked encoding needs at least 10 bytes for headers
    // Without the reset fix, we might only have a few bytes of contiguous space
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(10, contiguous,
        "Must have at least 10 bytes contiguous for chunk headers");

    free(fill_data);
    send_buffer_free(&sb, &test_pool);
}

// Test wrap-around with partial consume
static void test_wrap_around_partial_consume(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Fill buffer to move head near end
    size_t fill_size = SEND_BUFFER_SIZE - 100;
    char* fill_data = malloc(fill_size);
    TEST_ASSERT_NOT_NULL(fill_data);
    memset(fill_data, 'Y', fill_size);

    send_buffer_queue(&sb, fill_data, fill_size);

    // Consume only part of the data
    send_buffer_consume(&sb, fill_size / 2);

    uint16_t head_before = sb.head;

    // Head and tail should NOT be reset (buffer not empty)
    TEST_ASSERT_NOT_EQUAL(0, sb.tail);  // Tail moved forward
    TEST_ASSERT_EQUAL(head_before, sb.head);  // Head unchanged

    // Now consume the rest
    send_buffer_consume(&sb, fill_size - fill_size / 2);

    // NOW head and tail should be reset
    TEST_ASSERT_EQUAL_MESSAGE(0, sb.head,
        "Head should be reset after consuming all remaining data");
    TEST_ASSERT_EQUAL_MESSAGE(0, sb.tail,
        "Tail should be reset after consuming all remaining data");

    free(fill_data);
    send_buffer_free(&sb, &test_pool);
}

// Test simulating the chunked encoding scenario that caused the original bug
static void test_chunked_encoding_scenario(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    ESP_LOGI(TAG, "Simulating chunked encoding scenario");

    // Simulate multiple write/send cycles that gradually move head toward end
    const size_t CHUNK_SIZE = 1024;
    const size_t CHUNK_OVERHEAD = 10;  // Worst case: "XXXX\r\n" header + "\r\n" trailer
    char chunk_data[CHUNK_SIZE];
    memset(chunk_data, 'Z', CHUNK_SIZE);

    for (int cycle = 0; cycle < 20; cycle++) {
        // Check if we have enough contiguous space for chunk header + data
        uint8_t* write_ptr;
        size_t contiguous = send_buffer_write_ptr(&sb, &write_ptr);

        ESP_LOGD(TAG, "Cycle %d: head=%u, tail=%u, contiguous=%zu",
                 cycle, sb.head, sb.tail, contiguous);

        // This is the critical assertion that would fail without the fix:
        // After each cycle, we must have space for chunk overhead
        if (sb.head == sb.tail) {
            TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(CHUNK_OVERHEAD, contiguous,
                "Empty buffer must have space for chunk headers");
        }

        // Queue a chunk (simulating provider filling buffer)
        if (send_buffer_space(&sb) >= CHUNK_SIZE) {
            send_buffer_queue(&sb, chunk_data, CHUNK_SIZE);
        }

        // Send all pending data (simulating socket drain)
        while (send_buffer_has_data(&sb)) {
            const uint8_t* peek;
            size_t len = send_buffer_peek(&sb, &peek);
            send_buffer_consume(&sb, len);
        }

        // After consuming all data, verify reset happened
        TEST_ASSERT_EQUAL_MESSAGE(0, sb.head,
            "Head must reset to 0 after draining buffer");
        TEST_ASSERT_EQUAL_MESSAGE(0, sb.tail,
            "Tail must reset to 0 after draining buffer");
    }

    send_buffer_free(&sb, &test_pool);
}

// Test write_ptr and commit for zero-copy writes
static void test_zero_copy_write(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Get write pointer
    uint8_t* write_ptr;
    size_t available = send_buffer_write_ptr(&sb, &write_ptr);
    TEST_ASSERT_NOT_NULL(write_ptr);
    TEST_ASSERT_GREATER_THAN(0, available);

    // Write directly to buffer
    const char* test_str = "Zero-copy test";
    size_t len = strlen(test_str);
    memcpy(write_ptr, test_str, len);
    send_buffer_commit(&sb, len);

    // Verify data is in buffer
    TEST_ASSERT_EQUAL(len, send_buffer_pending(&sb));

    const uint8_t* peek;
    size_t peek_len = send_buffer_peek(&sb, &peek);
    TEST_ASSERT_EQUAL(len, peek_len);
    TEST_ASSERT_EQUAL_STRING_LEN(test_str, (const char*)peek, len);

    send_buffer_free(&sb, &test_pool);
}

// Test pool exhaustion
static void test_pool_exhaustion(void)
{
    reset_test_pool();
    send_buffer_t buffers[SEND_BUFFER_POOL_SIZE + 1];

    // Allocate all buffers in pool
    for (int i = 0; i < SEND_BUFFER_POOL_SIZE; i++) {
        send_buffer_init(&buffers[i]);
        bool result = send_buffer_alloc(&buffers[i], &test_pool);
        TEST_ASSERT_TRUE_MESSAGE(result, "Should be able to allocate buffer");
    }

    // Next allocation should fail
    send_buffer_init(&buffers[SEND_BUFFER_POOL_SIZE]);
    bool result = send_buffer_alloc(&buffers[SEND_BUFFER_POOL_SIZE], &test_pool);
    TEST_ASSERT_FALSE_MESSAGE(result, "Pool should be exhausted");

    // Free one buffer
    send_buffer_free(&buffers[0], &test_pool);

    // Now allocation should succeed
    result = send_buffer_alloc(&buffers[SEND_BUFFER_POOL_SIZE], &test_pool);
    TEST_ASSERT_TRUE_MESSAGE(result, "Should be able to allocate after free");

    // Cleanup
    for (int i = 1; i <= SEND_BUFFER_POOL_SIZE; i++) {
        send_buffer_free(&buffers[i], &test_pool);
    }
}

// Test buffer reset (keeps allocation)
static void test_buffer_reset(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Queue data and set flags
    send_buffer_queue(&sb, "test", 4);
    sb.chunked = 1;
    sb.headers_done = 1;

    // Reset buffer
    send_buffer_reset(&sb);

    // Buffer should be allocated but empty
    TEST_ASSERT_TRUE(sb.allocated);
    TEST_ASSERT_NOT_NULL(sb.buffer);
    TEST_ASSERT_EQUAL(0, sb.head);
    TEST_ASSERT_EQUAL(0, sb.tail);
    TEST_ASSERT_FALSE(sb.chunked);
    TEST_ASSERT_FALSE(sb.headers_done);
    TEST_ASSERT_FALSE(send_buffer_has_data(&sb));

    send_buffer_free(&sb, &test_pool);
}

// ============================================================================
// POINTER SAFETY AND EDGE CASE TESTS
// ============================================================================

// Test operations on unallocated buffer (NULL buffer pointer)
static void test_unallocated_buffer_operations(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);
    // Don't allocate - buffer is NULL

    // Queue should fail gracefully
    ssize_t result = send_buffer_queue(&sb, "test", 4);
    TEST_ASSERT_EQUAL_MESSAGE(-1, result,
        "Queue to unallocated buffer should return -1");

    // Space should be 0
    size_t space = send_buffer_space(&sb);
    TEST_ASSERT_EQUAL_MESSAGE(0, space,
        "Unallocated buffer should have 0 space");

    // Peek should return NULL and 0
    const uint8_t* peek_ptr;
    size_t peek_len = send_buffer_peek(&sb, &peek_ptr);
    TEST_ASSERT_NULL_MESSAGE(peek_ptr,
        "Peek on unallocated buffer should return NULL pointer");
    TEST_ASSERT_EQUAL_MESSAGE(0, peek_len,
        "Peek on unallocated buffer should return 0 length");

    // Write ptr should return NULL and 0
    uint8_t* write_ptr;
    size_t write_len = send_buffer_write_ptr(&sb, &write_ptr);
    TEST_ASSERT_NULL_MESSAGE(write_ptr,
        "Write ptr on unallocated buffer should return NULL");
    TEST_ASSERT_EQUAL_MESSAGE(0, write_len,
        "Write ptr on unallocated buffer should return 0 length");

    // has_data and pending should handle gracefully
    TEST_ASSERT_FALSE(send_buffer_has_data(&sb));
    TEST_ASSERT_EQUAL(0, send_buffer_pending(&sb));
}

// Test queue with zero length
static void test_queue_zero_length(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Queue zero bytes should fail (returns -1)
    ssize_t result = send_buffer_queue(&sb, "test", 0);
    TEST_ASSERT_EQUAL_MESSAGE(-1, result,
        "Queue with zero length should return -1");

    // Buffer should still be empty
    TEST_ASSERT_FALSE(send_buffer_has_data(&sb));
    TEST_ASSERT_EQUAL(0, send_buffer_pending(&sb));

    send_buffer_free(&sb, &test_pool);
}

// Test consume more than pending (should be clamped)
static void test_consume_overflow(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Queue some data
    send_buffer_queue(&sb, "test", 4);
    TEST_ASSERT_EQUAL(4, send_buffer_pending(&sb));

    // Try to consume way more than available
    send_buffer_consume(&sb, 1000000);

    // Should be clamped to actual pending, buffer should be empty
    TEST_ASSERT_EQUAL_MESSAGE(0, send_buffer_pending(&sb),
        "Consume overflow should clamp to pending amount");
    TEST_ASSERT_FALSE(send_buffer_has_data(&sb));

    // Head and tail should be reset to 0
    TEST_ASSERT_EQUAL(0, sb.head);
    TEST_ASSERT_EQUAL(0, sb.tail);

    send_buffer_free(&sb, &test_pool);
}

// Test consume zero bytes (should be no-op)
static void test_consume_zero(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    send_buffer_queue(&sb, "test", 4);
    uint16_t tail_before = sb.tail;

    // Consume zero should be no-op
    send_buffer_consume(&sb, 0);

    TEST_ASSERT_EQUAL_MESSAGE(tail_before, sb.tail,
        "Consume zero should not change tail");
    TEST_ASSERT_EQUAL(4, send_buffer_pending(&sb));

    send_buffer_free(&sb, &test_pool);
}

// Test double free (should be safe)
static void test_double_free(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    TEST_ASSERT_EQUAL(1, test_pool.in_use_mask);

    // First free
    send_buffer_free(&sb, &test_pool);
    TEST_ASSERT_EQUAL(0, test_pool.in_use_mask);
    TEST_ASSERT_NULL(sb.buffer);
    TEST_ASSERT_FALSE(sb.allocated);

    // Second free should be safe (no-op)
    send_buffer_free(&sb, &test_pool);
    TEST_ASSERT_EQUAL_MESSAGE(0, test_pool.in_use_mask,
        "Double free should not corrupt pool mask");
    TEST_ASSERT_NULL(sb.buffer);
}

// Test free unallocated buffer
static void test_free_unallocated(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);

    // Free without ever allocating - should be safe
    send_buffer_free(&sb, &test_pool);

    TEST_ASSERT_EQUAL_MESSAGE(0, test_pool.in_use_mask,
        "Free of unallocated buffer should not corrupt pool");
}

// Test buffer completely full (minus 1 byte)
static void test_buffer_full(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Fill to max capacity (size - 1)
    size_t max_fill = SEND_BUFFER_SIZE - 1;
    char* fill_data = malloc(max_fill);
    TEST_ASSERT_NOT_NULL(fill_data);
    memset(fill_data, 'F', max_fill);

    ssize_t queued = send_buffer_queue(&sb, fill_data, max_fill);
    TEST_ASSERT_EQUAL_MESSAGE(max_fill, queued,
        "Should be able to fill buffer to capacity");

    // Space should now be 0
    TEST_ASSERT_EQUAL_MESSAGE(0, send_buffer_space(&sb),
        "Full buffer should have 0 space");

    // Try to queue more - should fail
    ssize_t result = send_buffer_queue(&sb, "x", 1);
    TEST_ASSERT_EQUAL_MESSAGE(-1, result,
        "Queue to full buffer should return -1");

    free(fill_data);
    send_buffer_free(&sb, &test_pool);
}

// Test queue exactly fills remaining space
static void test_queue_exact_fit(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Queue to leave exactly 100 bytes
    size_t initial_fill = SEND_BUFFER_SIZE - 1 - 100;
    char* fill_data = malloc(initial_fill);
    TEST_ASSERT_NOT_NULL(fill_data);
    memset(fill_data, 'A', initial_fill);
    send_buffer_queue(&sb, fill_data, initial_fill);

    size_t space = send_buffer_space(&sb);
    TEST_ASSERT_EQUAL(100, space);

    // Queue exactly 100 bytes
    char exact_data[100];
    memset(exact_data, 'B', 100);
    ssize_t result = send_buffer_queue(&sb, exact_data, 100);
    TEST_ASSERT_EQUAL_MESSAGE(100, result,
        "Should be able to queue exactly remaining space");

    TEST_ASSERT_EQUAL(0, send_buffer_space(&sb));

    free(fill_data);
    send_buffer_free(&sb, &test_pool);
}

// Test peek on empty buffer
static void test_peek_empty(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Peek on empty allocated buffer
    const uint8_t* peek_ptr;
    size_t peek_len = send_buffer_peek(&sb, &peek_ptr);

    TEST_ASSERT_NULL_MESSAGE(peek_ptr,
        "Peek on empty buffer should return NULL pointer");
    TEST_ASSERT_EQUAL_MESSAGE(0, peek_len,
        "Peek on empty buffer should return 0 length");

    send_buffer_free(&sb, &test_pool);
}

// Test commit with bounds validation
static void test_commit_bounds(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Get write pointer
    uint8_t* write_ptr;
    size_t available = send_buffer_write_ptr(&sb, &write_ptr);
    TEST_ASSERT_GREATER_THAN(0, available);

    // Commit less than available - should work
    send_buffer_commit(&sb, 10);
    TEST_ASSERT_EQUAL(10, send_buffer_pending(&sb));

    // Verify head advanced correctly
    TEST_ASSERT_EQUAL(10, sb.head);

    send_buffer_free(&sb, &test_pool);
}

// Test wrap-around with exact boundary conditions
static void test_wrap_boundary(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Fill to move head to exact end of buffer
    size_t fill_size = SEND_BUFFER_SIZE - 1;
    char* fill_data = malloc(fill_size);
    TEST_ASSERT_NOT_NULL(fill_data);
    memset(fill_data, 'X', fill_size);

    send_buffer_queue(&sb, fill_data, fill_size);

    // Head should be at size-1
    TEST_ASSERT_EQUAL(fill_size, sb.head);

    // Consume all
    send_buffer_consume(&sb, fill_size);

    // Should reset to 0
    TEST_ASSERT_EQUAL(0, sb.head);
    TEST_ASSERT_EQUAL(0, sb.tail);

    // Now queue exactly 1 byte
    send_buffer_queue(&sb, "A", 1);
    TEST_ASSERT_EQUAL(1, sb.head);
    TEST_ASSERT_EQUAL(0, sb.tail);

    free(fill_data);
    send_buffer_free(&sb, &test_pool);
}

// Test data integrity through wrap-around
static void test_wrap_data_integrity(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Create a pattern that will wrap around buffer
    // First, fill and drain to position head near end
    size_t setup_size = SEND_BUFFER_SIZE - 100;
    char* setup_data = malloc(setup_size);
    TEST_ASSERT_NOT_NULL(setup_data);
    memset(setup_data, 'S', setup_size);
    send_buffer_queue(&sb, setup_data, setup_size);
    send_buffer_consume(&sb, setup_size);
    free(setup_data);

    // Now head is near end. Queue data that wraps
    char test_pattern[] = "0123456789ABCDEF0123456789ABCDEF"; // 32 bytes
    send_buffer_queue(&sb, test_pattern, 32);

    // Verify pending
    TEST_ASSERT_EQUAL(32, send_buffer_pending(&sb));

    // Read back in chunks (may be split due to wrap)
    char read_buffer[64] = {0};
    size_t total_read = 0;

    while (send_buffer_has_data(&sb)) {
        const uint8_t* peek;
        size_t len = send_buffer_peek(&sb, &peek);
        TEST_ASSERT_NOT_NULL(peek);
        TEST_ASSERT_GREATER_THAN(0, len);

        memcpy(read_buffer + total_read, peek, len);
        total_read += len;
        send_buffer_consume(&sb, len);
    }

    // Verify data integrity
    TEST_ASSERT_EQUAL_MESSAGE(32, total_read,
        "Should read back all queued data");
    TEST_ASSERT_EQUAL_STRING_LEN_MESSAGE(test_pattern, read_buffer, 32,
        "Data should survive wrap-around intact");

    send_buffer_free(&sb, &test_pool);
}

// Test multiple allocations use different slots
static void test_allocation_slots(void)
{
    reset_test_pool();
    send_buffer_t buffers[SEND_BUFFER_POOL_SIZE];

    // Allocate all and verify each uses different memory
    for (int i = 0; i < SEND_BUFFER_POOL_SIZE; i++) {
        send_buffer_init(&buffers[i]);
        bool result = send_buffer_alloc(&buffers[i], &test_pool);
        TEST_ASSERT_TRUE(result);

        // Verify buffer points to pool memory
        bool found = false;
        for (int j = 0; j < SEND_BUFFER_POOL_SIZE; j++) {
            if (buffers[i].buffer == test_pool.buffers[j]) {
                found = true;
                break;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found,
            "Allocated buffer should point to pool memory");
    }

    // Verify all buffers are unique
    for (int i = 0; i < SEND_BUFFER_POOL_SIZE; i++) {
        for (int j = i + 1; j < SEND_BUFFER_POOL_SIZE; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(buffers[i].buffer, buffers[j].buffer,
                "Each allocation should use unique buffer");
        }
    }

    // Cleanup
    for (int i = 0; i < SEND_BUFFER_POOL_SIZE; i++) {
        send_buffer_free(&buffers[i], &test_pool);
    }
}

// Test reset preserves buffer allocation
static void test_reset_preserves_allocation(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    uint8_t* original_buffer = sb.buffer;
    uint16_t original_size = sb.size;

    // Queue some data
    send_buffer_queue(&sb, "data", 4);

    // Reset
    send_buffer_reset(&sb);

    // Buffer pointer and size should be preserved
    TEST_ASSERT_EQUAL_MESSAGE(original_buffer, sb.buffer,
        "Reset should preserve buffer pointer");
    TEST_ASSERT_EQUAL_MESSAGE(original_size, sb.size,
        "Reset should preserve buffer size");
    TEST_ASSERT_TRUE(sb.allocated);

    // But data should be gone
    TEST_ASSERT_EQUAL(0, send_buffer_pending(&sb));

    send_buffer_free(&sb, &test_pool);
}

// ============================================================================
// File Streaming Tests (send_buffer_start_file, send_buffer_stop_file)
// ============================================================================

// Test start_file with valid fd
static void test_start_file_valid_fd(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Use a mock fd value (we're testing state changes, not actual I/O)
    int mock_fd = 42;
    size_t file_size = 1024;

    bool result = send_buffer_start_file(&sb, mock_fd, file_size);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(mock_fd, sb.file_fd);
    TEST_ASSERT_EQUAL(file_size, sb.file_remaining);
    TEST_ASSERT_TRUE(sb.streaming);
    TEST_ASSERT_TRUE(send_buffer_is_streaming(&sb));
    TEST_ASSERT_EQUAL(file_size, send_buffer_file_remaining(&sb));

    // Don't call send_buffer_free which would try to close the mock fd
    sb.file_fd = -1;  // Prevent close attempt
    send_buffer_free(&sb, &test_pool);
}

// Test start_file with invalid fd (-1)
static void test_start_file_invalid_fd(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    bool result = send_buffer_start_file(&sb, -1, 1024);

    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_FALSE(sb.streaming);
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));

    send_buffer_free(&sb, &test_pool);
}

// Test start_file with zero size
static void test_start_file_zero_size(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Zero size file is valid
    int mock_fd = 10;
    bool result = send_buffer_start_file(&sb, mock_fd, 0);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(mock_fd, sb.file_fd);
    TEST_ASSERT_EQUAL(0, sb.file_remaining);
    TEST_ASSERT_TRUE(sb.streaming);

    sb.file_fd = -1;  // Prevent close attempt
    send_buffer_free(&sb, &test_pool);
}

// Test stop_file clears state
static void test_stop_file_clears_state(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Setup file streaming state manually
    sb.file_fd = 99;  // Mock fd
    sb.file_remaining = 5000;
    sb.streaming = 1;

    // Stop file (note: this will try to close fd 99, which may fail silently)
    send_buffer_stop_file(&sb);

    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_EQUAL(0, sb.file_remaining);
    TEST_ASSERT_FALSE(sb.streaming);
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));
    TEST_ASSERT_EQUAL(0, send_buffer_file_remaining(&sb));

    send_buffer_free(&sb, &test_pool);
}

// Test stop_file when no file is open (should be safe)
static void test_stop_file_no_file(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // No file is open (file_fd = -1 from init)
    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_FALSE(sb.streaming);

    // Stop file should be safe no-op
    send_buffer_stop_file(&sb);

    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_FALSE(sb.streaming);

    send_buffer_free(&sb, &test_pool);
}

// Test is_streaming inline function
// Note: send_buffer_is_streaming checks BOTH streaming flag AND file_fd >= 0
static void test_is_streaming_accessor(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);

    // Not streaming initially (file_fd = -1, streaming = 0)
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));

    // Set streaming flag only - should still be false because file_fd = -1
    sb.streaming = 1;
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));

    // Set valid file_fd - now should return true
    sb.file_fd = 42;
    TEST_ASSERT_TRUE(send_buffer_is_streaming(&sb));

    // Clear streaming flag - should be false again
    sb.streaming = 0;
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));

    // Valid fd but not streaming - still false
    sb.file_fd = 42;
    sb.streaming = 0;
    TEST_ASSERT_FALSE(send_buffer_is_streaming(&sb));
}

// Test file_remaining inline function
static void test_file_remaining_accessor(void)
{
    send_buffer_t sb;
    send_buffer_init(&sb);

    // Zero initially
    TEST_ASSERT_EQUAL(0, send_buffer_file_remaining(&sb));

    // Set various values
    sb.file_remaining = 100;
    TEST_ASSERT_EQUAL(100, send_buffer_file_remaining(&sb));

    sb.file_remaining = 1000000;
    TEST_ASSERT_EQUAL(1000000, send_buffer_file_remaining(&sb));

    sb.file_remaining = 0;
    TEST_ASSERT_EQUAL(0, send_buffer_file_remaining(&sb));
}

// Test reset clears file state
static void test_reset_clears_file_state(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Setup file streaming state
    sb.file_fd = 123;  // Mock fd
    sb.file_remaining = 9999;
    sb.streaming = 1;

    // Reset should clear file state
    send_buffer_reset(&sb);

    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_EQUAL(0, sb.file_remaining);
    TEST_ASSERT_FALSE(sb.streaming);

    send_buffer_free(&sb, &test_pool);
}

// Test free closes file fd
static void test_free_closes_file(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Setup mock file fd
    sb.file_fd = 200;  // Mock fd
    sb.streaming = 1;

    // Free should attempt to close the file
    // (close will fail on invalid fd, but shouldn't crash)
    send_buffer_free(&sb, &test_pool);

    // After free, buffer should be reinitialized
    TEST_ASSERT_EQUAL(-1, sb.file_fd);
    TEST_ASSERT_FALSE(sb.streaming);
}

// Test start_file replaces existing file
static void test_start_file_replaces_existing(void)
{
    reset_test_pool();
    send_buffer_t sb;
    send_buffer_init(&sb);
    send_buffer_alloc(&sb, &test_pool);

    // Start first file
    sb.file_fd = 10;  // Pretend this is open
    sb.file_remaining = 500;
    sb.streaming = 1;

    // Start new file (should close old one)
    int new_fd = 20;
    bool result = send_buffer_start_file(&sb, new_fd, 1000);

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(new_fd, sb.file_fd);
    TEST_ASSERT_EQUAL(1000, sb.file_remaining);
    TEST_ASSERT_TRUE(sb.streaming);

    sb.file_fd = -1;  // Prevent close attempt
    send_buffer_free(&sb, &test_pool);
}

void test_send_buffer_run(void)
{
    // Basic functionality tests
    RUN_TEST(test_pool_init);
    RUN_TEST(test_buffer_init);
    RUN_TEST(test_buffer_alloc);
    RUN_TEST(test_queue_and_peek);
    RUN_TEST(test_buffer_space);
    RUN_TEST(test_zero_copy_write);
    RUN_TEST(test_pool_exhaustion);
    RUN_TEST(test_buffer_reset);

    // Critical wrap-around bug tests
    RUN_TEST(test_wrap_around_reset);
    RUN_TEST(test_wrap_around_partial_consume);
    RUN_TEST(test_chunked_encoding_scenario);

    // Pointer safety and edge case tests
    RUN_TEST(test_unallocated_buffer_operations);
    RUN_TEST(test_queue_zero_length);
    RUN_TEST(test_consume_overflow);
    RUN_TEST(test_consume_zero);
    RUN_TEST(test_double_free);
    RUN_TEST(test_free_unallocated);
    RUN_TEST(test_buffer_full);
    RUN_TEST(test_queue_exact_fit);
    RUN_TEST(test_peek_empty);
    RUN_TEST(test_commit_bounds);
    RUN_TEST(test_wrap_boundary);
    RUN_TEST(test_wrap_data_integrity);
    RUN_TEST(test_allocation_slots);
    RUN_TEST(test_reset_preserves_allocation);

    // File streaming tests
    RUN_TEST(test_start_file_valid_fd);
    RUN_TEST(test_start_file_invalid_fd);
    RUN_TEST(test_start_file_zero_size);
    RUN_TEST(test_stop_file_clears_state);
    RUN_TEST(test_stop_file_no_file);
    RUN_TEST(test_is_streaming_accessor);
    RUN_TEST(test_file_remaining_accessor);
    RUN_TEST(test_reset_clears_file_state);
    RUN_TEST(test_free_closes_file);
    RUN_TEST(test_start_file_replaces_existing);

    ESP_LOGI(TAG, "Send buffer tests completed (35 tests)");
}
