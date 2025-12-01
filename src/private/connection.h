#ifndef _CORE_CONNECTION_H_
#define _CORE_CONNECTION_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of concurrent connections (configurable via Kconfig)
#ifndef CONFIG_HTTPD_MAX_CONNECTIONS
#define CONFIG_HTTPD_MAX_CONNECTIONS 16
#endif
#define MAX_CONNECTIONS CONFIG_HTTPD_MAX_CONNECTIONS

// Connection states
typedef enum {
    CONN_STATE_FREE = 0,        // Connection slot is free
    CONN_STATE_NEW,             // New connection, reading request line
    CONN_STATE_HTTP_HEADERS,    // Reading HTTP headers
    CONN_STATE_HTTP_BODY,       // Reading HTTP body
    CONN_STATE_WEBSOCKET,       // WebSocket connection
    CONN_STATE_CLOSING,         // Connection is closing
    CONN_STATE_CLOSED           // Connection closed, pending cleanup
} conn_state_t;

// HTTP methods - use the public type from esphttpd.h
// Note: The http_method_t enum is defined in esphttpd.h with values:
// HTTP_GET=0, HTTP_POST=1, HTTP_PUT=2, HTTP_DELETE=3,
// HTTP_HEAD=4, HTTP_OPTIONS=5, HTTP_PATCH=6, HTTP_ANY=7
#include "esphttpd.h"

// WebSocket opcodes (internal)
typedef enum {
    WS_OPCODE_CONTINUATION = 0x0,
    WS_OPCODE_TEXT         = 0x1,
    WS_OPCODE_BINARY       = 0x2,
    WS_OPCODE_CLOSE        = 0x8,
    WS_OPCODE_PING         = 0x9,
    WS_OPCODE_PONG         = 0xA
} ws_opcode_internal_t;

// Packed connection structure - ~36 bytes per connection
typedef struct __attribute__((packed)) {
    int fd;                      // Socket file descriptor (4 bytes)

    // State and flags (2 bytes total)
    uint8_t state : 3;           // Connection state (3 bits)
    uint8_t is_websocket : 1;    // Is this a WebSocket connection (1 bit)
    uint8_t keep_alive : 1;      // HTTP keep-alive (1 bit)
    uint8_t method : 3;          // HTTP method (3 bits)

    uint8_t ws_fin : 1;          // WebSocket FIN flag (1 bit)
    uint8_t ws_masked : 1;       // WebSocket frame is masked (1 bit)
    uint8_t ws_opcode : 4;       // WebSocket opcode (4 bits)
    uint8_t ws_fragment : 1;     // Currently processing fragment (1 bit)
    uint8_t upgrade_ws : 1;      // Pending WebSocket upgrade (1 bit)

    // Deferred request handling and pool index (1 byte)
    uint8_t deferred : 1;        // Body handling deferred to callbacks (1 bit)
    uint8_t defer_paused : 1;    // Deferred receiving paused (flow control) (1 bit)
    uint8_t pool_index : 5;      // Index in connection pool (0-31) for O(1) context lookup
    uint8_t _reserved : 1;       // Reserved for future use (1 bit)

    // Parsing state (2 bytes)
    uint16_t header_bytes;       // Bytes of headers received

    // Content tracking (8 bytes) - supports uploads up to 4GB
    uint32_t content_length;     // Expected content length
    uint32_t bytes_received;     // Bytes received for current message

    // WebSocket state (8 bytes)
    uint16_t ws_payload_len;     // Current frame payload length
    uint16_t ws_payload_read;    // Payload bytes already processed
    uint32_t ws_mask_key;        // Masking key (when masked)

    // Routing (2 bytes)
    uint16_t route_id;           // Current route ID

    // URL tracking (2 bytes)
    uint16_t url_offset;         // Offset in shared URL buffer
    uint8_t url_len;             // URL length

    // Timing (4 bytes)
    uint32_t last_activity;      // Last activity timestamp (tick count)

    // Optional user context (4 bytes)
    void* user_ctx;              // User-defined context
} connection_t;

// Connection pool management
typedef struct {
    connection_t connections[MAX_CONNECTIONS];
    uint32_t active_mask;        // Bitmask of active connections
    uint32_t write_pending_mask; // Bitmask of connections with pending writes
    uint32_t ws_active_mask;     // Bitmask of active WebSocket connections (O(k) iteration)
} connection_pool_t;

// Connection management functions
void connection_pool_init(connection_pool_t* pool);
connection_t* connection_accept(connection_pool_t* pool, int listen_fd);
connection_t* connection_find(connection_pool_t* pool, int fd);
connection_t* connection_get(connection_pool_t* pool, int index);
int connection_get_index(connection_pool_t* pool, connection_t* conn);
void connection_close(connection_pool_t* pool, connection_t* conn);
void connection_cleanup_closed(connection_pool_t* pool);
int connection_count_active(connection_pool_t* pool);

// Utility functions
static inline bool connection_is_active(connection_pool_t* pool, int index) {
    return (pool->active_mask & (1U << index)) != 0;
}

static inline void connection_mark_active(connection_pool_t* pool, int index) {
    pool->active_mask |= (1U << index);
}

static inline void connection_mark_inactive(connection_pool_t* pool, int index) {
    pool->active_mask &= ~(1U << index);
}

static inline bool connection_has_write_pending(connection_pool_t* pool, int index) {
    return (pool->write_pending_mask & (1U << index)) != 0;
}

static inline void connection_mark_write_pending(connection_pool_t* pool, int index, bool pending) {
    if (pending) {
        pool->write_pending_mask |= (1U << index);
    } else {
        pool->write_pending_mask &= ~(1U << index);
    }
}

// WebSocket active tracking for O(k) broadcast iteration
static inline bool connection_is_ws_active(connection_pool_t* pool, int index) {
    return (pool->ws_active_mask & (1U << index)) != 0;
}

static inline void connection_mark_ws_active(connection_pool_t* pool, int index) {
    pool->ws_active_mask |= (1U << index);
}

static inline void connection_mark_ws_inactive(connection_pool_t* pool, int index) {
    pool->ws_active_mask &= ~(1U << index);
}

// Get count of active WebSocket connections using popcount
static inline int connection_ws_active_count(connection_pool_t* pool) {
    return __builtin_popcount(pool->ws_active_mask);
}

#ifdef __cplusplus
}
#endif

#endif // _CORE_CONNECTION_H_