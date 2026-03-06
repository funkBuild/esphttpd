#ifndef _CORE_CONNECTION_H_
#define _CORE_CONNECTION_H_

#include "sdkconfig.h"

#include <stdint.h>
#include <stdbool.h>
#ifndef CONFIG_HTTPD_USE_RAW_API
#include <sys/socket.h>
#endif

#ifdef CONFIG_HTTPD_USE_RAW_API
struct tcp_pcb;  // Forward declaration
struct pbuf;     // Forward declaration
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of concurrent connections (configurable via Kconfig)
#ifndef CONFIG_HTTPD_MAX_CONNECTIONS
#define CONFIG_HTTPD_MAX_CONNECTIONS 16
#endif
#define MAX_CONNECTIONS CONFIG_HTTPD_MAX_CONNECTIONS

// Enforce limit: active_mask/write_pending_mask/ws_active_mask are uint32_t bitmasks
_Static_assert(MAX_CONNECTIONS <= 32,
    "MAX_CONNECTIONS exceeds uint32_t bitmask capacity");

// Connection states (must fit in 3-bit bitfield, max value 7)
typedef enum {
    CONN_STATE_FREE = 0,        // Connection slot is free
    CONN_STATE_NEW,             // New connection, reading request line
    CONN_STATE_HTTP_HEADERS,    // Reading HTTP headers
    CONN_STATE_HTTP_BODY,       // Reading HTTP body
    CONN_STATE_WEBSOCKET,       // WebSocket connection
    CONN_STATE_CLOSING,         // Connection is closing
    CONN_STATE_CLOSED,          // Connection closed, pending cleanup
    CONN_STATE_WS_CLOSING       // WebSocket close sent, waiting for client ack (RFC 6455)
} conn_state_t;

// Ensure connection states fit in 3-bit bitfield
_Static_assert(CONN_STATE_WS_CLOSING <= 7,
    "Connection states exceed 3-bit bitfield capacity");

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

#ifdef CONFIG_HTTPD_USE_RAW_API
// Raw TCP connection state (replaces socket fd)
typedef struct {
    struct tcp_pcb *pcb;         // lwIP TCP protocol control block
    struct pbuf *recv_chain;     // Pending received pbuf chain (reserved for future buffered recv)
    uint16_t recv_offset;        // Read offset into current pbuf (reserved for future buffered recv)
    uint32_t unacked_bytes;      // Bytes written but not yet acked
    bool write_pending;          // tcp_write data waiting for output
} raw_tcp_conn_t;
#endif

// Connection structure - naturally aligned for zero-penalty access on Xtensa
// Fields ordered by alignment: 32-bit, 16-bit, 8-bit, bitfields
// ~40 bytes per connection (vs ~36 packed), eliminates unaligned access traps
typedef struct {
    // 32-bit aligned fields (24 bytes)
#ifdef CONFIG_HTTPD_USE_RAW_API
    raw_tcp_conn_t raw;          // Raw TCP connection state
    int fd;                      // Compatibility: always -1 under raw API
#else
    int fd;                      // Socket file descriptor
#endif
    uint32_t content_length;     // Expected content length (supports up to 4GB)
    uint32_t bytes_received;     // Bytes received for current message
    uint32_t ws_mask_key;        // WebSocket masking key (when masked)
    uint32_t last_activity;      // Last activity timestamp (tick count)
    void* user_ctx;              // User-defined context

    // 16-bit aligned fields (12 bytes)
    uint16_t header_bytes;       // Bytes of headers received
    uint16_t ws_payload_len;     // Current frame payload length
    uint16_t ws_payload_read;    // Payload bytes already processed
    uint16_t route_id;           // Current route ID
    uint16_t url_offset;         // Offset in shared URL buffer
    uint16_t url_len;            // URL length

    // 8-bit fields (1 byte)
    uint8_t pool_index;          // Index in connection pool (0-31) for O(1) context lookup
                                 // Max is 32 due to uint32_t bitmasks (enforced by _Static_assert)

    // State and flags bitfields (1 byte)
    uint8_t state : 3;           // Connection state (3 bits)
    uint8_t is_websocket : 1;    // Is this a WebSocket connection (1 bit)
    uint8_t keep_alive : 1;      // HTTP keep-alive (1 bit)
    uint8_t method : 3;          // HTTP method (3 bits)

    // WebSocket flags bitfield (1 byte)
    uint8_t ws_fin : 1;         // WebSocket FIN flag (1 bit)
    uint8_t ws_masked : 1;      // WebSocket frame is masked (1 bit)
    uint8_t ws_opcode : 4;      // WebSocket opcode (4 bits)
    uint8_t ws_fragment : 1;    // Currently processing fragment (1 bit)
    uint8_t upgrade_ws : 1;     // Pending WebSocket upgrade (1 bit)

    // Deferred handling flags bitfield (1 byte)
    uint8_t deferred : 1;       // Body handling deferred to callbacks (1 bit)
    uint8_t defer_paused : 1;   // Deferred receiving paused (flow control) (1 bit)
    uint8_t continuation : 1;   // Body handling via continuation callbacks (1 bit)
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
connection_t* connection_alloc_slot(connection_pool_t* pool);
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