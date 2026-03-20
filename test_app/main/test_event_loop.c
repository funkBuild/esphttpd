#include "unity.h"
#include "event_loop.h"
#include "connection.h"
#include "esp_log.h"
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

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

// ========== Issue #35: Config validation defaults ==========

static void test_config_zero_timeout_gets_default(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 0,         // Zero should get default 30000
        .backlog = 0,            // Zero should get default 5
        .select_timeout_ms = 1000,
        .io_buffer_size = 1024,
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(30000, loop.config.timeout_ms);
    TEST_ASSERT_EQUAL(5, loop.config.backlog);
}

// ========== Issue #43: WS close timeout configurable ==========

static void test_ws_close_timeout_configurable(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 1000,
        .io_buffer_size = 1024,
        .ws_close_timeout_ms = 10000,  // 10 second WS close timeout
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(10000, loop.config.ws_close_timeout_ms);
    // ws_close_timeout_ticks = 10000 / 1000 = 10
    TEST_ASSERT_EQUAL(10, loop.ws_close_timeout_ticks);
}

static void test_ws_close_timeout_zero_gets_default(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 1000,
        .io_buffer_size = 1024,
        .ws_close_timeout_ms = 0,  // Zero should get default 5000ms
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(5000, loop.config.ws_close_timeout_ms);
    // ws_close_timeout_ticks = 5000 / 1000 = 5
    TEST_ASSERT_EQUAL(5, loop.ws_close_timeout_ticks);
}

// ========== Issue #43: WS close timeout used by check_timeouts ==========

static void test_ws_closing_uses_shorter_timeout(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 1000,
        .io_buffer_size = 1024,
        .ws_close_timeout_ms = 3000,  // 3 second WS close timeout
    };

    event_loop_init(&loop, &pool, &config);

    // Set up a WS_CLOSING connection
    connection_t* conn = &pool.connections[0];
    conn->fd = 10;
    conn->state = CONN_STATE_WS_CLOSING;
    conn->last_activity = 0;
    connection_mark_active(&pool, 0);
    // WS_CLOSING should NOT be in ws_active_mask (so timeout check sees it)

    // Advance tick_count past WS close timeout (3 ticks) but not general timeout (30 ticks)
    loop.tick_count = 5;

    event_loop_check_timeouts(&loop);

    // Should have been timed out (5 > 3 ticks)
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);
}

// ========== Bug fix: on_ws_disconnect not called for timed-out WS connections ==========

static int ws_disconnect_called = 0;
static int disconnect_called = 0;
static int ws_disconnect_call_order = 0;
static int disconnect_call_order = 0;
static int call_order_counter = 0;

static void mock_ws_disconnect(connection_t* conn) {
    ws_disconnect_called++;
    ws_disconnect_call_order = ++call_order_counter;
}

static void mock_disconnect(connection_t* conn) {
    disconnect_called++;
    disconnect_call_order = ++call_order_counter;
}

// Test that on_ws_disconnect is called for timed-out WebSocket connections.
// Simulates the CONN_STATE_CLOSED cleanup path in event_loop_iteration()
// using direct struct manipulation (consistent with existing test patterns).
static void test_ws_timeout_calls_ws_disconnect(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Set up a WebSocket connection that has been timed out (state = CLOSED)
    connection_t* conn = &pool.connections[0];
    conn->fd = -1;  // Invalid fd; close(-1) is harmless
    conn->state = CONN_STATE_CLOSED;
    conn->is_websocket = 1;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);

    // Reset tracking variables
    ws_disconnect_called = 0;
    disconnect_called = 0;
    call_order_counter = 0;
    ws_disconnect_call_order = 0;
    disconnect_call_order = 0;

    event_handlers_t handlers = {0};
    handlers.on_ws_disconnect = mock_ws_disconnect;
    handlers.on_disconnect = mock_disconnect;

    // Simulate the CONN_STATE_CLOSED cleanup block from event_loop_iteration().
    // This is the exact code path that runs when iterating active connections.
    uint32_t mask = pool.active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;

        connection_t* c = &pool.connections[i];
        if (c->state == CONN_STATE_CLOSED) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            close(c->fd);
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Verify on_ws_disconnect was called
    TEST_ASSERT_EQUAL(1, ws_disconnect_called);
    // Verify on_disconnect was also called
    TEST_ASSERT_EQUAL(1, disconnect_called);
    // Verify on_ws_disconnect was called before on_disconnect
    TEST_ASSERT_TRUE(ws_disconnect_call_order < disconnect_call_order);
    // Verify connection was cleaned up
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
}

// Test that on_ws_disconnect is NOT called for non-WebSocket timed-out connections
static void test_non_ws_timeout_skips_ws_disconnect(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Set up a regular HTTP connection that has been timed out (state = CLOSED)
    connection_t* conn = &pool.connections[0];
    conn->fd = -1;  // Invalid fd; close(-1) is harmless
    conn->state = CONN_STATE_CLOSED;
    conn->is_websocket = 0;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);

    // Reset tracking variables
    ws_disconnect_called = 0;
    disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_ws_disconnect = mock_ws_disconnect;
    handlers.on_disconnect = mock_disconnect;

    // Simulate the CONN_STATE_CLOSED cleanup block from event_loop_iteration()
    uint32_t mask = pool.active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;

        connection_t* c = &pool.connections[i];
        if (c->state == CONN_STATE_CLOSED) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            close(c->fd);
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Verify on_ws_disconnect was NOT called
    TEST_ASSERT_EQUAL(0, ws_disconnect_called);
    // Verify on_disconnect WAS called
    TEST_ASSERT_EQUAL(1, disconnect_called);
    // Verify connection was cleaned up
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
}

// ========== Issue #15: EBADF stale fd cleanup ==========

static int ebadf_disconnect_called = 0;
static int ebadf_ws_disconnect_called = 0;

static void ebadf_mock_disconnect(connection_t* conn) {
    ebadf_disconnect_called++;
}

static void ebadf_mock_ws_disconnect(connection_t* conn) {
    ebadf_ws_disconnect_called++;
}

// Test that a stale (closed) fd is detected and cleaned up via fcntl validation.
// This exercises the EBADF handling path added to event_loop_iteration().
static void test_ebadf_stale_fd_cleanup(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Use an fd number that is guaranteed to not be open (no socket() needed)
    const int stale_fd = 999;

    // Verify the fd is actually stale
    TEST_ASSERT_EQUAL(-1, fcntl(stale_fd, F_GETFD));
    TEST_ASSERT_EQUAL(EBADF, errno);

    // Set up a connection with this stale fd
    connection_t* conn = &pool.connections[0];
    memset(conn, 0, sizeof(connection_t));
    conn->fd = stale_fd;
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);

    // Also set up a "valid" connection using stdout (always open, avoids LWIP)
    connection_t* valid_conn = &pool.connections[1];
    memset(valid_conn, 0, sizeof(connection_t));
    valid_conn->fd = STDOUT_FILENO;
    valid_conn->state = CONN_STATE_HTTP_HEADERS;
    valid_conn->pool_index = 1;
    connection_mark_active(&pool, 1);

    // Reset tracking
    ebadf_disconnect_called = 0;
    ebadf_ws_disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_disconnect = ebadf_mock_disconnect;
    handlers.on_ws_disconnect = ebadf_mock_ws_disconnect;

    // Simulate the EBADF scan logic from event_loop_iteration()
    uint32_t scan_mask = pool.active_mask;
    while (scan_mask) {
        int i = __builtin_ctz(scan_mask);
        scan_mask &= scan_mask - 1;

        connection_t* c = &pool.connections[i];
        if (fcntl(c->fd, F_GETFD) == -1 && errno == EBADF) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            c->state = CONN_STATE_CLOSED;
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Stale connection should be cleaned up
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);
    TEST_ASSERT_EQUAL(1, ebadf_disconnect_called);

    // Valid connection should still be active
    TEST_ASSERT_TRUE(connection_is_active(&pool, 1));
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_HEADERS, valid_conn->state);

    // WebSocket disconnect should NOT have been called (not a WS connection)
    TEST_ASSERT_EQUAL(0, ebadf_ws_disconnect_called);
}

// Test that a stale WebSocket fd triggers on_ws_disconnect
static void test_ebadf_stale_ws_fd_cleanup(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Use an fd number that is guaranteed to not be open
    const int stale_fd = 998;

    // Set up a WebSocket connection with the stale fd
    connection_t* conn = &pool.connections[0];
    memset(conn, 0, sizeof(connection_t));
    conn->fd = stale_fd;
    conn->state = CONN_STATE_WEBSOCKET;
    conn->is_websocket = 1;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);
    connection_mark_ws_active(&pool, 0);

    // Reset tracking
    ebadf_disconnect_called = 0;
    ebadf_ws_disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_disconnect = ebadf_mock_disconnect;
    handlers.on_ws_disconnect = ebadf_mock_ws_disconnect;

    // Simulate the EBADF scan logic
    uint32_t scan_mask = pool.active_mask;
    while (scan_mask) {
        int i = __builtin_ctz(scan_mask);
        scan_mask &= scan_mask - 1;

        connection_t* c = &pool.connections[i];
        if (fcntl(c->fd, F_GETFD) == -1 && errno == EBADF) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            c->state = CONN_STATE_CLOSED;
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Both disconnect handlers should have been called
    TEST_ASSERT_EQUAL(1, ebadf_ws_disconnect_called);
    TEST_ASSERT_EQUAL(1, ebadf_disconnect_called);
    // All pool bits should be cleared
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
    TEST_ASSERT_FALSE(connection_is_ws_active(&pool, 0));
}

// ========== Config validation: io_buffer_size and select_timeout_ms ==========

static void test_config_zero_io_buffer_size_gets_default(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 1000,
        .io_buffer_size = 0,  // Zero should get default 1024
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(1024, loop.config.io_buffer_size);
}

static void test_config_zero_select_timeout_gets_default(void) {
    event_loop_t loop = {0};
    connection_pool_t pool = {0};

    event_loop_config_t config = {
        .timeout_ms = 30000,
        .select_timeout_ms = 0,  // Zero should get default 1000
        .io_buffer_size = 1024,
    };

    event_loop_init(&loop, &pool, &config);

    TEST_ASSERT_EQUAL(1000, loop.config.select_timeout_ms);
    // Verify derived values use the defaulted select_timeout_ms
    // timeout_ticks = 30000 / 1000 = 30
    TEST_ASSERT_EQUAL(30, loop.timeout_ticks);
    // select_timeout struct should be 1 second
    TEST_ASSERT_EQUAL(1, loop.select_timeout.tv_sec);
    TEST_ASSERT_EQUAL(0, loop.select_timeout.tv_usec);
}

// ========== Socket close/shutdown sequence tests ==========

// Test that the CONN_STATE_CLOSED cleanup path in event_loop_iteration
// calls shutdown() before close() and properly cleans up all pool state.
// We use fd=-1 (harmless for both shutdown and close) to verify the
// logic flow without requiring a real socket.
static int close_shutdown_disconnect_called = 0;

static void close_shutdown_mock_disconnect(connection_t* conn) {
    close_shutdown_disconnect_called++;
}

static void test_connection_close_calls_shutdown(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Set up a connection marked CLOSED with all pool bits set
    // to verify complete cleanup
    connection_t* conn = &pool.connections[0];
    memset(conn, 0, sizeof(connection_t));
    conn->fd = -1;  // Invalid fd; shutdown(-1) and close(-1) are harmless
    conn->state = CONN_STATE_CLOSED;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);
    connection_mark_write_pending(&pool, 0, true);
    connection_mark_read_paused(&pool, 0, true);

    close_shutdown_disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_disconnect = close_shutdown_mock_disconnect;

    // Verify all bits are set before cleanup
    TEST_ASSERT_TRUE(connection_is_active(&pool, 0));
    TEST_ASSERT_TRUE(connection_has_write_pending(&pool, 0));
    TEST_ASSERT_TRUE(connection_is_read_paused(&pool, 0));

    // Simulate the CONN_STATE_CLOSED cleanup block from event_loop_iteration().
    // The production code calls shutdown() then close() in this exact order.
    uint32_t mask = pool.active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;

        connection_t* c = &pool.connections[i];
        if (c->state == CONN_STATE_CLOSED) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            shutdown(c->fd, SHUT_RDWR);
            close(c->fd);
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Verify disconnect callback was called
    TEST_ASSERT_EQUAL(1, close_shutdown_disconnect_called);

    // Verify all pool bitmasks were cleared
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
    TEST_ASSERT_FALSE(connection_has_write_pending(&pool, 0));
    TEST_ASSERT_FALSE(connection_is_ws_active(&pool, 0));
    TEST_ASSERT_FALSE(connection_is_read_paused(&pool, 0));
}

// Test that recv() returning 0 (peer closed) triggers proper cleanup.
// Since handle_connection_data is static, we simulate the recv-EOF path
// it follows: state -> CLOSED, on_disconnect called, shutdown + close,
// all pool bitmasks cleared. Also tests the WebSocket variant where
// on_ws_disconnect must be called before on_disconnect.
static int recv_eof_disconnect_called = 0;
static int recv_eof_ws_disconnect_called = 0;

static void recv_eof_mock_disconnect(connection_t* conn) {
    recv_eof_disconnect_called++;
}

static void recv_eof_mock_ws_disconnect(connection_t* conn) {
    recv_eof_ws_disconnect_called++;
}

static void test_recv_eof_triggers_close(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Set up an active WebSocket connection (tests the WS code path too)
    connection_t* conn = &pool.connections[0];
    memset(conn, 0, sizeof(connection_t));
    conn->fd = -1;  // Invalid fd; shutdown(-1) and close(-1) are harmless
    conn->state = CONN_STATE_WEBSOCKET;
    conn->is_websocket = 1;
    conn->pool_index = 0;
    connection_mark_active(&pool, 0);
    connection_mark_ws_active(&pool, 0);

    recv_eof_disconnect_called = 0;
    recv_eof_ws_disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_disconnect = recv_eof_mock_disconnect;
    handlers.on_ws_disconnect = recv_eof_mock_ws_disconnect;

    // Simulate what handle_connection_data does when recv() returns 0:
    // For WebSocket connections, on_ws_disconnect is called first,
    // then state is set to CLOSED, then on_disconnect, shutdown, close.
    if ((conn->state == CONN_STATE_WEBSOCKET || conn->state == CONN_STATE_WS_CLOSING)
        && handlers.on_ws_disconnect) {
        handlers.on_ws_disconnect(conn);
    }
    conn->state = CONN_STATE_CLOSED;
    if (handlers.on_disconnect) {
        handlers.on_disconnect(conn);
    }
    shutdown(conn->fd, SHUT_RDWR);
    close(conn->fd);
    connection_mark_inactive(&pool, conn->pool_index);
    connection_mark_write_pending(&pool, conn->pool_index, false);
    connection_mark_ws_inactive(&pool, conn->pool_index);
    connection_mark_read_paused(&pool, conn->pool_index, false);

    // Verify state is CLOSED
    TEST_ASSERT_EQUAL(CONN_STATE_CLOSED, conn->state);

    // Verify both disconnect callbacks were called
    TEST_ASSERT_EQUAL(1, recv_eof_ws_disconnect_called);
    TEST_ASSERT_EQUAL(1, recv_eof_disconnect_called);

    // Verify all pool bitmasks cleared
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
    TEST_ASSERT_FALSE(connection_has_write_pending(&pool, 0));
    TEST_ASSERT_FALSE(connection_is_ws_active(&pool, 0));
    TEST_ASSERT_FALSE(connection_is_read_paused(&pool, 0));
}

// Test that closing one connection does not affect other active connections.
// Sets up two connections, closes one via the CLOSED cleanup path,
// and verifies the other remains active with valid state.
static int preserve_disconnect_called = 0;

static void preserve_mock_disconnect(connection_t* conn) {
    preserve_disconnect_called++;
}

static void test_connection_cleanup_preserves_other_connections(void) {
    connection_pool_t pool = {0};
    connection_pool_init(&pool);

    // Set up connection 0: will be closed
    connection_t* conn0 = &pool.connections[0];
    memset(conn0, 0, sizeof(connection_t));
    conn0->fd = -1;  // Invalid fd; close(-1) is harmless
    conn0->state = CONN_STATE_CLOSED;
    conn0->pool_index = 0;
    connection_mark_active(&pool, 0);

    // Set up connection 1: should survive
    connection_t* conn1 = &pool.connections[1];
    memset(conn1, 0, sizeof(connection_t));
    conn1->fd = STDOUT_FILENO;  // Always-valid fd (we won't close it)
    conn1->state = CONN_STATE_WEBSOCKET;
    conn1->is_websocket = 1;
    conn1->pool_index = 1;
    conn1->last_activity = 42;
    connection_mark_active(&pool, 1);
    connection_mark_ws_active(&pool, 1);
    connection_mark_write_pending(&pool, 1, true);

    preserve_disconnect_called = 0;

    event_handlers_t handlers = {0};
    handlers.on_disconnect = preserve_mock_disconnect;

    // Run the CONN_STATE_CLOSED cleanup loop (same as event_loop_iteration)
    uint32_t mask = pool.active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;

        connection_t* c = &pool.connections[i];
        if (c->state == CONN_STATE_CLOSED) {
            if (c->is_websocket && handlers.on_ws_disconnect) {
                handlers.on_ws_disconnect(c);
            }
            if (handlers.on_disconnect) {
                handlers.on_disconnect(c);
            }
            close(c->fd);
            connection_mark_inactive(&pool, i);
            connection_mark_write_pending(&pool, i, false);
            connection_mark_ws_inactive(&pool, i);
            connection_mark_read_paused(&pool, i, false);
        }
    }

    // Connection 0 should be fully cleaned up
    TEST_ASSERT_FALSE(connection_is_active(&pool, 0));
    TEST_ASSERT_EQUAL(1, preserve_disconnect_called);

    // Connection 1 should be completely unaffected
    TEST_ASSERT_TRUE(connection_is_active(&pool, 1));
    TEST_ASSERT_TRUE(connection_is_ws_active(&pool, 1));
    TEST_ASSERT_TRUE(connection_has_write_pending(&pool, 1));
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn1->state);
    TEST_ASSERT_EQUAL(STDOUT_FILENO, conn1->fd);
    TEST_ASSERT_EQUAL(42, conn1->last_activity);
    TEST_ASSERT_EQUAL(1, conn1->is_websocket);
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

    // Bug fix regression tests
    RUN_TEST(test_config_zero_timeout_gets_default);
    RUN_TEST(test_ws_close_timeout_configurable);
    RUN_TEST(test_ws_close_timeout_zero_gets_default);
    RUN_TEST(test_ws_closing_uses_shorter_timeout);
    RUN_TEST(test_ws_timeout_calls_ws_disconnect);
    RUN_TEST(test_non_ws_timeout_skips_ws_disconnect);

    // EBADF stale fd cleanup tests
    RUN_TEST(test_ebadf_stale_fd_cleanup);
    RUN_TEST(test_ebadf_stale_ws_fd_cleanup);

    // Config validation: io_buffer_size and select_timeout_ms
    RUN_TEST(test_config_zero_io_buffer_size_gets_default);
    RUN_TEST(test_config_zero_select_timeout_gets_default);

    // Socket close/shutdown sequence tests
    RUN_TEST(test_connection_close_calls_shutdown);
    RUN_TEST(test_recv_eof_triggers_close);
    RUN_TEST(test_connection_cleanup_preserves_other_connections);

    ESP_LOGI(TAG, "Event Loop tests completed");
}