#ifndef _TEST_EXPORTS_H_
#define _TEST_EXPORTS_H_

#ifdef CONFIG_ESPHTTPD_TEST_MODE

#include <stdio.h>
#include "event_loop.h"
#include "connection.h"
#include "filesystem.h"
#include "radix_tree.h"
#include "http_parser.h"

// Configuration defaults - must match esphttpd.c
#ifndef CONFIG_HTTPD_MAX_WS_ROUTES
#define CONFIG_HTTPD_MAX_WS_ROUTES 8
#endif
#ifndef CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES
#define CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES 4
#endif
#ifndef CONFIG_HTTPD_MAX_ROUTERS
#define CONFIG_HTTPD_MAX_ROUTERS 8
#endif

#define MAX_WS_ROUTES CONFIG_HTTPD_MAX_WS_ROUTES
#define MAX_MIDDLEWARES CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES

// Channel hash table configuration - must match esphttpd.c
#define CHANNEL_HASH_BUCKETS 32

// WebSocket route entry (must match esphttpd.c)
typedef struct {
    const char* pattern;
    httpd_ws_handler_t handler;
    void* user_ctx;
    uint32_t ping_interval_ms;
} test_httpd_ws_route_entry_t;

// Mounted router entry (must match esphttpd.c)
typedef struct {
    const char* prefix;
    uint8_t prefix_len;
    httpd_router_t router;
} test_mounted_router_t;

// Channel hash entry (must match esphttpd.c)
typedef struct {
    char name[16];
    int8_t index;
} test_channel_hash_entry_t;

// Internal server context exposed for tests
// This MUST match the struct httpd_server layout in esphttpd.c EXACTLY
typedef struct {
    // Configuration
    httpd_config_t config;

    // Event loop and connections
    event_loop_t event_loop;
    connection_pool_t connection_pool;
    event_handlers_t handlers;

    // Filesystem (optional)
    filesystem_t* filesystem;
    bool filesystem_enabled;

    // Legacy routes - using radix tree for O(log n) lookup
    radix_tree_t* legacy_routes;
    test_httpd_ws_route_entry_t ws_routes[MAX_WS_ROUTES];
    uint8_t ws_route_count;

    // Mounted routers
    test_mounted_router_t mounted_routers[CONFIG_HTTPD_MAX_ROUTERS];
    uint8_t mounted_router_count;

    // Server-level middleware
    httpd_middleware_t middlewares[MAX_MIDDLEWARES];
    uint8_t middleware_count;

    // Server-level error handler
    httpd_err_handler_t error_handler;

    // WebSocket channel registry (hash table for O(1) lookup) - lazy allocated
    test_channel_hash_entry_t* channel_hash;  // Pointer (NULL until first channel join)
    test_channel_hash_entry_t* channel_by_index[HTTPD_WS_MAX_CHANNELS];
    uint8_t channel_count;

    // State
    bool initialized;
    bool running;
} esphttpd_server_t;

// Global server instance accessible to tests
extern esphttpd_server_t* g_server;

// Export _middleware_next for testing
httpd_err_t _middleware_next_test(httpd_req_t* req);

// Query parameter cache entry (must match esphttpd.c)
typedef struct {
    const char* key;
    const char* value;
    uint8_t key_len;
    uint8_t value_len;
} test_query_param_entry_t;

#define MAX_QUERY_PARAMS 8
#define REQ_HEADER_BUF_SIZE 2048
#ifndef CONFIG_HTTPD_MAX_REQ_HEADERS
#define CONFIG_HTTPD_MAX_REQ_HEADERS 16
#endif
#define MAX_REQ_HEADERS CONFIG_HTTPD_MAX_REQ_HEADERS

// Request header entry (must match esphttpd.c)
typedef struct {
    uint16_t key_offset;
    uint16_t value_offset;
    uint8_t key_len;
    uint8_t value_len;
} test_req_header_entry_t;

// Per-connection request context (must match esphttpd.c request_context_t EXACTLY)
// Fields are ordered: zero-target group first, then scratch buffers after _zero_end
typedef struct {
    // === Fields that NEED zeroing per request (memset target) ===
    httpd_req_t req;                      // Public request struct
    char* header_buf;                     // Header storage (dynamically allocated)
    char* uri_buf;                        // URI storage (dynamically allocated)
    struct httpd_server* server;          // Back pointer to server
    void* matched_route;                  // Matched route
    // Pre-received body data (received with headers) - dynamically allocated
    uint8_t* body_buf;                    // Buffer for body data (NULL if not allocated)
    size_t body_buf_len;                  // Amount of data in body_buf
    size_t body_buf_pos;                  // Current read position in body_buf
    // HTTP header accumulation buffer (for multi-recv parsing)
    uint8_t* recv_buf;                    // Accumulated recv data (NULL if not parsing)
    size_t recv_buf_len;                  // Amount of data accumulated
    size_t recv_buf_capacity;             // Allocated capacity
    http_parser_context_t parser_ctx;     // Persistent parser context across recv calls
    bool parsing_in_progress;             // True while headers are being accumulated
    bool recv_buf_is_heap;                // true if recv_buf was malloc'd (needs free)
    bool uri_buf_is_heap;                 // true if uri_buf was malloc'd (needs free)
    uint8_t query_param_count;
    bool query_parsed;
    // Deferred (async) body handling
    struct {
        httpd_body_cb_t on_body;          // Body data callback
        httpd_done_cb_t on_done;          // Completion callback
        FILE* file_fp;                    // File pointer for defer_to_file (inlined)
        httpd_done_cb_t file_user_done_cb; // User's done callback for defer_to_file (inlined)
        bool active;                      // Request is in deferred mode
        bool paused;                      // Flow control - receiving paused
    } defer;
    // Async response sending
    struct {
        httpd_send_cb_t on_done;          // Completion callback
        bool active;                      // Response send in progress
    } async_send;
    // Data provider for streaming responses
    struct {
        httpd_data_provider_t provider;   // Data provider callback
        httpd_send_cb_t on_complete;      // Completion callback
        bool active;                      // Provider mode active
        bool eof_reached;                 // Provider returned 0 (EOF)
        bool use_chunked;                 // Using chunked transfer encoding
    } data_provider;
    // Continuation-based body handling (non-blocking)
    struct {
        httpd_continuation_t handler;     // Continuation handler callback
        httpd_req_continuation_t cont;    // Continuation state
        bool active;                      // Continuation mode active
    } continuation;
    char _zero_end[0];                    // Marker: memset stops here

    // === Scratch buffers that DON'T need zeroing per request ===
    test_req_header_entry_t headers[MAX_REQ_HEADERS];  // Header index
    uint8_t inline_recv_buf[512];         // Embedded buffer for single-packet requests
    char inline_uri_buf[64];             // Embedded buffer for typical URI lengths
    test_query_param_entry_t query_params[MAX_QUERY_PARAMS];
    // Middleware chain (persists for request lifetime, safe across deferred handlers)
    httpd_middleware_t mw_chain[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];
} test_request_context_t;

// Global request_contexts pointer array accessible to tests (via void* for type safety)
// Points to request_context_t*[MAX_CONNECTIONS] - pointers into pre-allocated backing storage
extern void* g_test_request_contexts;

// Global connection_send_buffers pointer array accessible to tests
// Points to send_buffer_t*[MAX_CONNECTIONS] - pointers into pre-allocated backing storage
extern void* g_test_send_buffers;

#ifdef CONFIG_HTTPD_USE_RAW_API
#include "raw_tcp.h"
// Tests can install a mock for raw_tcp_write to capture output
// See raw_tcp.h: raw_tcp_set_write_mock()
#endif

#endif // CONFIG_ESPHTTPD_TEST_MODE

#endif // _TEST_EXPORTS_H_
