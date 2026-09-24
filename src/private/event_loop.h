#ifndef _CORE_EVENT_LOOP_H_
#define _CORE_EVENT_LOOP_H_

#include "sdkconfig.h"

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#ifndef CONFIG_HTTPD_USE_RAW_API
#include <sys/time.h>
#endif
#include "connection.h"

#ifdef CONFIG_HTTPD_USE_RAW_API
struct tcp_pcb;  // Forward declaration
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Event loop configuration
typedef struct {
    uint16_t port;                  // Server port
    uint16_t backlog;               // Listen backlog
    uint32_t timeout_ms;            // Connection timeout in milliseconds
#ifndef CONFIG_HTTPD_USE_RAW_API
    uint32_t select_timeout_ms;     // Select timeout in milliseconds
    size_t io_buffer_size;          // I/O buffer size (typically 1024)
#endif
    uint16_t ws_close_timeout_ms;       // WebSocket close handshake timeout (0 = default 5s)
    uint16_t header_timeout_ms;         // Deadline to complete request headers (0 = default 10s)
    uint16_t body_timeout_ms;           // Request-body progress window (0 = default 10s)
    uint16_t body_min_bytes;            // Body bytes required per window (0 = default 4096)
    bool nodelay;                   // TCP_NODELAY option
    bool reuseaddr;                 // SO_REUSEADDR option
} event_loop_config_t;

// Event loop context
typedef struct {
#ifdef CONFIG_HTTPD_USE_RAW_API
    struct tcp_pcb* listen_pcb;     // Listening PCB (raw API)
#else
    int listen_fd;                  // Listening socket
#endif
    connection_pool_t* pool;        // Connection pool
    event_loop_config_t config;     // Configuration
    uint32_t tick_count;            // Tick counter for timeouts
    uint32_t timeout_ticks;         // Precomputed timeout in ticks
    uint32_t ws_close_timeout_ticks; // Precomputed WS close handshake timeout in ticks
    uint32_t header_timeout_ticks;  // Precomputed request-header deadline in ticks
    uint32_t body_timeout_ticks;    // Precomputed body-progress window in ticks
    uint32_t last_check_tick;       // tick_count at the previous timeout scan
    int64_t last_tick_us;           // Wall-clock time of the last tick advance (µs)
#ifndef CONFIG_HTTPD_USE_RAW_API
    struct timeval select_timeout;  // Precomputed select timeout struct
#endif
    atomic_bool running;
    atomic_bool stop_requested;                   // Event loop is running

#ifndef CONFIG_HTTPD_USE_RAW_API
    // I/O buffer (heap allocated to save stack space)
    uint8_t* io_buffer;             // Receive buffer (allocated on start)
#endif

    // Statistics
    uint32_t total_connections;     // Total connections accepted
    uint32_t total_requests;        // Total HTTP requests
    uint32_t total_ws_frames;       // Total WebSocket frames
} event_loop_t;

// Event handlers - to be implemented by modules
typedef struct {
    // HTTP handlers
    void (*on_http_request)(connection_t* conn, uint8_t* buffer, size_t len);
    void (*on_http_body)(connection_t* conn, uint8_t* buffer, size_t len);

    // WebSocket handlers
    void (*on_ws_frame)(connection_t* conn, uint8_t* buffer, size_t len);
    void (*on_ws_connect)(connection_t* conn);
    void (*on_ws_disconnect)(connection_t* conn);

    // Connection events
    void (*on_connect)(connection_t* conn);
    void (*on_disconnect)(connection_t* conn);

    // Write-ready handler (for non-blocking sends)
    void (*on_write_ready)(connection_t* conn);
} event_handlers_t;

// Initialize event loop with default configuration
void event_loop_init_default(event_loop_t* loop, connection_pool_t* pool);

// Initialize event loop with custom configuration
void event_loop_init(event_loop_t* loop, connection_pool_t* pool, const event_loop_config_t* config);

#ifndef CONFIG_HTTPD_USE_RAW_API
// Create and bind listening socket (socket mode only)
int event_loop_create_listener(event_loop_t* loop);

// Main event loop - runs until stopped (socket mode only)
void event_loop_run(event_loop_t* loop, const event_handlers_t* handlers);

// Process single iteration (for testing/integration, socket mode only)
int event_loop_iteration(event_loop_t* loop, const event_handlers_t* handlers, uint8_t* io_buffer);

// Utility functions (socket mode only)
void event_loop_check_timeouts(event_loop_t* loop);

// Pool full: close the longest-idle keep-alive connection (nothing in
// progress) and return its slot, or -1 when no connection is evictable
int event_loop_evict_idle(event_loop_t* loop, const event_handlers_t* handlers);
#endif

// Stop the event loop (both modes)
void event_loop_stop(event_loop_t* loop);

// Wake the event loop out of select() so it rebuilds its fd sets now. For
// producers on OTHER tasks that change what the loop must wait on (bytes
// queued behind a full socket, a resumed deferred upload, a stop request);
// without it the change is only seen at the next select timeout (1 s).
// Safe from any task; coalesced (at most one wake datagram in flight); a
// no-op from the loop task itself, which rebuilds its sets before every
// select anyway. No-op in raw API mode (lwIP callbacks, no select).
void event_loop_wake(event_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif // _CORE_EVENT_LOOP_H_