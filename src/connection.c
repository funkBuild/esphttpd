#include "private/connection.h"
#include <string.h>
#include "esp_log.h"

// Connection pool functions that aren't in event_loop.c
// These are simple utility functions for connection management

void connection_close(connection_pool_t* pool, connection_t* conn)
{
    if (!pool || !conn) return;

    // Use cached pool_index for O(1) lookup
    int index = conn->pool_index;

    // Mark as closed and inactive
    conn->state = CONN_STATE_CLOSED;
    connection_mark_inactive(pool, index);

    // Clear write pending and WebSocket active if set
    connection_mark_write_pending(pool, index, false);
    connection_mark_ws_inactive(pool, index);
}

void connection_cleanup_closed(connection_pool_t* pool)
{
    if (!pool) return;

    // Iterate only over active connections using bitmask
    uint32_t mask = pool->active_mask;
    connection_t* base = pool->connections;  // Cache base pointer for efficient indexing
    while (mask) {
        // Bit isolation: extract lowest set bit directly (1 cycle vs CTZ+shift)
        uint32_t bit = mask & -mask;
        int i = __builtin_ctz(bit);  // Get index from isolated bit
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = base + i;
        if (conn->state == CONN_STATE_CLOSED) {
            // Selective field reset instead of expensive memset
            conn->fd = -1;
            conn->state = CONN_STATE_FREE;

            // Clear bitmasks using pre-isolated bit
            uint32_t clear_bit = ~bit;
            pool->active_mask &= clear_bit;
            pool->write_pending_mask &= clear_bit;
            pool->ws_active_mask &= clear_bit;
        }
    }
}

connection_t* connection_accept(connection_pool_t* pool, int listen_fd)
{
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
        ESP_LOGE("CONNECTION", "connection_get: pool is NULL");
        return NULL;
    }
    if (index < 0 || index >= MAX_CONNECTIONS) {
        ESP_LOGE("CONNECTION", "connection_get: invalid index %d (MAX=%d)", index, MAX_CONNECTIONS);
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
    // Initialize all connections as free
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        pool->connections[i].fd = -1;
        pool->connections[i].state = CONN_STATE_FREE;
    }
}

int connection_count_active(connection_pool_t* pool)
{
    if (!pool) return 0;

    // Use popcount for O(1) count
    return __builtin_popcount(pool->active_mask);
}