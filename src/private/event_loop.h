#ifndef _CORE_EVENT_LOOP_H_
#define _CORE_EVENT_LOOP_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>
#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

// Event loop configuration
typedef struct {
    uint16_t port;                  // Server port
    uint16_t backlog;               // Listen backlog
    uint32_t timeout_ms;            // Connection timeout in milliseconds
    uint32_t select_timeout_ms;     // Select timeout in milliseconds
    size_t io_buffer_size;          // I/O buffer size (typically 1024)
    bool nodelay;                   // TCP_NODELAY option
    bool reuseaddr;                 // SO_REUSEADDR option
} event_loop_config_t;

// Event loop context
typedef struct {
    int listen_fd;                  // Listening socket
    connection_pool_t* pool;        // Connection pool
    event_loop_config_t config;     // Configuration
    uint32_t tick_count;            // Tick counter for timeouts
    uint32_t timeout_ticks;         // Precomputed timeout in ticks
    struct timeval select_timeout;  // Precomputed select timeout struct
    bool running;                   // Event loop is running

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

// Create and bind listening socket
int event_loop_create_listener(event_loop_t* loop);

// Main event loop - runs until stopped
void event_loop_run(event_loop_t* loop, const event_handlers_t* handlers);

// Stop the event loop
void event_loop_stop(event_loop_t* loop);

// Process single iteration (for testing/integration)
int event_loop_iteration(event_loop_t* loop, const event_handlers_t* handlers, uint8_t* io_buffer);

// Utility functions
void event_loop_check_timeouts(event_loop_t* loop);

#ifdef __cplusplus
}
#endif

#endif // _CORE_EVENT_LOOP_H_