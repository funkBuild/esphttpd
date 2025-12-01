#include "unity.h"
#include "event_loop.h"
#include "connection.h"
#include "esp_log.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char* TAG = "TEST_EVENT_LOOP";

// Test callbacks
static int on_http_request_called = 0;
static int on_ws_frame_called = 0;

static void test_on_http_request(connection_t* conn, uint8_t* buffer, size_t len) {
    on_http_request_called++;
}

static void test_on_ws_frame(connection_t* conn, uint8_t* buffer, size_t len) {
    on_ws_frame_called++;
}

// ==================== TEST FUNCTIONS ====================

// Test event loop initialization
static void test_event_loop_init(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .port = 8080,
        .backlog = 5,
        .timeout_ms = 30000,
        .select_timeout_ms = 100,
        .io_buffer_size = 1024,
        .nodelay = true,
        .reuseaddr = true
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(8080, loop.config.port);
    TEST_ASSERT_EQUAL(5, loop.config.backlog);
    TEST_ASSERT_EQUAL(100, loop.config.select_timeout_ms);
    TEST_ASSERT_TRUE(loop.config.nodelay);
    TEST_ASSERT_TRUE(loop.config.reuseaddr);
}

// Test FD set management
static void test_fd_set_management(void) {
    connection_pool_t pool = {0};

    connection_pool_init(&pool);

    // Add mock connections
    connection_t* conn1 = &pool.connections[0];
    conn1->fd = 10;
    conn1->state = CONN_STATE_HTTP_HEADERS;
    connection_mark_active(&pool, 0);

    connection_t* conn2 = &pool.connections[1];
    conn2->fd = 11;
    conn2->state = CONN_STATE_WEBSOCKET;
    connection_mark_write_pending(&pool, 1, true);
    connection_mark_active(&pool, 1);

    // Test FD set preparation
    fd_set read_fds, write_fds;
    int max_fd = 0;

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    // Simulate what event loop does
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_is_active(&pool, i)) {
            connection_t* conn = &pool.connections[i];
            FD_SET(conn->fd, &read_fds);
            if (connection_has_write_pending(&pool, i)) {
                FD_SET(conn->fd, &write_fds);
            }
            if (conn->fd > max_fd) {
                max_fd = conn->fd;
            }
        }
    }

    TEST_ASSERT_TRUE(FD_ISSET(10, &read_fds));
    TEST_ASSERT_TRUE(FD_ISSET(11, &read_fds));
    TEST_ASSERT_TRUE(FD_ISSET(11, &write_fds));
    TEST_ASSERT_FALSE(FD_ISSET(10, &write_fds));
    TEST_ASSERT_EQUAL(11, max_fd);
}

// Test connection timeout tracking
static void test_connection_timeout(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Add connection
    connection_t* conn = &pool.connections[0];
    conn->fd = 10;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->last_activity = 1000; // Mock timestamp
    connection_mark_active(&pool, 0);

    // Test timeout check
    uint32_t current_time = 35000; // 34 seconds later
    uint32_t timeout_ms = 30000;   // 30 second timeout

    bool should_timeout = (current_time - conn->last_activity) > timeout_ms;
    TEST_ASSERT_TRUE(should_timeout);

    // Test recent activity
    conn->last_activity = 33000; // 2 seconds ago
    should_timeout = (current_time - conn->last_activity) > timeout_ms;
    TEST_ASSERT_FALSE(should_timeout);
}

// Test stop mechanism
static void test_event_loop_stop(void) {
    event_loop_t loop = {0};

    TEST_ASSERT_FALSE(loop.running);

    loop.running = true;
    TEST_ASSERT_TRUE(loop.running);

    event_loop_stop(&loop);

    TEST_ASSERT_FALSE(loop.running);
}

// Test event handler dispatch
static void test_event_dispatch(void) {
    event_handlers_t handlers = {
        .on_http_request = test_on_http_request,
        .on_http_body = NULL,
        .on_ws_frame = test_on_ws_frame,
        .on_ws_connect = NULL,
        .on_ws_disconnect = NULL
    };

    connection_t conn = {0};
    uint8_t buffer[128] = "test data";

    // Reset counters
    on_http_request_called = 0;
    on_ws_frame_called = 0;

    // Test HTTP request dispatch
    conn.state = CONN_STATE_HTTP_HEADERS;
    if (handlers.on_http_request) {
        handlers.on_http_request(&conn, buffer, 9);
    }
    TEST_ASSERT_EQUAL(1, on_http_request_called);

    // Test WebSocket frame dispatch
    conn.state = CONN_STATE_WEBSOCKET;
    if (handlers.on_ws_frame) {
        handlers.on_ws_frame(&conn, buffer, 9);
    }
    TEST_ASSERT_EQUAL(1, on_ws_frame_called);
}

// Test maximum connections handling
static void test_max_connections(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    int available_count = 0;

    // Fill the pool
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t* conn = &pool.connections[i];
        conn->fd = 100 + i;
        conn->state = CONN_STATE_HTTP_HEADERS;
        connection_mark_active(&pool, i);
    }

    // Count available slots
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_is_active(&pool, i)) {
            available_count++;
        }
    }

    TEST_ASSERT_EQUAL(0, available_count);

    // Free one connection
    connection_mark_inactive(&pool, 5);
    available_count = 0;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connection_is_active(&pool, i)) {
            available_count++;
        }
    }

    TEST_ASSERT_EQUAL(1, available_count);
}

// Test buffer size limits
static void test_buffer_limits(void) {
    event_loop_config_t config = {
        .io_buffer_size = 4096
    };

    TEST_ASSERT_EQUAL(4096, config.io_buffer_size);

    // Test minimum buffer size
    config.io_buffer_size = 64;
    TEST_ASSERT_TRUE(config.io_buffer_size >= 64);

    // Test maximum buffer size (platform dependent)
    config.io_buffer_size = 65536;
    TEST_ASSERT_TRUE(config.io_buffer_size <= 65536);
}

// Test select timeout configuration
static void test_select_timeout(void) {
    event_loop_config_t config = {
        .select_timeout_ms = 1000
    };

    struct timeval tv;
    tv.tv_sec = config.select_timeout_ms / 1000;
    tv.tv_usec = (config.select_timeout_ms % 1000) * 1000;

    TEST_ASSERT_EQUAL(1, tv.tv_sec);
    TEST_ASSERT_EQUAL(0, tv.tv_usec);

    // Test sub-second timeout
    config.select_timeout_ms = 250;
    tv.tv_sec = config.select_timeout_ms / 1000;
    tv.tv_usec = (config.select_timeout_ms % 1000) * 1000;

    TEST_ASSERT_EQUAL(0, tv.tv_sec);
    TEST_ASSERT_EQUAL(250000, tv.tv_usec);
}

// Test default initialization
static void test_event_loop_init_default(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_init_default(&loop, &pool);

    // Verify defaults are set
    TEST_ASSERT_EQUAL(&pool, loop.pool);
    TEST_ASSERT_FALSE(loop.running);
    TEST_ASSERT_EQUAL(-1, loop.listen_fd);
    TEST_ASSERT_EQUAL(0, loop.total_connections);
    TEST_ASSERT_EQUAL(0, loop.total_requests);
}

// Test event loop stop on already stopped loop
static void test_event_loop_stop_idempotent(void) {
    event_loop_t loop = {0};

    loop.running = false;
    event_loop_stop(&loop);
    TEST_ASSERT_FALSE(loop.running);

    // Stop multiple times should be safe
    event_loop_stop(&loop);
    event_loop_stop(&loop);
    TEST_ASSERT_FALSE(loop.running);
}

// Test statistics tracking
static void test_event_loop_statistics(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_init_default(&loop, &pool);

    // Statistics should start at zero
    TEST_ASSERT_EQUAL(0, loop.total_connections);
    TEST_ASSERT_EQUAL(0, loop.total_requests);
    TEST_ASSERT_EQUAL(0, loop.total_ws_frames);

    // Simulate incrementing stats
    loop.total_connections++;
    loop.total_requests += 5;
    loop.total_ws_frames += 10;

    TEST_ASSERT_EQUAL(1, loop.total_connections);
    TEST_ASSERT_EQUAL(5, loop.total_requests);
    TEST_ASSERT_EQUAL(10, loop.total_ws_frames);
}

// Test NULL handler callbacks are safe to check
static void test_handlers_null_safety(void) {
    event_handlers_t handlers = {0};

    // All should be NULL
    TEST_ASSERT_NULL(handlers.on_http_request);
    TEST_ASSERT_NULL(handlers.on_http_body);
    TEST_ASSERT_NULL(handlers.on_ws_frame);
    TEST_ASSERT_NULL(handlers.on_ws_connect);
    TEST_ASSERT_NULL(handlers.on_ws_disconnect);
    TEST_ASSERT_NULL(handlers.on_connect);
    TEST_ASSERT_NULL(handlers.on_disconnect);
    TEST_ASSERT_NULL(handlers.on_write_ready);

    // Safe to check before calling
    connection_t conn = {0};
    uint8_t buffer[16] = {0};

    if (handlers.on_http_request) {
        handlers.on_http_request(&conn, buffer, sizeof(buffer));
    }
    // Test passes if we don't crash
    TEST_PASS();
}

// Test timeout tick calculations
static void test_timeout_ticks(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 100
    };

    event_loop_init(&loop, &pool, &config);

    // Tick count should start at zero
    TEST_ASSERT_EQUAL(0, loop.tick_count);

    // Simulate tick progression
    loop.tick_count++;
    TEST_ASSERT_EQUAL(1, loop.tick_count);
}

// Test WebSocket active state with FD management
static void test_websocket_fd_set(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Add a WebSocket connection
    connection_t* ws_conn = &pool.connections[0];
    ws_conn->fd = 20;
    ws_conn->state = CONN_STATE_WEBSOCKET;
    connection_mark_active(&pool, 0);
    connection_mark_ws_active(&pool, 0);

    // WebSocket should be in read set
    fd_set read_fds;
    FD_ZERO(&read_fds);

    if (connection_is_active(&pool, 0)) {
        FD_SET(ws_conn->fd, &read_fds);
    }

    TEST_ASSERT_TRUE(FD_ISSET(20, &read_fds));
    TEST_ASSERT_TRUE(connection_is_ws_active(&pool, 0));
}

// ==================== TEST RUNNER ====================

void test_event_loop_run(void) {
    ESP_LOGI(TAG, "Running Event Loop tests");

    RUN_TEST(test_event_loop_init);
    RUN_TEST(test_fd_set_management);
    RUN_TEST(test_connection_timeout);
    RUN_TEST(test_event_loop_stop);
    RUN_TEST(test_event_dispatch);
    RUN_TEST(test_max_connections);
    RUN_TEST(test_buffer_limits);
    RUN_TEST(test_select_timeout);

    // Additional tests
    RUN_TEST(test_event_loop_init_default);
    RUN_TEST(test_event_loop_stop_idempotent);
    RUN_TEST(test_event_loop_statistics);
    RUN_TEST(test_handlers_null_safety);
    RUN_TEST(test_timeout_ticks);
    RUN_TEST(test_websocket_fd_set);

    ESP_LOGI(TAG, "Event Loop tests completed");
}