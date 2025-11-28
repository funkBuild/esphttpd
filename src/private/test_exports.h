#ifndef _TEST_EXPORTS_H_
#define _TEST_EXPORTS_H_

#ifdef CONFIG_ESPHTTPD_TEST_MODE

#include "event_loop.h"
#include "connection.h"
#include "filesystem.h"
#include "radix_tree.h"

// Max routes for capacity tests - must match esphttpd.c
#define MAX_WS_ROUTES 16
#define MAX_MIDDLEWARES 8

#ifndef CONFIG_HTTPD_MAX_ROUTERS
#define CONFIG_HTTPD_MAX_ROUTERS 8
#endif

// Channel hash table configuration - must match esphttpd.c
#define CHANNEL_HASH_BUCKETS 16

// WebSocket route entry (must match esphttpd.c)
typedef struct {
    char pattern[64];
    httpd_ws_handler_t handler;
    void* user_ctx;
    uint32_t ping_interval_ms;
} test_httpd_ws_route_entry_t;

// Mounted router entry (must match esphttpd.c)
typedef struct {
    char prefix[32];
    uint8_t prefix_len;
    httpd_router_t router;
} test_mounted_router_t;

// Channel hash entry (must match esphttpd.c)
typedef struct {
    char name[32];
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

    // WebSocket channel registry (hash table for O(1) lookup)
    test_channel_hash_entry_t channel_hash[CHANNEL_HASH_BUCKETS];
    test_channel_hash_entry_t* channel_by_index[HTTPD_WS_MAX_CHANNELS];
    uint8_t channel_count;

    // State
    bool initialized;
    bool running;
} esphttpd_server_t;

// Global server instance accessible to tests
extern esphttpd_server_t* g_server;

#endif // CONFIG_ESPHTTPD_TEST_MODE

#endif // _TEST_EXPORTS_H_
