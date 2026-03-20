#include "private/connection.h"
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "esp_log.h"

static const char TAG[] __attribute__((unused)) = "CONNECTION";

// Connection pool functions that aren't in event_loop.c
// These are simple utility functions for connection management

// Mark a connection as closed and clear its bitmasks.
// Note: This does NOT close the socket fd. The caller (typically the event loop)
// is responsible for calling shutdown()+close() on the fd before or after this call.
void connection_close(connection_pool_t* pool, connection_t* conn)
{
    if (!pool || !conn) return;

    // Use cached pool_index for O(1) lookup
    int index = conn->pool_index;

    // Mark as closed (keep active so cleanup_closed can find it)
    conn->state = CONN_STATE_CLOSED;

    // Clear write pending, WebSocket active, and read paused if set
    connection_mark_write_pending(pool, index, false);
    connection_mark_ws_inactive(pool, index);
    connection_mark_read_paused(pool, index, false);
}

void connection_cleanup_closed(connection_pool_t* pool)
{
    if (!pool) return;

    // Iterate only over active connections using bitmask
    uint32_t mask = pool->active_mask;
    connection_t* base = pool->connections;  // Cache base pointer for efficient indexing
    while (mask) {
        int i = __builtin_ctz(mask);
        uint32_t bit = 1U << i;
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = base + i;
        if (conn->state == CONN_STATE_CLOSED) {
            // Defensive: close the fd if it's still open.
            // The event loop normally closes fds inline before reaching here,
            // but guard against fd leaks if called from other code paths.
            if (conn->fd > 0) {
                ESP_LOGW(TAG, "Connection [%d] fd %d still open during cleanup, closing", i, conn->fd);
                shutdown(conn->fd, SHUT_RDWR);
                close(conn->fd);
            }

            // Selective field reset instead of expensive memset
            conn->fd = -1;
            conn->state = CONN_STATE_FREE;
            conn->user_ctx = NULL;

            // Clear bitmasks using pre-isolated bit
            uint32_t clear_bit = ~bit;
            pool->active_mask &= clear_bit;
            pool->write_pending_mask &= clear_bit;
            pool->ws_active_mask &= clear_bit;
            pool->read_paused_mask &= clear_bit;
        }
    }
}

connection_t* connection_accept(connection_pool_t* pool, int listen_fd)
{
    if (!pool) return NULL;

    // Find first free slot using O(1) bit manipulation
    uint32_t free_mask = ~pool->active_mask;
    if (free_mask == 0) {
        // No free slots
        return NULL;
    }

    // Get index of first free slot
    int i = __builtin_ctz(free_mask);
    if (i >= MAX_CONNECTIONS) {
        return NULL;
    }

    connection_t* conn = &pool->connections[i];

    // Initialize connection using memset (faster than 20+ individual assignments)
    memset(conn, 0, sizeof(connection_t));
    conn->fd = -1;                  // Only non-zero initial value
    conn->state = CONN_STATE_NEW;   // CONN_STATE_NEW = 1
    conn->pool_index = i;           // Cache index for O(1) lookup

    // Mark as active
    connection_mark_active(pool, i);

    return conn;
}

connection_t* connection_find(connection_pool_t* pool, int fd)
{
    if (__builtin_expect(!pool, 0)) return NULL;

    // Iterate only over active connections using bitmask
    uint32_t mask = pool->active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask ^= (1U << i);  // Clear specific bit using XOR

        // Direct array access for better optimization
        if (__builtin_expect(pool->connections[i].fd == fd, 0)) {
            return &pool->connections[i];
        }
    }

    return NULL;
}

connection_t* connection_get(connection_pool_t* pool, int index)
{
    if (!pool) {
        ESP_LOGE(TAG, "connection_get: pool is NULL");
        return NULL;
    }
    if (index < 0 || index >= MAX_CONNECTIONS) {
        ESP_LOGE(TAG, "connection_get: invalid index %d (MAX=%d)", index, MAX_CONNECTIONS);
        return NULL;
    }
    return &pool->connections[index];
}

int connection_get_index(connection_pool_t* pool, connection_t* conn)
{
    if (!pool || !conn) return -1;

    int index = (conn - pool->connections);
    if (index < 0 || index >= MAX_CONNECTIONS) {
        return -1;
    }
    return index;
}

void connection_pool_init(connection_pool_t* pool)
{
    if (!pool) return;

    memset(pool, 0, sizeof(connection_pool_t));
    // Initialize fd to -1; state is already CONN_STATE_FREE (0) from memset
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        pool->connections[i].fd = -1;
    }
}

int connection_count_active(connection_pool_t* pool)
{
    if (!pool) return 0;

    // Use popcount for O(1) count
    return __builtin_popcount(pool->active_mask);
}