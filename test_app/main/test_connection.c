#include "unity.h"
#include "esphttpd.h"  // For public WS_OP_ constants
#include "connection.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

static const char* TAG = "TEST_CONNECTION";

// Test connection pool initialization
static void test_connection_pool_init(void)
{
    connection_pool_t pool;
    connection_pool_init(&pool);

    TEST_ASSERT_EQUAL(0, pool.active_mask);
    TEST_ASSERT_EQUAL(0, pool.write_pending_mask);

    // All connections should be inactive
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        TEST_ASSERT_FALSE(connection_is_active(&pool, i));
        TEST_ASSERT_FALSE(connection_has_write_pending(&pool, i));
    }

    TEST_ASSERT_EQUAL(0, connection_count_active(&pool));
}

// Test marking connections active/inactive
static void test_connection_active_management(void)
{
    connection_pool_t pool;
    connection_pool_init(&pool);

    // Mark some connections active
    connection_mark_active(&pool, 0);
    connection_mark_active(&pool, 5);
    connection_mark_active(&pool, 31);

    TEST_ASSERT_TRUE(connection_is_active(&pool, 0));
    TEST_ASSERT_TRUE(connection_is_active(&pool, 5));
    TEST_ASSERT_TRUE(connection_is_active(&pool, 31));
    TEST_ASSERT_FALSE(connection_is_active(&pool, 1));
    TEST_ASSERT_FALSE(connection_is_active(&pool, 30));

    TEST_ASSERT_EQUAL(3, connection_count_active(&pool));

    // Mark one inactive
    connection_mark_inactive(&pool, 5);
    TEST_ASSERT_FALSE(connection_is_active(&pool, 5));
    TEST_ASSERT_EQUAL(2, connection_count_active(&pool));

    // Test boundary conditions
    connection_mark_active(&pool, 0);  // Already active
    TEST_ASSERT_EQUAL(2, connection_count_active(&pool));

    connection_mark_inactive(&pool, 10); // Already inactive
    TEST_ASSERT_EQUAL(2, connection_count_active(&pool));
}

// Test write pending management
static void test_write_pending_management(void)
{
    connection_pool_t pool;
    connection_pool_init(&pool);

    // Mark some connections with pending writes
    connection_mark_write_pending(&pool, 3, true);
    connection_mark_write_pending(&pool, 7, true);

    TEST_ASSERT_TRUE(connection_has_write_pending(&pool, 3));
    TEST_ASSERT_TRUE(connection_has_write_pending(&pool, 7));
    TEST_ASSERT_FALSE(connection_has_write_pending(&pool, 0));

    // Clear pending write
    connection_mark_write_pending(&pool, 3, false);
    TEST_ASSERT_FALSE(connection_has_write_pending(&pool, 3));
    TEST_ASSERT_TRUE(connection_has_write_pending(&pool, 7));
}

// Test connection state transitions
static void test_connection_states(void)
{
    connection_t conn = {0};

    // Initial state
    TEST_ASSERT_EQUAL(CONN_STATE_FREE, conn.state);

    // State transitions
    conn.state = CONN_STATE_NEW;
    TEST_ASSERT_EQUAL(CONN_STATE_NEW, conn.state);

    conn.state = CONN_STATE_HTTP_HEADERS;
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_HEADERS, conn.state);

    conn.state = CONN_STATE_HTTP_BODY;
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn.state);

    conn.state = CONN_STATE_WEBSOCKET;
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn.state);

    conn.state = CONN_STATE_CLOSING;
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSING, conn.state);

    conn.state = CONN_STATE_CLOSED;
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn.state);
}

// Test HTTP method storage
static void test_http_methods(void)
{
    connection_t conn = {0};

    // Test all methods fit in 3 bits
    conn.method = HTTP_GET;
    TEST_ASSERT_EQUAL(HTTP_GET, conn.method);

    conn.method = HTTP_POST;
    TEST_ASSERT_EQUAL(HTTP_POST, conn.method);

    conn.method = HTTP_PUT;
    TEST_ASSERT_EQUAL(HTTP_PUT, conn.method);

    conn.method = HTTP_DELETE;
    TEST_ASSERT_EQUAL(HTTP_DELETE, conn.method);

    conn.method = HTTP_HEAD;
    TEST_ASSERT_EQUAL(HTTP_HEAD, conn.method);

    conn.method = HTTP_OPTIONS;
    TEST_ASSERT_EQUAL(HTTP_OPTIONS, conn.method);

    conn.method = HTTP_PATCH;
    TEST_ASSERT_EQUAL(HTTP_PATCH, conn.method);

    conn.method = HTTP_ANY;
    TEST_ASSERT_EQUAL(HTTP_ANY, conn.method);
}

// Test WebSocket state fields
static void test_websocket_fields(void)
{
    connection_t conn = {0};

    // Test WebSocket flags
    conn.is_websocket = 1;
    TEST_ASSERT_TRUE(conn.is_websocket);

    conn.ws_fin = 1;
    TEST_ASSERT_TRUE(conn.ws_fin);

    conn.ws_masked = 1;
    TEST_ASSERT_TRUE(conn.ws_masked);

    // Test opcode storage (4 bits)
    conn.ws_opcode = WS_OPCODE_TEXT;
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);

    conn.ws_opcode = WS_OPCODE_BINARY;
    TEST_ASSERT_EQUAL(WS_OPCODE_BINARY, conn.ws_opcode);

    conn.ws_opcode = WS_OPCODE_CLOSE;
    TEST_ASSERT_EQUAL(WS_OPCODE_CLOSE, conn.ws_opcode);

    conn.ws_opcode = WS_OPCODE_PING;
    TEST_ASSERT_EQUAL(WS_OPCODE_PING, conn.ws_opcode);

    conn.ws_opcode = WS_OPCODE_PONG;
    TEST_ASSERT_EQUAL(WS_OPCODE_PONG, conn.ws_opcode);

    // Test WebSocket payload tracking
    conn.ws_payload_len = 1234;
    TEST_ASSERT_EQUAL(1234, conn.ws_payload_len);

    conn.ws_payload_read = 567;
    TEST_ASSERT_EQUAL(567, conn.ws_payload_read);

    conn.ws_mask_key = 0x12345678;
    TEST_ASSERT_EQUAL(0x12345678, conn.ws_mask_key);
}

// Test connection content length
static void test_content_length(void)
{
    connection_t conn = {0};

    // Test various content lengths
    conn.content_length = 0;
    TEST_ASSERT_EQUAL(0, conn.content_length);

    conn.content_length = 1024;
    TEST_ASSERT_EQUAL(1024, conn.content_length);

    conn.content_length = 65535;
    TEST_ASSERT_EQUAL(65535, conn.content_length);

    // Test large values (16MB+) - now supported with uint32_t
    conn.content_length = 16777216; // 16MB
    TEST_ASSERT_EQUAL(16777216, conn.content_length);

    conn.content_length = 104857600; // 100MB
    TEST_ASSERT_EQUAL(104857600, conn.content_length);

    conn.content_length = UINT32_MAX; // Max uint32_t (~4GB)
    TEST_ASSERT_EQUAL(UINT32_MAX, conn.content_length);

    // Test bytes received tracking
    conn.bytes_received = 0;
    TEST_ASSERT_EQUAL(0, conn.bytes_received);

    conn.bytes_received = 512;
    TEST_ASSERT_EQUAL(512, conn.bytes_received);

    conn.bytes_received = 65535;
    TEST_ASSERT_EQUAL(65535, conn.bytes_received);
}

// Test connection finding functions
static void test_connection_finding(void)
{
    connection_pool_t pool;
    connection_pool_init(&pool);

    // Set up some connections
    pool.connections[5].fd = 100;
    connection_mark_active(&pool, 5);

    pool.connections[10].fd = 200;
    connection_mark_active(&pool, 10);

    // Test connection_get
    connection_t* conn = connection_get(&pool, 5);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL(100, conn->fd);

    conn = connection_get(&pool, 10);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL(200, conn->fd);

    // Test inactive connection - should still return the connection
    conn = connection_get(&pool, 3);
    TEST_ASSERT_NOT_NULL(conn);  // Should return connection even if inactive

    // Test out of bounds
    conn = connection_get(&pool, 32);
    TEST_ASSERT_NULL(conn);
    conn = connection_get(&pool, -1);
    TEST_ASSERT_NULL(conn);

    // Test connection_find
    conn = connection_find(&pool, 100);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL(100, conn->fd);

    conn = connection_find(&pool, 200);
    TEST_ASSERT_NOT_NULL(conn);
    TEST_ASSERT_EQUAL(200, conn->fd);

    // Test not found
    conn = connection_find(&pool, 999);
    TEST_ASSERT_NULL(conn);

    // Test connection_get_index
    conn = &pool.connections[5];
    int index = connection_get_index(&pool, conn);
    TEST_ASSERT_EQUAL(5, index);

    conn = &pool.connections[10];
    index = connection_get_index(&pool, conn);
    TEST_ASSERT_EQUAL(10, index);

    // Test invalid pointer
    connection_t dummy;
    index = connection_get_index(&pool, &dummy);
    TEST_ASSERT_EQUAL(-1, index);

    index = connection_get_index(&pool, NULL);
    TEST_ASSERT_EQUAL(-1, index);
}

// Test connection URL tracking
static void test_url_tracking(void)
{
    connection_t conn = {0};

    // Test URL offset and length
    conn.url_offset = 0;
    conn.url_len = 10;
    TEST_ASSERT_EQUAL(0, conn.url_offset);
    TEST_ASSERT_EQUAL(10, conn.url_len);

    conn.url_offset = 1024;
    conn.url_len = 255; // Max uint8_t
    TEST_ASSERT_EQUAL(1024, conn.url_offset);
    TEST_ASSERT_EQUAL(255, conn.url_len);
}

// Test connection timing
static void test_connection_timing(void)
{
    connection_t conn = {0};

    // Test last activity tracking
    conn.last_activity = 0;
    TEST_ASSERT_EQUAL(0, conn.last_activity);

    conn.last_activity = 1000000;
    TEST_ASSERT_EQUAL(1000000, conn.last_activity);

    conn.last_activity = 0xFFFFFFFF; // Max uint32_t
    TEST_ASSERT_EQUAL(0xFFFFFFFF, conn.last_activity);
}

// Test all connections in pool
static void test_full_connection_pool(void)
{
    connection_pool_t pool;
    connection_pool_init(&pool);

    // Fill the pool
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_mark_active(&pool, i);
        pool.connections[i].fd = 100 + i;
    }

    TEST_ASSERT_EQUAL(MAX_CONNECTIONS, connection_count_active(&pool));

    // Verify all are active
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        TEST_ASSERT_TRUE(connection_is_active(&pool, i));
        TEST_ASSERT_EQUAL(100 + i, pool.connections[i].fd);
    }

    // Clear half the connections
    for (int i = 0; i < MAX_CONNECTIONS / 2; i++) {
        connection_mark_inactive(&pool, i);
    }

    TEST_ASSERT_EQUAL(MAX_CONNECTIONS / 2, connection_count_active(&pool));
}

// Test structure sizes (verify packing)
static void test_structure_sizes(void)
{
    // Connection should be compact
    size_t conn_size = sizeof(connection_t);
    ESP_LOGI(TAG, "connection_t size: %u bytes", conn_size);
    TEST_ASSERT_LESS_OR_EQUAL(40, conn_size); // Should be around 32-36 bytes

    // Pool size
    size_t pool_size = sizeof(connection_pool_t);
    ESP_LOGI(TAG, "connection_pool_t size: %u bytes", pool_size);
    TEST_ASSERT_LESS_OR_EQUAL(1536, pool_size); // 32 * ~36 + 8 bytes overhead
}

void test_connection_run(void)
{
    RUN_TEST(test_connection_pool_init);
    RUN_TEST(test_connection_active_management);
    RUN_TEST(test_write_pending_management);
    RUN_TEST(test_connection_states);
    RUN_TEST(test_http_methods);
    RUN_TEST(test_websocket_fields);
    RUN_TEST(test_content_length);
    RUN_TEST(test_connection_finding);
    RUN_TEST(test_url_tracking);
    RUN_TEST(test_connection_timing);
    RUN_TEST(test_full_connection_pool);
    RUN_TEST(test_structure_sizes);

    ESP_LOGI(TAG, "Connection tests completed");
}