/**
 * @file esphttpd.c
 * @brief HTTP/WebSocket server implementation
 */

#include "esphttpd.h"
#include "private/connection.h"
#include "private/event_loop.h"
#include "private/http_parser.h"
#include "private/websocket.h"
#include "private/radix_tree.h"
#include "private/template.h"
#include "private/filesystem.h"
#include "private/send_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char* TAG = "ESPHTTPD";

// ============================================================================
// Internal Structures
// ============================================================================

#define MAX_MIDDLEWARES 8
#define MAX_HTTP_ROUTES 64
#define MAX_WS_ROUTES 16
#define REQ_HEADER_BUF_SIZE 2048
#define MAX_REQ_HEADERS 32

#ifndef CONFIG_HTTPD_MAX_ROUTERS
#define CONFIG_HTTPD_MAX_ROUTERS 8
#endif

// Header entry for per-request storage
typedef struct {
    uint16_t key_offset;
    uint16_t value_offset;
    uint8_t key_len;
    uint8_t value_len;
} req_header_entry_t;

// HTTP route entry (new API)
typedef struct {
    char pattern[64];
    http_method_t method;
    httpd_handler_t handler;
    void* user_ctx;
    bool has_params;           // Pattern contains :param
} httpd_route_entry_t;

// WebSocket route entry (new API)
typedef struct {
    char pattern[64];
    httpd_ws_handler_t handler;
    void* user_ctx;
    uint32_t ping_interval_ms;
} httpd_ws_route_entry_t;

// Mounted router entry
typedef struct {
    char prefix[32];
    uint8_t prefix_len;
    httpd_router_t router;  // httpd_router_t is already a pointer type
} mounted_router_t;

// Channel hash table entry (replaces linked list for O(1) lookup)
typedef struct {
    char name[32];              // Channel name (empty string if unused)
    int8_t index;               // Bitmask index (0-31), -1 if empty
} channel_hash_entry_t;

#define CHANNEL_HASH_BUCKETS 64  // Power of 2, > 2 * MAX_CHANNELS for low collision

// FNV-1a hash for channel names
static uint32_t channel_hash_fn(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

// Server context structure
struct httpd_server {
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
    httpd_ws_route_entry_t ws_routes[MAX_WS_ROUTES];
    uint8_t ws_route_count;

    // Mounted routers
    mounted_router_t mounted_routers[CONFIG_HTTPD_MAX_ROUTERS];
    uint8_t mounted_router_count;

    // Server-level middleware
    httpd_middleware_t middlewares[MAX_MIDDLEWARES];
    uint8_t middleware_count;

    // Server-level error handler
    httpd_err_handler_t error_handler;

    // WebSocket channel registry (hash table for O(1) lookup)
    channel_hash_entry_t channel_hash[CHANNEL_HASH_BUCKETS];  // Hash table
    channel_hash_entry_t* channel_by_index[HTTPD_WS_MAX_CHANNELS];  // Index -> entry mapping
    uint8_t channel_count;                          // Next available channel index

    // Send buffer pool for non-blocking sends
    send_buffer_pool_t send_buffer_pool;

    // State
    bool initialized;
    bool running;
};

// Per-connection send buffers
static send_buffer_t connection_send_buffers[MAX_CONNECTIONS];

// Initialize channel hash table - O(1) memset instead of O(n) loop
// Setting all bytes to 0xFF makes index = -1 (int8_t), marking slots as empty.
// Name field contents don't matter for empty slots since we check index < 0 first.
static inline void init_channel_hash(struct httpd_server* server) {
    memset(server->channel_hash, 0xFF, sizeof(server->channel_hash));
    memset(server->channel_by_index, 0, sizeof(server->channel_by_index));
    server->channel_count = 0;
}

// Query parameter cache entry (pointers into query string)
typedef struct {
    const char* key;
    const char* value;
    uint8_t key_len;
    uint8_t value_len;
} query_param_entry_t;

#define MAX_QUERY_PARAMS 8

// Per-connection request context
typedef struct {
    httpd_req_t req;                      // Public request struct
    req_header_entry_t headers[MAX_REQ_HEADERS];  // Header index
    char header_buf[REQ_HEADER_BUF_SIZE]; // Header storage
    char uri_buf[256];                    // URI storage
    struct httpd_server* server;          // Back pointer to server
    httpd_route_entry_t* matched_route;   // Matched route
    // Pre-received body data (received with headers)
    uint8_t body_buf[1024];               // Buffer for body data received with headers
    size_t body_buf_len;                  // Amount of data in body_buf
    size_t body_buf_pos;                  // Current read position in body_buf
    // Query parameter cache (lazy parsing)
    query_param_entry_t query_params[MAX_QUERY_PARAMS];
    uint8_t query_param_count;
    bool query_parsed;
    // Deferred (async) body handling
    struct {
        httpd_body_cb_t on_body;          // Body data callback
        httpd_done_cb_t on_done;          // Completion callback
        void* file_ctx;                   // Internal context for defer_to_file
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
} request_context_t;

// Per-connection WebSocket context
typedef struct {
    httpd_ws_t ws;                        // Public WebSocket struct
    httpd_ws_route_entry_t* route;        // Associated route
    ws_frame_context_t frame_ctx;         // Frame parsing context
    uint32_t channel_mask;                // Bitmask of subscribed channels
} ws_context_t;

// Global server instance (for now - could be made multi-instance later)
static struct httpd_server server_instance;
#ifdef CONFIG_ESPHTTPD_TEST_MODE
struct httpd_server* g_server = NULL;  // Non-static for test access
#else
static struct httpd_server* g_server = NULL;
#endif
static filesystem_t fs_instance;

// WebSocket client key buffer (used by http_parser)
char ws_client_key[32] = {0};

// Per-connection contexts
static request_context_t request_contexts[MAX_CONNECTIONS];
static ws_context_t ws_contexts[MAX_CONNECTIONS];

// Track current connection being parsed (for header storage callback)
static connection_t* g_parsing_connection = NULL;

// Forward declarations
static request_context_t* get_request_context(connection_t* conn);
static void store_header_in_req(request_context_t* ctx, const uint8_t* key, uint8_t key_len,
                                const uint8_t* value, uint8_t value_len);

// ============================================================================
// Forward Declarations
// ============================================================================

static void on_http_request(connection_t* conn, uint8_t* buffer, size_t len);
static void on_http_body(connection_t* conn, uint8_t* buffer, size_t len);
static void on_ws_frame(connection_t* conn, uint8_t* buffer, size_t len);
static void on_ws_connect(connection_t* conn);
static void on_ws_disconnect(connection_t* conn);
static void on_disconnect(connection_t* conn);
static void on_write_ready(connection_t* conn);
static void server_task(void* pvParameters);

// Get send buffer for connection
static inline send_buffer_t* get_send_buffer(connection_t* conn) {
    return &connection_send_buffers[conn->pool_index];
}

// ============================================================================
// Utility Functions
// ============================================================================

// Drain send buffer - sends as much buffered data as possible
// Returns true if buffer is now empty, false if more data pending
static bool drain_send_buffer(connection_t* conn) {
    send_buffer_t* sb = get_send_buffer(conn);

    while (send_buffer_has_data(sb)) {
        const uint8_t* data;
        size_t len = send_buffer_peek(sb, &data);
        if (len == 0) break;

        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // Socket buffer full, more data pending
            }
            ESP_LOGE(TAG, "drain_send_buffer failed: %s", strerror(errno));
            return false;
        }
        send_buffer_consume(sb, sent);
    }

    return !send_buffer_has_data(sb);
}

// Non-blocking send - tries to send data, queues remainder if socket would block
// Returns number of bytes sent/queued, or -1 on error
static ssize_t send_nonblocking(connection_t* conn, const void* data, size_t len, int flags) {
    if (!conn || !data || len == 0) {
        return 0;
    }

    send_buffer_t* sb = get_send_buffer(conn);
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = len;

    // If there's already queued data, we must queue to maintain order
    // Only attempt drain for larger writes (>64 bytes) where making room is worthwhile
    if (send_buffer_has_data(sb)) {
        if (len > 64) {
            drain_send_buffer(conn);
        }
        // Still have queued data - must queue this too to maintain order
        if (send_buffer_has_data(sb)) {
            goto queue_data;
        }
    }

    // Try to send directly (fast path)
    while (remaining > 0) {
        ssize_t sent = send(conn->fd, ptr, remaining, flags | MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, queue remaining data
                goto queue_data;
            }
            ESP_LOGE(TAG, "send_nonblocking failed: %s", strerror(errno));
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    // All data sent directly
    return (ssize_t)len;

queue_data:
    // Ensure we have a buffer allocated
    if (!sb->allocated) {
        if (!send_buffer_alloc(sb, &g_server->send_buffer_pool)) {
            ESP_LOGE(TAG, "Failed to allocate send buffer");
            return -1;
        }
    }

    // Try to queue data - if buffer full, attempt one drain and retry
    if (send_buffer_queue(sb, ptr, remaining) < 0) {
        // Buffer full - try draining once
        drain_send_buffer(conn);
        if (send_buffer_queue(sb, ptr, remaining) < 0) {
            // Still can't fit - data is too large for buffer
            ESP_LOGE(TAG, "Send buffer full, cannot queue %zu bytes", remaining);
            return -1;
        }
    }

    // Mark connection as having pending writes
    connection_mark_write_pending(&g_server->connection_pool, conn->pool_index, true);

    return (ssize_t)len;
}

// Legacy blocking send - for backwards compatibility during transition
// TODO: Remove once all callers updated to non-blocking
static ssize_t send_all(int fd, const void* data, size_t len, int flags) {
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, flags);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, yield briefly and retry
                vTaskDelay(1);
                continue;
            }
            ESP_LOGE(TAG, "send_all failed: %s", strerror(errno));
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    return (ssize_t)len;
}

const char* httpd_status_text(int status) {
    // Common case fast path (200, 404, 500 are most frequent)
    if (status == 200) return "OK";
    if (status == 404) return "Not Found";
    if (status == 500) return "Internal Server Error";

    // Remaining cases
    switch (status) {
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 426: return "Upgrade Required";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

// MIME type lookup table entry
typedef struct {
    const char* ext;
    uint8_t ext_len;
    const char* mime;
} mime_entry_t;

// MIME types grouped by first character (lowercase) for O(1) dispatch
// Each group is small (1-3 entries) so linear search within group is fast
static const mime_entry_t mime_c[] = { {"css", 3, "text/css"}, {NULL, 0, NULL} };
static const mime_entry_t mime_g[] = { {"gif", 3, "image/gif"}, {"gz", 2, "application/gzip"}, {NULL, 0, NULL} };
static const mime_entry_t mime_h[] = { {"html", 4, "text/html"}, {"htm", 3, "text/html"}, {NULL, 0, NULL} };
static const mime_entry_t mime_i[] = { {"ico", 3, "image/x-icon"}, {NULL, 0, NULL} };
static const mime_entry_t mime_j[] = { {"js", 2, "application/javascript"}, {"json", 4, "application/json"}, {"jpg", 3, "image/jpeg"}, {"jpeg", 4, "image/jpeg"}, {NULL, 0, NULL} };
static const mime_entry_t mime_p[] = { {"png", 3, "image/png"}, {"pdf", 3, "application/pdf"}, {NULL, 0, NULL} };
static const mime_entry_t mime_s[] = { {"svg", 3, "image/svg+xml"}, {NULL, 0, NULL} };
static const mime_entry_t mime_t[] = { {"txt", 3, "text/plain"}, {"ttf", 3, "font/ttf"}, {NULL, 0, NULL} };
static const mime_entry_t mime_w[] = { {"woff", 4, "font/woff"}, {"woff2", 5, "font/woff2"}, {NULL, 0, NULL} };
static const mime_entry_t mime_x[] = { {"xml", 3, "application/xml"}, {NULL, 0, NULL} };

// Dispatch table indexed by (first_char - 'a'), NULL for unused letters
static const mime_entry_t* mime_dispatch[26] = {
    NULL,    // a
    NULL,    // b
    mime_c,  // c
    NULL,    // d
    NULL,    // e
    NULL,    // f
    mime_g,  // g
    mime_h,  // h
    mime_i,  // i
    mime_j,  // j
    NULL,    // k
    NULL,    // l
    NULL,    // m
    NULL,    // n
    NULL,    // o
    mime_p,  // p
    NULL,    // q
    NULL,    // r
    mime_s,  // s
    mime_t,  // t
    NULL,    // u
    NULL,    // v
    mime_w,  // w
    mime_x,  // x
    NULL,    // y
    NULL,    // z
};

const char* httpd_get_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;

    // Get extension length
    size_t ext_len = strlen(ext);
    if (ext_len == 0 || ext_len > 5) return "application/octet-stream";

    // Convert first char to lowercase for dispatch
    char first = ext[0];
    if (first >= 'A' && first <= 'Z') first += 32;
    if (first < 'a' || first > 'z') return "application/octet-stream";

    // O(1) dispatch to correct group
    const mime_entry_t* group = mime_dispatch[first - 'a'];
    if (!group) return "application/octet-stream";

    // Search within small group (max 4 entries)
    for (const mime_entry_t* e = group; e->ext; e++) {
        if (e->ext_len == ext_len && strcasecmp(ext, e->ext) == 0) {
            return e->mime;
        }
    }

    return "application/octet-stream";
}

// Hex digit to value lookup table (-1 for invalid chars)
// Index by ASCII code, returns 0-15 for valid hex or -1 for invalid
static const int8_t hex_lookup[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 0-15
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 16-31
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 32-47
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1, // 48-63 (0-9)
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 64-79 (A-F)
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 80-95
    -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 96-111 (a-f)
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 112-127
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, // 128-255 (extended ASCII)
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

int httpd_url_decode(const char* src, char* dst, size_t dst_size) {
    size_t src_len = strlen(src);
    size_t dst_idx = 0;

    for (size_t i = 0; i < src_len && dst_idx < dst_size - 1; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            // O(1) hex lookup instead of strtol()
            int8_t hi = hex_lookup[(uint8_t)src[i+1]];
            int8_t lo = hex_lookup[(uint8_t)src[i+2]];
            if (hi >= 0 && lo >= 0) {
                dst[dst_idx++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        } else if (src[i] == '+') {
            dst[dst_idx++] = ' ';
            continue;
        }
        dst[dst_idx++] = src[i];
    }

    dst[dst_idx] = '\0';
    return (int)dst_idx;
}

// Inline case-insensitive comparison - avoids libc call overhead
static inline bool str_casecmp(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        // Fast case-insensitive compare using OR with 0x20
        // Works for ASCII letters: 'A'|0x20 == 'a', etc.
        if ((a[i] | 0x20) != (b[i] | 0x20)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Query String Parsing (with lazy caching for O(k) lookup after first parse)
// ============================================================================

// Get request_context from httpd_req_t for internal use - O(1) via pool_index
static request_context_t* get_req_context(httpd_req_t* req) {
    if (!req || !req->_internal || !g_server) return NULL;
    connection_t* conn = (connection_t*)req->_internal;
    return &request_contexts[conn->pool_index];
}

// Parse query string once and cache results
static void parse_query_params(request_context_t* ctx) {
    if (ctx->query_parsed) return;
    ctx->query_parsed = true;
    ctx->query_param_count = 0;

    const char* query = ctx->req.query;
    if (!query || ctx->req.query_len == 0) return;

    const char* p = query;
    const char* end = query + ctx->req.query_len;

    while (p < end && ctx->query_param_count < MAX_QUERY_PARAMS) {
        // Find '=' for key-value split
        const char* eq = memchr(p, '=', end - p);
        if (!eq) break;

        // Find end of this parameter
        const char* amp = memchr(eq + 1, '&', end - (eq + 1));
        const char* v_end = amp ? amp : end;

        // Store in cache (pointers into query string, no copying)
        query_param_entry_t* entry = &ctx->query_params[ctx->query_param_count];
        entry->key = p;
        entry->key_len = eq - p;
        entry->value = eq + 1;
        entry->value_len = v_end - (eq + 1);
        ctx->query_param_count++;

        if (!amp) break;
        p = amp + 1;
    }
}

int httpd_req_get_query(httpd_req_t* req, const char* key, char* value, size_t value_size) {
    if (!req || !key || !value || value_size == 0) return -1;
    if (!req->query || req->query_len == 0) return -1;

    // Get request context and parse query if needed
    request_context_t* ctx = get_req_context(req);
    if (ctx) {
        parse_query_params(ctx);

        // O(k) lookup in cached parameters
        size_t key_len = strlen(key);
        for (uint8_t i = 0; i < ctx->query_param_count; i++) {
            query_param_entry_t* entry = &ctx->query_params[i];
            if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
                // Found - URL decode value into output buffer
                return httpd_url_decode(entry->value, value, value_size);
            }
        }
        return -1;  // Not found in cache
    }

    // Fallback to original linear scan if context unavailable
    size_t key_len = strlen(key);
    const char* p = req->query;
    const char* end = req->query + req->query_len;

    while (p < end) {
        const char* eq = memchr(p, '=', end - p);
        if (!eq) break;

        size_t k_len = eq - p;
        if (k_len == key_len && memcmp(p, key, key_len) == 0) {
            const char* v_start = eq + 1;
            return httpd_url_decode(v_start, value, value_size);
        }

        const char* amp = memchr(p, '&', end - p);
        if (!amp) break;
        p = amp + 1;
    }

    return -1;
}

const char* httpd_req_get_query_string(httpd_req_t* req) {
    return req ? req->query : NULL;
}

// ============================================================================
// Request Context Management
// ============================================================================

// O(1) context lookup using cached pool_index (avoids O(k) connection_get_index)
static inline request_context_t* get_request_context(connection_t* conn) {
    if (!g_server) return NULL;
    return &request_contexts[conn->pool_index];
}

static inline ws_context_t* get_ws_context(connection_t* conn) {
    if (!g_server) return NULL;
    return &ws_contexts[conn->pool_index];
}

static void init_request_context(request_context_t* ctx, connection_t* conn) {
    // Selective initialization - avoid clearing large buffers (~3.5KB savings)
    // Only reset fields that need to be zeroed for a new request

    // Essential pointers and connection info
    ctx->req.fd = conn->fd;
    ctx->req._internal = conn;
    ctx->req.header_buf = ctx->header_buf;
    ctx->req.header_buf_size = sizeof(ctx->header_buf);
    ctx->req.uri = ctx->uri_buf;
    ctx->req.status_code = 200;
    ctx->server = g_server;

    // Reset request line fields
    ctx->req.method = HTTP_GET;
    ctx->req.uri_len = 0;
    ctx->req.path = NULL;
    ctx->req.path_len = 0;
    ctx->req.query = NULL;
    ctx->req.query_len = 0;
    ctx->req.original_url = NULL;
    ctx->req.original_url_len = 0;
    ctx->req.base_url = NULL;
    ctx->req.base_url_len = 0;

    // Reset header tracking (don't clear header_buf itself)
    ctx->req.header_buf_used = 0;
    ctx->req.header_count = 0;

    // Reset route params
    ctx->req.param_count = 0;

    // Reset body tracking
    ctx->req.content_length = 0;
    ctx->req.body_received = 0;

    // Reset response state
    ctx->req.headers_sent = false;
    ctx->req.user_data = NULL;
    ctx->req.is_websocket = false;
    ctx->req.ws_key[0] = '\0';

    // Reset context fields
    ctx->matched_route = NULL;
    ctx->body_buf_len = 0;
    ctx->body_buf_pos = 0;
    ctx->query_param_count = 0;
    ctx->query_parsed = false;

    // Reset deferred handling state
    ctx->defer.on_body = NULL;
    ctx->defer.on_done = NULL;
    ctx->defer.file_ctx = NULL;
    ctx->defer.active = false;
    ctx->defer.paused = false;

    // Reset async send state
    ctx->async_send.on_done = NULL;
    ctx->async_send.active = false;

    // Reset data provider state
    ctx->data_provider.provider = NULL;
    ctx->data_provider.on_complete = NULL;
    ctx->data_provider.active = false;
    ctx->data_provider.eof_reached = false;
    ctx->data_provider.use_chunked = false;
}

// Store header in request context
void esphttpd_store_header(const uint8_t* key, uint8_t key_len,
                           const uint8_t* value, uint8_t value_len) {
    // Use the global parsing connection to find the request context
    if (!g_parsing_connection || !g_server) return;

    request_context_t* ctx = get_request_context(g_parsing_connection);
    if (!ctx) return;

    store_header_in_req(ctx, key, key_len, value, value_len);
}

// New header storage that works with request context
// Optimized: uses pointer arithmetic to avoid repeated offset calculations
static void store_header_in_req(request_context_t* ctx, const uint8_t* key, uint8_t key_len,
                                const uint8_t* value, uint8_t value_len) {
    if (ctx->req.header_count >= MAX_REQ_HEADERS) return;

    size_t needed = key_len + 1 + value_len + 1;
    if (ctx->req.header_buf_used + needed > ctx->req.header_buf_size) return;

    req_header_entry_t* entry = &ctx->headers[ctx->req.header_count];
    char* dst = &ctx->header_buf[ctx->req.header_buf_used];

    // Store key
    entry->key_offset = ctx->req.header_buf_used;
    entry->key_len = key_len;
    memcpy(dst, key, key_len);
    dst[key_len] = '\0';
    dst += key_len + 1;

    // Store value
    entry->value_offset = ctx->req.header_buf_used + key_len + 1;
    entry->value_len = value_len;
    memcpy(dst, value, value_len);
    dst[value_len] = '\0';

    ctx->req.header_buf_used += needed;
    ctx->req.header_count++;
}

// ============================================================================
// Server Lifecycle
// ============================================================================

httpd_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config) {
    if (!handle) return HTTPD_ERR_INVALID_ARG;

    struct httpd_server* server = &server_instance;

    if (server->initialized) {
        return HTTPD_ERR_ALREADY_RUNNING;
    }

    // Use default config if not provided
    httpd_config_t cfg = config ? *config : (httpd_config_t)HTTPD_DEFAULT_CONFIG();
    server->config = cfg;

    // Initialize event loop config
    event_loop_config_t el_config = {
        .port = cfg.port,
        .backlog = cfg.backlog,
        .timeout_ms = cfg.timeout_ms,
        .select_timeout_ms = 1000,
        .io_buffer_size = cfg.recv_buffer_size,
        .nodelay = true,
        .reuseaddr = true
    };

    // Initialize components
    event_loop_init(&server->event_loop, &server->connection_pool, &el_config);
    connection_pool_init(&server->connection_pool);

    // Initialize send buffer pool and per-connection buffers
    send_buffer_pool_init(&server->send_buffer_pool);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        send_buffer_init(&connection_send_buffers[i]);
    }

    // Initialize channel hash table
    init_channel_hash(server);

    // Initialize radix tree for legacy routes (O(log n) lookup)
    server->legacy_routes = radix_tree_create();
    if (!server->legacy_routes) {
        ESP_LOGE(TAG, "Failed to create legacy routes radix tree");
        return HTTPD_ERR_NO_MEM;
    }

    // Setup event handlers
    server->handlers.on_http_request = on_http_request;
    server->handlers.on_http_body = on_http_body;
    server->handlers.on_ws_frame = on_ws_frame;
    server->handlers.on_ws_connect = on_ws_connect;
    server->handlers.on_ws_disconnect = on_ws_disconnect;
    server->handlers.on_disconnect = on_disconnect;
    server->handlers.on_write_ready = on_write_ready;

    server->initialized = true;
    g_server = server;
    *handle = server;

    ESP_LOGI(TAG, "Server initialized on port %d", cfg.port);

#ifdef CONFIG_ESPHTTPD_TEST_MODE
    server->event_loop.running = true;
    server->running = true;
    ESP_LOGI(TAG, "Server started in TEST MODE (no task created)");
#else
    // Start the server task
    BaseType_t ret = xTaskCreate(server_task, "httpd",
                                  cfg.stack_size, server,
                                  cfg.task_priority, NULL);
    if (ret != pdPASS) {
        server->initialized = false;
        g_server = NULL;
        return HTTPD_ERR_NO_MEM;
    }
    server->running = true;
#endif

    return HTTPD_OK;
}

httpd_err_t httpd_stop(httpd_handle_t handle) {
    struct httpd_server* server = handle;
    if (!server || !server->initialized) {
        return HTTPD_ERR_NOT_RUNNING;
    }

    ESP_LOGI(TAG, "Stopping server");
    event_loop_stop(&server->event_loop);

    // Close all active connections
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_is_active(&server->connection_pool, i)) {
            connection_t* conn = &server->connection_pool.connections[i];
            close(conn->fd);
        }
    }

    server->initialized = false;
    server->running = false;

    // Destroy legacy routes radix tree
    if (server->legacy_routes) {
        radix_tree_destroy(server->legacy_routes);
        server->legacy_routes = NULL;
    }

    // Reset route counts
    server->ws_route_count = 0;
    server->middleware_count = 0;

    // Reset channel hash table
    init_channel_hash(server);

    if (g_server == server) {
        g_server = NULL;
    }

    return HTTPD_OK;
}

bool httpd_is_running(httpd_handle_t handle) {
    struct httpd_server* server = handle;
    return server && server->running;
}

static void server_task(void* pvParameters) {
    struct httpd_server* server = (struct httpd_server*)pvParameters;
    event_loop_run(&server->event_loop, &server->handlers);
    server->running = false;
    vTaskDelete(NULL);
}

// ============================================================================
// Route Management
// ============================================================================

httpd_err_t httpd_register_route(httpd_handle_t handle, const httpd_route_t* route) {
    struct httpd_server* server = handle;
    if (!server || !route || !route->pattern || !route->handler) {
        return HTTPD_ERR_INVALID_ARG;
    }

    if (!server->legacy_routes) {
        ESP_LOGE(TAG, "Legacy routes radix tree not initialized");
        return HTTPD_ERR_INVALID_ARG;
    }

    // Insert into radix tree for O(log n) lookup
    httpd_err_t err = radix_insert(server->legacy_routes, route->pattern,
                                   route->method, route->handler,
                                   route->user_ctx, NULL, 0);
    if (err != HTTPD_OK) {
        ESP_LOGW(TAG, "Failed to register route: %s", route->pattern);
        return err;
    }

    ESP_LOGI(TAG, "Registered route: %s %s",
             route->method == HTTP_GET ? "GET" :
             route->method == HTTP_POST ? "POST" :
             route->method == HTTP_PUT ? "PUT" :
             route->method == HTTP_DELETE ? "DELETE" : "OTHER",
             route->pattern);

    return HTTPD_OK;
}

httpd_err_t httpd_register_ws_route(httpd_handle_t handle, const httpd_ws_route_t* route) {
    struct httpd_server* server = handle;
    if (!server || !route || !route->pattern || !route->handler) {
        return HTTPD_ERR_INVALID_ARG;
    }

    if (server->ws_route_count >= MAX_WS_ROUTES) {
        ESP_LOGW(TAG, "WebSocket route table full");
        return HTTPD_ERR_ROUTE_FULL;
    }

    httpd_ws_route_entry_t* entry = &server->ws_routes[server->ws_route_count++];
    strncpy(entry->pattern, route->pattern, sizeof(entry->pattern) - 1);
    entry->handler = route->handler;
    entry->user_ctx = route->user_ctx;
    entry->ping_interval_ms = route->ping_interval_ms;

    ESP_LOGI(TAG, "Registered WebSocket route: %s", route->pattern);

    return HTTPD_OK;
}

// ============================================================================
// Router Mounting (Phase 5)
// ============================================================================

httpd_err_t httpd_mount(httpd_handle_t handle, const char* prefix,
                        httpd_router_t router) {
    struct httpd_server* server = handle;
    if (!server || !prefix || !router) {
        return HTTPD_ERR_INVALID_ARG;
    }

    if (server->mounted_router_count >= CONFIG_HTTPD_MAX_ROUTERS) {
        ESP_LOGE(TAG, "Maximum number of mounted routers reached (%d)",
                 CONFIG_HTTPD_MAX_ROUTERS);
        return HTTPD_ERR_NO_MEM;
    }

    // Validate prefix format
    if (prefix[0] != '/') {
        ESP_LOGE(TAG, "Mount prefix must start with '/'");
        return HTTPD_ERR_INVALID_ARG;
    }

    size_t prefix_len = strlen(prefix);
    if (prefix_len >= sizeof(server->mounted_routers[0].prefix)) {
        ESP_LOGE(TAG, "Mount prefix too long (max %zu)",
                 sizeof(server->mounted_routers[0].prefix) - 1);
        return HTTPD_ERR_INVALID_ARG;
    }

    // Store the mounted router
    mounted_router_t* entry = &server->mounted_routers[server->mounted_router_count];
    strncpy(entry->prefix, prefix, sizeof(entry->prefix) - 1);
    entry->prefix[sizeof(entry->prefix) - 1] = '\0';
    entry->prefix_len = prefix_len;
    entry->router = router;
    server->mounted_router_count++;

    ESP_LOGI(TAG, "Mounted router at '%s'", prefix);
    return HTTPD_OK;
}

httpd_err_t httpd_use(httpd_handle_t handle, httpd_middleware_t middleware) {
    struct httpd_server* server = handle;
    if (!server || !middleware) {
        return HTTPD_ERR_INVALID_ARG;
    }

    if (server->middleware_count >= MAX_MIDDLEWARES) {
        ESP_LOGE(TAG, "Server middleware limit reached (%d)", MAX_MIDDLEWARES);
        return HTTPD_ERR_NO_MEM;
    }

    server->middlewares[server->middleware_count++] = middleware;
    ESP_LOGI(TAG, "Added server-level middleware (count=%d)", server->middleware_count);
    return HTTPD_OK;
}

httpd_err_t httpd_on_error(httpd_handle_t handle, httpd_err_handler_t handler) {
    struct httpd_server* server = handle;
    if (!server || !handler) {
        return HTTPD_ERR_INVALID_ARG;
    }

    server->error_handler = handler;
    ESP_LOGI(TAG, "Set server error handler");
    return HTTPD_OK;
}

httpd_err_t httpd_unregister_route(httpd_handle_t handle, http_method_t method, const char* pattern) {
    (void)handle;
    (void)method;
    (void)pattern;
    // Route removal not supported with radix tree - routes are cleared on server stop
    ESP_LOGW(TAG, "httpd_unregister_route() not supported - routes cleared on httpd_stop()");
    return HTTPD_ERR_INVALID_ARG;  // Use existing error code
}

// ============================================================================
// Request Information
// ============================================================================

http_method_t httpd_req_get_method(httpd_req_t* req) {
    return req ? req->method : HTTP_GET;
}

const char* httpd_req_get_uri(httpd_req_t* req) {
    return req ? req->uri : NULL;
}

const char* httpd_req_get_path(httpd_req_t* req) {
    return req ? req->path : NULL;
}

const char* httpd_req_get_original_url(httpd_req_t* req) {
    return req ? req->original_url : NULL;
}

const char* httpd_req_get_base_url(httpd_req_t* req) {
    return req ? req->base_url : NULL;
}

const char* httpd_req_get_header(httpd_req_t* req, const char* key) {
    if (!req || !key) return NULL;

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    size_t key_len = strlen(key);

    // Pre-compute first char lowercase for fast filtering
    char first_lower = key[0] | 0x20;

    for (int i = 0; i < req->header_count; i++) {
        req_header_entry_t* entry = &ctx->headers[i];
        // Filter by length and first char before expensive str_casecmp
        if (entry->key_len == key_len &&
            (ctx->header_buf[entry->key_offset] | 0x20) == first_lower &&
            str_casecmp(&ctx->header_buf[entry->key_offset], key, key_len)) {
            return &ctx->header_buf[entry->value_offset];
        }
    }

    return NULL;
}

const char* httpd_req_get_param(httpd_req_t* req, const char* key) {
    if (!req || !key) return NULL;

    size_t key_len = strlen(key);
    for (int i = 0; i < req->param_count; i++) {
        if (req->params[i].key_len == key_len &&
            memcmp(req->params[i].key, key, key_len) == 0) {
            // Return pointer to value (it's null-terminated in uri_buf copy)
            return req->params[i].value;
        }
    }

    return NULL;
}

size_t httpd_req_get_content_length(httpd_req_t* req) {
    return req ? req->content_length : 0;
}

int httpd_req_recv(httpd_req_t* req, void* buf, size_t len) {
    if (!req || !buf || len == 0) return -1;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return -1;

    // Check if we have remaining content to receive
    size_t remaining = req->content_length - req->body_received;
    if (remaining == 0) return 0;

    // Get the request context (which contains pre-received body data)
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    size_t total_received = 0;

    // First, return any pre-received body data from the initial request buffer
    if (ctx->body_buf_pos < ctx->body_buf_len) {
        size_t pre_available = ctx->body_buf_len - ctx->body_buf_pos;
        size_t pre_to_copy = (len < pre_available) ? len : pre_available;
        if (pre_to_copy > remaining) pre_to_copy = remaining;

        memcpy(buf, &ctx->body_buf[ctx->body_buf_pos], pre_to_copy);
        ctx->body_buf_pos += pre_to_copy;
        req->body_received += pre_to_copy;
        total_received = pre_to_copy;

        // If we've filled the buffer or received all content, return
        if (total_received >= len || req->body_received >= req->content_length) {
            return (int)total_received;
        }
    }

    // If we need more data, receive from socket
    remaining = req->content_length - req->body_received;
    if (remaining > 0 && total_received < len) {
        size_t to_recv = (len - total_received) < remaining ? (len - total_received) : remaining;
        ssize_t received = recv(conn->fd, (char*)buf + total_received, to_recv, 0);

        if (received > 0) {
            req->body_received += received;
            total_received += received;
        } else if (received < 0 && total_received == 0) {
            return (int)received; // Return error only if we haven't returned anything
        }
    }

    return (int)total_received;
}

void* httpd_req_get_user_data(httpd_req_t* req) {
    return req ? req->user_data : NULL;
}

void httpd_req_set_user_data(httpd_req_t* req, void* data) {
    if (req) req->user_data = data;
}

// ============================================================================
// Response Building
// ============================================================================

httpd_err_t httpd_resp_set_status(httpd_req_t* req, int status) {
    if (!req) return HTTPD_ERR_INVALID_ARG;
    req->status_code = status;
    return HTTPD_OK;
}

httpd_err_t httpd_resp_set_header(httpd_req_t* req, const char* key, const char* value) {
    if (!req || !key || !value) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    // If headers not yet started, send status first
    if (!req->headers_sent) {
        char status_line[64];
        int len = snprintf(status_line, sizeof(status_line),
                          "HTTP/1.1 %d %s\r\n",
                          req->status_code, httpd_status_text(req->status_code));
        if (send_all(conn->fd, status_line, len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    char header[512];
    int len = snprintf(header, sizeof(header), "%s: %s\r\n", key, value);
    if (send_all(conn->fd, header, len, MSG_MORE) < 0) {
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) {
    return httpd_resp_set_header(req, "Content-Type", type);
}

httpd_err_t httpd_resp_send(httpd_req_t* req, const char* body, ssize_t len) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    size_t body_len = (len >= 0) ? (size_t)len : (body ? strlen(body) : 0);

    // Track if we're starting fresh (no headers sent yet)
    bool was_headers_fresh = !req->headers_sent;

    // Send status line if not sent
    if (!req->headers_sent) {
        char status_line[64];
        int slen = snprintf(status_line, sizeof(status_line),
                           "HTTP/1.1 %d %s\r\n",
                           req->status_code, httpd_status_text(req->status_code));
        if (send_nonblocking(conn, status_line, slen, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    // Only send Content-Length if this is a fresh response (headers just started)
    // or if there's actually body content to send.
    // If headers were already being sent (user called httpd_resp_set_header before),
    // they may have already set Content-Length manually.
    if (was_headers_fresh || body_len > 0) {
        char cl_header[64];
        int cl_len = snprintf(cl_header, sizeof(cl_header), "Content-Length: %zu\r\n", body_len);
        if (send_nonblocking(conn, cl_header, cl_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    // End headers
    if (send_nonblocking(conn, "\r\n", 2, MSG_MORE) < 0) {
        return HTTPD_ERR_IO;
    }

    // Send body - use blocking send to ensure complete delivery before handler returns.
    // Non-blocking send would queue data to be drained by on_write_ready, but the
    // connection state may change after the handler returns (e.g., ready for next
    // request in keep-alive), causing the queued data to never be sent.
    // For httpd_resp_send (Content-Length responses), blocking is the correct approach
    // since we've already committed to sending exactly body_len bytes.
    if (body && body_len > 0) {
        if (send_all(conn->fd, body, body_len, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* chunk, ssize_t len) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    // First chunk - send headers with Transfer-Encoding
    if (!req->headers_sent) {
        char status_line[64];
        int slen = snprintf(status_line, sizeof(status_line),
                           "HTTP/1.1 %d %s\r\n",
                           req->status_code, httpd_status_text(req->status_code));
        if (send_nonblocking(conn, status_line, slen, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        if (send_nonblocking(conn, "Transfer-Encoding: chunked\r\n", 28, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        if (send_nonblocking(conn, "\r\n", 2, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    size_t chunk_len = (len >= 0) ? (size_t)len : (chunk ? strlen(chunk) : 0);

    if (chunk_len == 0) {
        // Final chunk
        if (send_nonblocking(conn, "0\r\n\r\n", 5, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    } else {
        // Send chunk size in hex
        char size_line[16];
        int size_len = snprintf(size_line, sizeof(size_line), "%zx\r\n", chunk_len);
        if (send_nonblocking(conn, size_line, size_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        // Use non-blocking send for chunk data
        if (send_nonblocking(conn, chunk, chunk_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        if (send_nonblocking(conn, "\r\n", 2, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_error(httpd_req_t* req, int status, const char* message) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    req->status_code = status;
    const char* msg = message ? message : httpd_status_text(status);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, -1);
}

httpd_err_t httpd_resp_sendfile(httpd_req_t* req, const char* path) {
    if (!req || !path) return HTTPD_ERR_INVALID_ARG;

    if (!g_server || !g_server->filesystem_enabled) {
        return httpd_resp_send_error(req, 404, "File not found");
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (filesystem_serve_file(g_server->filesystem, conn, path, false) < 0) {
        return httpd_resp_send_error(req, 404, "File not found");
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_json(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

// ============================================================================
// Asynchronous Response Sending
// ============================================================================

httpd_err_t httpd_resp_send_async(httpd_req_t* req, const char* body, ssize_t len,
                                   httpd_send_cb_t on_done) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    request_context_t* ctx = get_request_context(conn);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    size_t body_len = (len >= 0) ? (size_t)len : (body ? strlen(body) : 0);

    // Build all headers in one buffer to minimize syscalls
    if (!req->headers_sent) {
        char header_buf[256];
        int header_len = snprintf(header_buf, sizeof(header_buf),
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            req->status_code, httpd_status_text(req->status_code),
            body_len);

        if (header_len < 0 || (size_t)header_len >= sizeof(header_buf)) {
            return HTTPD_ERR_NO_MEM;
        }

        // Single syscall for all headers
        if (send_nonblocking(conn, header_buf, header_len, body_len > 0 ? MSG_MORE : 0) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    } else {
        // Headers already partially sent, just send Content-Length and terminator
        char cl_buf[80];
        int cl_len = snprintf(cl_buf, sizeof(cl_buf), "Content-Length: %zu\r\n\r\n", body_len);
        if (send_nonblocking(conn, cl_buf, cl_len, body_len > 0 ? MSG_MORE : 0) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    // Queue body
    if (body && body_len > 0) {
        if (send_nonblocking(conn, body, body_len, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    // Set up async completion tracking
    ctx->async_send.on_done = on_done;
    ctx->async_send.active = true;

    return HTTPD_OK;
}

httpd_err_t httpd_resp_sendfile_async(httpd_req_t* req, const char* path,
                                       httpd_send_cb_t on_done) {
    if (!req || !path) return HTTPD_ERR_INVALID_ARG;

    if (!g_server || !g_server->filesystem_enabled) {
        if (on_done) on_done(req, HTTPD_ERR_NOT_FOUND);
        return HTTPD_ERR_NOT_FOUND;
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        if (on_done) on_done(req, HTTPD_ERR_CONN_CLOSED);
        return HTTPD_ERR_CONN_CLOSED;
    }

    // Note: filesystem_serve_file uses blocking I/O internally.
    // The file is sent synchronously before this function returns.
    int result = filesystem_serve_file(g_server->filesystem, conn, path, false);

    httpd_err_t err = (result < 0) ? HTTPD_ERR_NOT_FOUND : HTTPD_OK;

    // Invoke callback immediately since send completed synchronously
    if (on_done) {
        on_done(req, err);
    }

    return err;
}

// ============================================================================
// Data Provider API
// ============================================================================

httpd_err_t httpd_resp_send_provider(httpd_req_t* req, ssize_t content_length,
                                      httpd_data_provider_t provider,
                                      httpd_send_cb_t on_complete) {
    if (!req || !provider) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    request_context_t* ctx = get_request_context(conn);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    // Determine if using chunked encoding
    bool use_chunked = (content_length < 0);

    // Send headers - if headers not started yet, send status line first
    if (!req->headers_sent) {
        char status_line[64];
        int slen = snprintf(status_line, sizeof(status_line),
                           "HTTP/1.1 %d %s\r\n",
                           req->status_code, httpd_status_text(req->status_code));
        if (send_all(conn->fd, status_line, slen, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    // Send Content-Length or Transfer-Encoding header
    char header_buf[64];
    int header_len;
    if (use_chunked) {
        header_len = snprintf(header_buf, sizeof(header_buf), "Transfer-Encoding: chunked\r\n");
    } else {
        header_len = snprintf(header_buf, sizeof(header_buf), "Content-Length: %zd\r\n", content_length);
    }
    if (send_all(conn->fd, header_buf, header_len, MSG_MORE) < 0) {
        return HTTPD_ERR_IO;
    }

    // End headers
    if (send_all(conn->fd, "\r\n", 2, 0) < 0) {
        return HTTPD_ERR_IO;
    }

    // Allocate send buffer for the data provider
    send_buffer_t* sb = get_send_buffer(conn);
    if (!send_buffer_alloc(sb, &g_server->send_buffer_pool)) {
        ESP_LOGE(TAG, "Failed to allocate send buffer for provider");
        return HTTPD_ERR_NO_MEM;
    }

    // Set up data provider state
    ctx->data_provider.provider = provider;
    ctx->data_provider.on_complete = on_complete;
    ctx->data_provider.active = true;
    ctx->data_provider.eof_reached = false;
    ctx->data_provider.use_chunked = use_chunked;

    // Mark connection as write-pending to trigger on_write_ready
    connection_mark_write_pending(&g_server->connection_pool, conn->pool_index, true);

    ESP_LOGD(TAG, "Data provider started for conn [%d], chunked=%d", conn->pool_index, use_chunked);

    return HTTPD_OK;
}

// ============================================================================
// Request Body Handling
// ============================================================================

#define PIPE_BUFFER_SIZE 1024

ssize_t httpd_req_pipe_to_file(httpd_req_t* req, const char* path) {
    // Error codes are already negative, so return them directly
    if (!req || !path) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    // Check for Expect: 100-continue and respond if needed
    const char* expect = httpd_req_get_header(req, "Expect");
    if (expect && strcasecmp(expect, "100-continue") == 0) {
        httpd_resp_send_continue(req);
    }

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return HTTPD_ERR_IO;
    }

    char buf[PIPE_BUFFER_SIZE];
    ssize_t total_written = 0;
    int received;

    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        size_t written = fwrite(buf, 1, received, fp);
        if (written != (size_t)received) {
            ESP_LOGE(TAG, "Failed to write to file: %s", path);
            fclose(fp);
            return HTTPD_ERR_IO;
        }
        total_written += written;
    }

    fclose(fp);

    if (received < 0) {
        ESP_LOGE(TAG, "Error receiving request body");
        return HTTPD_ERR_IO;
    }

    ESP_LOGI(TAG, "Piped %zd bytes to file: %s", total_written, path);
    return total_written;
}

httpd_err_t httpd_resp_send_continue(httpd_req_t* req) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    // Use compile-time constant length instead of runtime strlen()
    static const char response[] = "HTTP/1.1 100 Continue\r\n\r\n";
    if (send_all(conn->fd, response, sizeof(response) - 1, 0) < 0) {
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

// ============================================================================
// Deferred (Async) Request Handling
// ============================================================================

// Internal context for httpd_req_defer_to_file
typedef struct {
    FILE* fp;
    httpd_done_cb_t user_done_cb;
} defer_file_ctx_t;

// Internal body callback for defer_to_file
static httpd_err_t defer_file_body_cb(httpd_req_t* req, const void* data, size_t len) {
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    defer_file_ctx_t* file_ctx = (defer_file_ctx_t*)ctx->defer.file_ctx;

    if (!file_ctx || !file_ctx->fp) {
        return HTTPD_ERR_IO;
    }

    size_t written = fwrite(data, 1, len, file_ctx->fp);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write to file: wrote %zu of %zu bytes", written, len);
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

// Internal done callback for defer_to_file
static void defer_file_done_cb(httpd_req_t* req, httpd_err_t err) {
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    defer_file_ctx_t* file_ctx = (defer_file_ctx_t*)ctx->defer.file_ctx;

    if (file_ctx) {
        if (file_ctx->fp) {
            fclose(file_ctx->fp);
        }
        httpd_done_cb_t user_cb = file_ctx->user_done_cb;
        free(file_ctx);
        ctx->defer.file_ctx = NULL;

        // Call user's done callback
        if (user_cb) {
            user_cb(req, err);
        }
    }
}

httpd_err_t httpd_req_defer(httpd_req_t* req, httpd_body_cb_t on_body, httpd_done_cb_t on_done) {
    if (!req || !on_done) {
        return HTTPD_ERR_INVALID_ARG;
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        return HTTPD_ERR_CONN_CLOSED;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    // Set up deferred state
    ctx->defer.on_body = on_body;
    ctx->defer.on_done = on_done;
    ctx->defer.active = true;
    ctx->defer.paused = false;
    conn->deferred = 1;
    conn->defer_paused = 0;

    // Transition to body state
    conn->state = CONN_STATE_HTTP_BODY;

    ESP_LOGD(TAG, "Request deferred, content_length=%zu, already_received=%zu",
             req->content_length, req->body_received);

    // Deliver any pre-received body data that came with headers
    if (ctx->body_buf_len > ctx->body_buf_pos && on_body) {
        size_t pre_data_len = ctx->body_buf_len - ctx->body_buf_pos;
        httpd_err_t err = on_body(req, &ctx->body_buf[ctx->body_buf_pos], pre_data_len);
        if (err != HTTPD_OK) {
            on_done(req, err);
            ctx->defer.active = false;
            conn->deferred = 0;
            return err;
        }
        req->body_received += pre_data_len;
        ctx->body_buf_pos = ctx->body_buf_len;  // Mark as consumed
    }

    // Check if body is already complete (small POST that fit in header buffer)
    if (req->content_length > 0 && req->body_received >= req->content_length) {
        ESP_LOGD(TAG, "Body already complete, calling on_done");
        on_done(req, HTTPD_OK);
        ctx->defer.active = false;
        conn->deferred = 0;
    }

    return HTTPD_OK;
}

httpd_err_t httpd_req_defer_pause(httpd_req_t* req) {
    if (!req) {
        return HTTPD_ERR_INVALID_ARG;
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        return HTTPD_ERR_CONN_CLOSED;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    if (!ctx->defer.active) {
        return HTTPD_ERR_INVALID_ARG;
    }

    ctx->defer.paused = true;
    conn->defer_paused = 1;
    ESP_LOGD(TAG, "Deferred request paused");
    return HTTPD_OK;
}

httpd_err_t httpd_req_defer_resume(httpd_req_t* req) {
    if (!req) {
        return HTTPD_ERR_INVALID_ARG;
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        return HTTPD_ERR_CONN_CLOSED;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    if (!ctx->defer.active) {
        return HTTPD_ERR_INVALID_ARG;
    }

    ctx->defer.paused = false;
    conn->defer_paused = 0;
    ESP_LOGD(TAG, "Deferred request resumed");
    return HTTPD_OK;
}

bool httpd_req_is_deferred(httpd_req_t* req) {
    if (!req) {
        return false;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    return ctx->defer.active;
}

httpd_err_t httpd_req_defer_to_file(httpd_req_t* req, const char* path, httpd_done_cb_t on_done) {
    if (!req || !path || !on_done) {
        return HTTPD_ERR_INVALID_ARG;
    }

    // Allocate file context
    defer_file_ctx_t* file_ctx = (defer_file_ctx_t*)calloc(1, sizeof(defer_file_ctx_t));
    if (!file_ctx) {
        return HTTPD_ERR_NO_MEM;
    }

    // Open file for writing
    file_ctx->fp = fopen(path, "wb");
    if (!file_ctx->fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        free(file_ctx);
        return HTTPD_ERR_IO;
    }

    file_ctx->user_done_cb = on_done;

    // Store context and set up defer
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    ctx->defer.file_ctx = file_ctx;

    ESP_LOGI(TAG, "Deferring body to file: %s (content_length=%zu)", path, req->content_length);

    return httpd_req_defer(req, defer_file_body_cb, defer_file_done_cb);
}

// ============================================================================
// Authentication
// ============================================================================

#include "mbedtls/base64.h"

bool httpd_check_basic_auth(httpd_req_t* req, const char* username, const char* password) {
    if (!req || !username || !password) return false;

    const char* auth_header = httpd_req_get_header(req, "Authorization");
    if (!auth_header) return false;

    // Check for "Basic " prefix
    if (strncmp(auth_header, "Basic ", 6) != 0) return false;

    const char* encoded = auth_header + 6;

    // Decode base64 credentials
    unsigned char decoded[256];
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                     (const unsigned char*)encoded, strlen(encoded));
    if (ret != 0) return false;

    decoded[decoded_len] = '\0';

    // Format is "username:password"
    char* colon = strchr((char*)decoded, ':');
    if (!colon) return false;

    // Split at colon
    *colon = '\0';
    const char* recv_user = (char*)decoded;
    const char* recv_pass = colon + 1;

    // Compare credentials (constant-time comparison would be more secure)
    if (strcmp(recv_user, username) != 0) return false;
    if (strcmp(recv_pass, password) != 0) return false;

    return true;
}

httpd_err_t httpd_resp_send_auth_challenge(httpd_req_t* req, const char* realm) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    const char* actual_realm = realm ? realm : "Restricted";

    req->status_code = 401;

    // Build WWW-Authenticate header value
    char auth_value[128];
    snprintf(auth_value, sizeof(auth_value), "Basic realm=\"%s\"", actual_realm);

    httpd_resp_set_header(req, "WWW-Authenticate", auth_value);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "401 Unauthorized", -1);
}

// ============================================================================
// WebSocket Operations
// ============================================================================

httpd_err_t httpd_ws_accept(httpd_req_t* req, httpd_ws_t** ws_out) {
    if (!req || !ws_out) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn || !g_server) return HTTPD_ERR_CONN_CLOSED;

    // Send WebSocket handshake response
    if (ws_send_handshake_response(conn->fd, req->ws_key) < 0) {
        return HTTPD_ERR_IO;
    }

    conn->state = CONN_STATE_WEBSOCKET;

    // Mark as active WebSocket - O(1) using pool_index
    connection_mark_ws_active(&g_server->connection_pool, conn->pool_index);

    // Setup WebSocket context
    ws_context_t* ws_ctx = get_ws_context(conn);
    ws_ctx->ws.fd = conn->fd;
    ws_ctx->ws._internal = conn;
    ws_ctx->ws.connected = true;

    // Initialize frame context with pre-allocated buffer
    ws_frame_ctx_init(&ws_ctx->frame_ctx);

    *ws_out = &ws_ctx->ws;
    return HTTPD_OK;
}

httpd_err_t httpd_ws_reject(httpd_req_t* req, int status, const char* reason) {
    return httpd_resp_send_error(req, status, reason);
}

httpd_err_t httpd_ws_send(httpd_ws_t* ws, const void* data, size_t len, ws_type_t type) {
    if (!ws || !ws->connected) return HTTPD_ERR_CONN_CLOSED;

    ws_opcode_internal_t opcode;
    switch (type) {
        case WS_TYPE_TEXT:   opcode = WS_OPCODE_TEXT; break;
        case WS_TYPE_BINARY: opcode = WS_OPCODE_BINARY; break;
        case WS_TYPE_PING:   opcode = WS_OPCODE_PING; break;
        case WS_TYPE_PONG:   opcode = WS_OPCODE_PONG; break;
        case WS_TYPE_CLOSE:  opcode = WS_OPCODE_CLOSE; break;
        default: return HTTPD_ERR_INVALID_ARG;
    }

    if (ws_send_frame(ws->fd, opcode, (const uint8_t*)data, len, false) < 0) {
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

httpd_err_t httpd_ws_send_text(httpd_ws_t* ws, const char* text) {
    return httpd_ws_send(ws, text, strlen(text), WS_TYPE_TEXT);
}

int httpd_ws_broadcast(httpd_handle_t handle, const char* pattern,
                       const void* data, size_t len, ws_type_t type) {
    struct httpd_server* server = handle;
    if (!server) return -1;

    ws_opcode_internal_t opcode = (type == WS_TYPE_BINARY) ? WS_OPCODE_BINARY : WS_OPCODE_TEXT;
    int sent = 0;

    // O(k) iteration where k = number of active WebSocket connections
    uint32_t mask = server->connection_pool.ws_active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);  // Get index of lowest set bit
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = &server->connection_pool.connections[i];
        // TODO: Check if connection matches pattern
        if (ws_send_frame(conn->fd, opcode, (const uint8_t*)data, len, false) >= 0) {
            sent++;
        }
    }

    return sent;
}

httpd_err_t httpd_ws_close(httpd_ws_t* ws, uint16_t code, const char* reason) {
    if (!ws) return HTTPD_ERR_INVALID_ARG;

    uint8_t close_data[128];
    size_t close_len = 2;
    close_data[0] = (code >> 8) & 0xFF;
    close_data[1] = code & 0xFF;

    if (reason) {
        size_t reason_len = strlen(reason);
        if (reason_len > sizeof(close_data) - 2) {
            reason_len = sizeof(close_data) - 2;
        }
        memcpy(&close_data[2], reason, reason_len);
        close_len += reason_len;
    }

    ws_send_frame(ws->fd, WS_OPCODE_CLOSE, close_data, close_len, false);
    ws->connected = false;

    return HTTPD_OK;
}

unsigned int httpd_ws_get_connection_count(httpd_handle_t handle) {
    struct httpd_server* server = handle;
    if (!server) return 0;

    // O(1) using popcount on ws_active_mask
    return __builtin_popcount(server->connection_pool.ws_active_mask);
}

void* httpd_ws_get_user_data(httpd_ws_t* ws) {
    return ws ? ws->user_data : NULL;
}

void httpd_ws_set_user_data(httpd_ws_t* ws, void* data) {
    if (ws) ws->user_data = data;
}

// ============================================================================
// WebSocket Channels
// ============================================================================

// Get ws_context from httpd_ws_t (reverse lookup via _internal pointer)
static ws_context_t* get_ws_context_from_ws(httpd_ws_t* ws) {
    if (!ws || !ws->_internal || !g_server) return NULL;
    connection_t* conn = (connection_t*)ws->_internal;
    return get_ws_context(conn);
}

// Find channel index by name using hash table - O(1) average case
static int find_channel(const char* channel) {
    if (!g_server || !channel) return -1;

    uint32_t hash = channel_hash_fn(channel);
    uint32_t bucket = hash & (CHANNEL_HASH_BUCKETS - 1);

    // Linear probing
    for (int probe = 0; probe < CHANNEL_HASH_BUCKETS; probe++) {
        uint32_t idx = (bucket + probe) & (CHANNEL_HASH_BUCKETS - 1);
        channel_hash_entry_t* entry = &g_server->channel_hash[idx];

        if (entry->index < 0) {
            return -1;  // Empty slot = not found
        }
        if (strcmp(entry->name, channel) == 0) {
            return entry->index;
        }
    }
    return -1;  // Table full (shouldn't happen)
}

// Find or create a channel index (-1 if full)
static int find_or_create_channel(const char* channel) {
    if (!g_server || !channel) return -1;

    uint32_t hash = channel_hash_fn(channel);
    uint32_t bucket = hash & (CHANNEL_HASH_BUCKETS - 1);

    // Search for existing or find empty slot
    for (int probe = 0; probe < CHANNEL_HASH_BUCKETS; probe++) {
        uint32_t idx = (bucket + probe) & (CHANNEL_HASH_BUCKETS - 1);
        channel_hash_entry_t* entry = &g_server->channel_hash[idx];

        if (entry->index < 0) {
            // Empty slot - create new channel if space available
            if (g_server->channel_count >= HTTPD_WS_MAX_CHANNELS) {
                return -1;
            }

            strncpy(entry->name, channel, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';
            entry->index = g_server->channel_count++;

            // Update index -> entry mapping
            g_server->channel_by_index[entry->index] = entry;

            return entry->index;
        }
        if (strcmp(entry->name, channel) == 0) {
            return entry->index;  // Already exists
        }
    }
    return -1;  // Table full (shouldn't happen with proper sizing)
}

httpd_err_t httpd_ws_join(httpd_ws_t* ws, const char* channel) {
    if (!ws || !channel) return HTTPD_ERR_INVALID_ARG;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    int idx = find_or_create_channel(channel);
    if (idx < 0) return HTTPD_ERR_NO_MEM;

    ctx->channel_mask |= (1u << idx);
    return HTTPD_OK;
}

httpd_err_t httpd_ws_leave(httpd_ws_t* ws, const char* channel) {
    if (!ws || !channel) return HTTPD_ERR_INVALID_ARG;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    int idx = find_channel(channel);
    if (idx < 0) return HTTPD_ERR_NOT_FOUND;

    if (!(ctx->channel_mask & (1u << idx))) {
        return HTTPD_ERR_NOT_FOUND;
    }

    ctx->channel_mask &= ~(1u << idx);
    return HTTPD_OK;
}

void httpd_ws_leave_all(httpd_ws_t* ws) {
    if (!ws) return;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (ctx) {
        ctx->channel_mask = 0;
    }
}

bool httpd_ws_in_channel(httpd_ws_t* ws, const char* channel) {
    if (!ws || !channel) return false;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return false;

    int idx = find_channel(channel);
    if (idx < 0) return false;

    return (ctx->channel_mask & (1u << idx)) != 0;
}

int httpd_ws_publish(httpd_handle_t handle, const char* channel,
                     const void* data, size_t len, ws_type_t type) {
    struct httpd_server* server = (struct httpd_server*)handle;
    if (!server || !channel || !data) return 0;

    int idx = find_channel(channel);
    if (idx < 0) return 0;

    uint32_t channel_mask = 1u << idx;
    int sent = 0;

    // Map type to opcode
    ws_opcode_internal_t opcode;
    switch (type) {
        case WS_TYPE_TEXT:   opcode = WS_OPCODE_TEXT; break;
        case WS_TYPE_BINARY: opcode = WS_OPCODE_BINARY; break;
        default: return 0;
    }

    // O(k) iteration where k = number of active WebSocket connections
    uint32_t ws_mask = server->connection_pool.ws_active_mask;
    while (ws_mask) {
        int i = __builtin_ctz(ws_mask);  // Get index of lowest set bit
        ws_mask &= ws_mask - 1;  // Clear lowest set bit

        ws_context_t* ctx = &ws_contexts[i];
        if (ctx->channel_mask & channel_mask) {
            connection_t* conn = &server->connection_pool.connections[i];
            if (ws_send_frame(conn->fd, opcode, (const uint8_t*)data, len, false) >= 0) {
                sent++;
            }
        }
    }

    return sent;
}

unsigned int httpd_ws_channel_size(httpd_handle_t handle, const char* channel) {
    struct httpd_server* server = (struct httpd_server*)handle;
    if (!server || !channel) return 0;

    int idx = find_channel(channel);
    if (idx < 0) return 0;

    uint32_t channel_mask = 1u << idx;
    unsigned int count = 0;

    // O(k) iteration where k = number of active WebSocket connections
    uint32_t ws_mask = server->connection_pool.ws_active_mask;
    while (ws_mask) {
        int i = __builtin_ctz(ws_mask);  // Get index of lowest set bit
        ws_mask &= ws_mask - 1;  // Clear lowest set bit

        ws_context_t* ctx = &ws_contexts[i];
        if (ctx->channel_mask & channel_mask) {
            count++;
        }
    }

    return count;
}

unsigned int httpd_ws_get_channels(httpd_ws_t* ws, const char** channels, unsigned int max_channels) {
    if (!ws || !channels || max_channels == 0) return 0;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx || !g_server) return 0;

    unsigned int count = 0;
    uint32_t mask = ctx->channel_mask;

    // Iterate using channel_by_index for O(k) where k = popcount(mask)
    while (mask && count < max_channels) {
        int idx = __builtin_ctz(mask);  // Get lowest set bit
        mask &= mask - 1;  // Clear lowest set bit

        channel_hash_entry_t* entry = g_server->channel_by_index[idx];
        if (entry) {
            channels[count++] = entry->name;
        }
    }

    return count;
}

// ============================================================================
// Middleware and Error Handling (Phase 4 & 7)
// ============================================================================

// Forward declaration for middleware chain
static httpd_err_t _middleware_next(httpd_req_t* req);

static httpd_err_t _middleware_next(httpd_req_t* req) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    // Execute next middleware in chain
    if (req->_mw.current < req->_mw.chain_len) {
        httpd_middleware_t mw = req->_mw.chain[req->_mw.current++];
        return mw(req, _middleware_next);
    }

    // Chain exhausted - call final handler
    if (req->_mw.final_handler) {
        req->user_data = req->_mw.final_user_ctx;
        return req->_mw.final_handler(req);
    }

    return HTTPD_OK;
}

static httpd_err_t handle_error(httpd_err_t err, httpd_req_t* req) {
    // Check router's error handler first
    if (req->_mw.router && req->_mw.router->error_handler) {
        httpd_err_t result = req->_mw.router->error_handler(err, req);
        if (result == HTTPD_OK) {
            return HTTPD_OK;
        }
    }

    // Fall back to server error handler
    if (g_server && g_server->error_handler) {
        httpd_err_t result = g_server->error_handler(err, req);
        if (result == HTTPD_OK) {
            return HTTPD_OK;
        }
    }

    // Default error response
    int status = 500;
    const char* message = NULL;

    switch (err) {
        case HTTPD_ERR_NOT_FOUND:
            status = 404;
            message = "Not Found";
            break;
        case HTTPD_ERR_INVALID_ARG:
            status = 400;
            message = "Bad Request";
            break;
        case HTTPD_ERR_NO_MEM:
            status = 503;
            message = "Service Unavailable";
            break;
        case HTTPD_ERR_MIDDLEWARE:
            status = 500;
            message = "Middleware Error";
            break;
        default:
            status = 500;
            message = "Internal Server Error";
            break;
    }

    return httpd_resp_send_error(req, status, message);
}

// ============================================================================
// Event Handlers
// ============================================================================

static void on_http_request(connection_t* conn, uint8_t* buffer, size_t len) {
    request_context_t* ctx = get_request_context(conn);
    if (!ctx) return;

    init_request_context(ctx, conn);

    // Set global parsing connection so headers can be stored
    g_parsing_connection = conn;

    // Parse request using http_parser
    http_parser_context_t parser_ctx = {0};
    parse_result_t result = http_parse_request(conn, buffer, len, &parser_ctx);

    // Clear global parsing connection
    g_parsing_connection = NULL;

    if (result == PARSE_ERROR) {
        httpd_resp_send_error(&ctx->req, 400, "Bad Request");
        return;
    }

    // Copy URL to request context
    if (parser_ctx.url && parser_ctx.url_len < sizeof(ctx->uri_buf)) {
        memcpy(ctx->uri_buf, parser_ctx.url, parser_ctx.url_len);
        ctx->uri_buf[parser_ctx.url_len] = '\0';
        ctx->req.uri = ctx->uri_buf;
        ctx->req.uri_len = parser_ctx.url_len;

        // Split path and query
        char* query = strchr(ctx->uri_buf, '?');
        if (query) {
            *query = '\0';
            ctx->req.path = ctx->uri_buf;
            ctx->req.path_len = query - ctx->uri_buf;
            ctx->req.query = query + 1;
            ctx->req.query_len = ctx->req.uri_len - ctx->req.path_len - 1;
        } else {
            ctx->req.path = ctx->uri_buf;
            ctx->req.path_len = ctx->req.uri_len;
        }
    }

    // Set method
    ctx->req.method = (http_method_t)conn->method;

    // Check for WebSocket upgrade
    ctx->req.is_websocket = conn->upgrade_ws;
    if (conn->upgrade_ws) {
        // Copy WebSocket key from global buffer (set by parser)
        snprintf(ctx->req.ws_key, sizeof(ctx->req.ws_key), "%s", ws_client_key);
    }

    // Store content length
    ctx->req.content_length = conn->content_length;

    // Save any body data that was received along with headers
    // conn->header_bytes contains the number of header bytes (including final CRLF)
    if (conn->content_length > 0 && len > conn->header_bytes) {
        size_t body_in_buffer = len - conn->header_bytes;
        size_t to_copy = body_in_buffer;
        if (to_copy > sizeof(ctx->body_buf)) {
            to_copy = sizeof(ctx->body_buf);
        }
        memcpy(ctx->body_buf, buffer + conn->header_bytes, to_copy);
        ctx->body_buf_len = to_copy;
        ctx->body_buf_pos = 0;
    }

    // Save original URL
    ctx->req.original_url = ctx->req.path;
    ctx->req.original_url_len = ctx->req.path_len;
    ctx->req.base_url = NULL;
    ctx->req.base_url_len = 0;

    // Check WebSocket routes first (before HTTP routes which may have catch-all patterns)
    if (ctx->req.is_websocket) {
        for (int i = 0; i < g_server->ws_route_count; i++) {
            if (strcmp(g_server->ws_routes[i].pattern, ctx->req.path) == 0) {
                // WebSocket route found - set up context
                ws_context_t* ws_ctx = get_ws_context(conn);
                ws_ctx->route = &g_server->ws_routes[i];
                ws_ctx->ws.fd = conn->fd;
                ws_ctx->ws._internal = conn;

                // Initialize frame context with pre-allocated buffer
                ws_frame_ctx_init(&ws_ctx->frame_ctx);

                // Send WebSocket handshake response FIRST (HTTP 101 Switching Protocols)
                // This MUST happen before the handler can send any WebSocket frames
                if (ws_send_handshake_response(conn->fd, ctx->req.ws_key) < 0) {
                    // Handshake failed - close connection
                    return;
                }
                conn->state = CONN_STATE_WEBSOCKET;
                ws_ctx->ws.connected = true;

                // Mark as active WebSocket - O(1) using pool_index
                connection_mark_ws_active(&g_server->connection_pool, conn->pool_index);

                // Now call handler with connect event
                httpd_ws_event_t event = {
                    .type = WS_EVENT_CONNECT,
                    .data = NULL,
                    .len = 0
                };
                ws_ctx->route->handler(&ws_ctx->ws, &event);
                return;
            }
        }
    }

    // Try mounted routers first
    bool route_found = false;
    radix_match_t match = {0};
    httpd_router_t matched_router = NULL;  // httpd_router_t is already a pointer

    // Fast path: single router case (common configuration)
    if (__builtin_expect(g_server->mounted_router_count == 1, 1)) {
        mounted_router_t* mr = &g_server->mounted_routers[0];
        if (ctx->req.path_len >= mr->prefix_len &&
            memcmp(ctx->req.path, mr->prefix, mr->prefix_len) == 0) {
            const char* stripped_path = ctx->req.path + mr->prefix_len;
            if (stripped_path[0] == '\0') stripped_path = "/";
            match = radix_lookup(mr->router->tree, stripped_path,
                               ctx->req.method, ctx->req.is_websocket);
            if (match.matched) {
                ctx->req.base_url = mr->prefix;
                ctx->req.base_url_len = mr->prefix_len;
                matched_router = mr->router;
                route_found = true;
            } else if (match.middlewares) {
                free(match.middlewares);
                match.middlewares = NULL;
                match.middleware_count = 0;
            }
        }
    } else {
        // Multiple routers - iterate
        for (uint8_t i = 0; i < g_server->mounted_router_count; i++) {
            mounted_router_t* mr = &g_server->mounted_routers[i];

            // Check if path starts with prefix
            if (ctx->req.path_len >= mr->prefix_len &&
                memcmp(ctx->req.path, mr->prefix, mr->prefix_len) == 0) {

                // Path matches prefix - look up in router's radix tree
                const char* stripped_path = ctx->req.path + mr->prefix_len;
                if (stripped_path[0] == '\0') {
                    stripped_path = "/";  // Handle exact prefix match
                }

                match = radix_lookup(mr->router->tree, stripped_path,
                                   ctx->req.method, ctx->req.is_websocket);

                if (match.matched) {
                    // Set base URL
                    ctx->req.base_url = mr->prefix;
                    ctx->req.base_url_len = mr->prefix_len;
                    matched_router = mr->router;
                    route_found = true;
                    break;
                }

                // Clean up middleware from failed match
                if (match.middlewares) {
                    free(match.middlewares);
                    match.middlewares = NULL;
                    match.middleware_count = 0;
                }
            }
        }
    }

    // Fall back to legacy route table if no router matched (O(log n) radix tree lookup)
    if (!route_found && g_server->legacy_routes) {
        radix_match_t legacy_match = radix_lookup(g_server->legacy_routes, ctx->req.path,
                                                   ctx->req.method, false);

        if (legacy_match.matched && legacy_match.handler) {
            route_found = true;

            // Copy parameters from radix match using memcpy (faster than field-by-field)
            uint8_t param_count = legacy_match.param_count < 8 ? legacy_match.param_count : 8;
            if (param_count > 0) {
                memcpy(ctx->req.params, legacy_match.params, param_count * sizeof(httpd_param_t));
            }
            ctx->req.param_count = legacy_match.param_count;

            // Build middleware chain: server global only (no router middleware for legacy routes)
            httpd_middleware_t mw_chain[MAX_MIDDLEWARES];
            uint8_t mw_count = g_server->middleware_count;

            // Use memcpy instead of loop for middleware chain copying
            if (mw_count > 0) {
                memcpy(mw_chain, g_server->middlewares, mw_count * sizeof(httpd_middleware_t));
            }

            // Set up middleware context
            ctx->req._mw.chain = mw_chain;
            ctx->req._mw.chain_len = mw_count;
            ctx->req._mw.current = 0;
            ctx->req._mw.final_handler = legacy_match.handler;
            ctx->req._mw.final_user_ctx = legacy_match.user_ctx;
            ctx->req._mw.router = NULL;

            // Execute middleware chain
            httpd_err_t err = (mw_count > 0) ? _middleware_next(&ctx->req) : legacy_match.handler(&ctx->req);
            if (err != HTTPD_OK) {
                handle_error(err, &ctx->req);
            }

            // Clean up allocated middleware from radix match
            if (legacy_match.middlewares) {
                free(legacy_match.middlewares);
            }
            return;
        }

        // Clean up if no match
        if (legacy_match.middlewares) {
            free(legacy_match.middlewares);
        }
    }

    // Handle routed request (from mounted router)
    if (route_found && match.matched) {
        // Copy parameters from radix match using memcpy (faster than field-by-field)
        uint8_t param_count = match.param_count < 8 ? match.param_count : 8;
        if (param_count > 0) {
            memcpy(ctx->req.params, match.params, param_count * sizeof(httpd_param_t));
        }
        ctx->req.param_count = match.param_count;

        // Build middleware chain: server global + router + route using memcpy
        httpd_middleware_t mw_chain[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];
        uint8_t mw_count = 0;

        // Add server global middleware
        uint8_t server_mw = g_server->middleware_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE
                           ? g_server->middleware_count : CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE;
        if (server_mw > 0) {
            memcpy(mw_chain, g_server->middlewares, server_mw * sizeof(httpd_middleware_t));
            mw_count = server_mw;
        }

        // Add router middleware
        if (matched_router && mw_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t router_mw = matched_router->middleware_count < avail
                               ? matched_router->middleware_count : avail;
            if (router_mw > 0) {
                memcpy(mw_chain + mw_count, matched_router->middlewares, router_mw * sizeof(httpd_middleware_t));
                mw_count += router_mw;
            }
        }

        // Add route middleware
        if (mw_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t route_mw = match.middleware_count < avail ? match.middleware_count : avail;
            if (route_mw > 0) {
                memcpy(mw_chain + mw_count, match.middlewares, route_mw * sizeof(httpd_middleware_t));
                mw_count += route_mw;
            }
        }

        // Set up middleware context
        ctx->req._mw.chain = mw_chain;
        ctx->req._mw.chain_len = mw_count;
        ctx->req._mw.current = 0;
        ctx->req._mw.router = matched_router;

        if (match.is_websocket) {
            // WebSocket route - handle differently
            // TODO: Implement WebSocket with new router system
            ESP_LOGW(TAG, "WebSocket routes not yet implemented in new router system");
        } else {
            ctx->req._mw.final_handler = match.handler;
            ctx->req._mw.final_user_ctx = match.user_ctx;

            // Execute middleware chain
            httpd_err_t err = (mw_count > 0) ? _middleware_next(&ctx->req) : match.handler(&ctx->req);
            if (err != HTTPD_OK) {
                handle_error(err, &ctx->req);
            }
        }

        // Clean up allocated middleware
        if (match.middlewares) {
            free(match.middlewares);
        }
        return;
    }

    // No route found
    handle_error(HTTPD_ERR_NOT_FOUND, &ctx->req);
}

static void on_http_body(connection_t* conn, uint8_t* buffer, size_t len) {
    // Quick check using connection bitfield - avoids context lookup for non-deferred
    // This is the hot path optimization: conn->deferred is O(1) vs get_request_context
    if (!conn->deferred) {
        // Non-deferred: body is received via blocking httpd_req_recv()
        return;
    }

    request_context_t* ctx = get_request_context(conn);
    if (!ctx || !ctx->defer.active) return;

    // Skip if paused (flow control)
    if (ctx->defer.paused) {
        ESP_LOGD(TAG, "Deferred body paused, ignoring %zu bytes", len);
        return;
    }

    // Call body callback if set
    if (ctx->defer.on_body) {
        httpd_err_t err = ctx->defer.on_body(&ctx->req, buffer, len);
        if (err != HTTPD_OK) {
            // Error in callback - call done with error and close
            ESP_LOGW(TAG, "Deferred body callback returned error: %d", err);
            if (ctx->defer.on_done) {
                ctx->defer.on_done(&ctx->req, err);
            }
            ctx->defer.active = false;
            conn->deferred = 0;
            conn->state = CONN_STATE_CLOSING;
            return;
        }
    }

    // Track received bytes
    ctx->req.body_received += len;

    ESP_LOGD(TAG, "Deferred body: received %zu bytes, total %zu/%zu",
             len, ctx->req.body_received, ctx->req.content_length);

    // Check if body is complete
    if (ctx->req.content_length > 0 &&
        ctx->req.body_received >= ctx->req.content_length) {
        ESP_LOGD(TAG, "Deferred body complete, calling on_done");
        if (ctx->defer.on_done) {
            ctx->defer.on_done(&ctx->req, HTTPD_OK);
        }
        ctx->defer.active = false;
        conn->deferred = 0;
        // Connection stays open for the response to be sent
    }
}

static void on_ws_frame(connection_t* conn, uint8_t* buffer, size_t len) {
    ws_context_t* ws_ctx = get_ws_context(conn);
    if (!ws_ctx || !ws_ctx->route) return;

    // Process all WebSocket frames in the buffer
    size_t offset = 0;
    while (offset < len) {
        size_t bytes_consumed = 0;
        ws_frame_result_t result = ws_process_frame(conn, buffer + offset, len - offset,
                                                     &ws_ctx->frame_ctx,
                                                     &bytes_consumed);

        if (bytes_consumed == 0) {
            // No progress - need more data
            break;
        }

        offset += bytes_consumed;

        if (result == WS_FRAME_COMPLETE) {
            // Frame complete - deliver message to handler
            ws_opcode_internal_t opcode = (ws_opcode_internal_t)conn->ws_opcode;

            // Map internal opcode to public type
            ws_type_t frame_type;
            switch (opcode) {
                case WS_OPCODE_TEXT:   frame_type = WS_TYPE_TEXT; break;
                case WS_OPCODE_BINARY: frame_type = WS_TYPE_BINARY; break;
                case WS_OPCODE_CLOSE:  frame_type = WS_TYPE_CLOSE; break;
                case WS_OPCODE_PING:   frame_type = WS_TYPE_PING; break;
                case WS_OPCODE_PONG:   frame_type = WS_TYPE_PONG; break;
                default:               frame_type = WS_TYPE_TEXT; break;
            }

            httpd_ws_event_t event = {
                .type = WS_EVENT_MESSAGE,
                .data = ws_ctx->frame_ctx.payload_buffer,
                .len = ws_ctx->frame_ctx.payload_received,
                .frame_type = frame_type
            };

            ws_ctx->route->handler(&ws_ctx->ws, &event);

            // Reset frame context for next frame
            ws_ctx->frame_ctx.state = WS_STATE_OPCODE;
            ws_ctx->frame_ctx.payload_received = 0;
        } else if (result == WS_FRAME_CLOSE) {
            // Close frame received - disconnect will be handled by event loop
            break;
        } else if (result == WS_FRAME_NEED_MORE) {
            // Need more data - wait for next read
            break;
        }
    }
}

static void on_ws_connect(connection_t* conn) {
    // Handled in on_http_request when upgrade detected
    (void)conn;
}

static void on_ws_disconnect(connection_t* conn) {
    ws_context_t* ws_ctx = get_ws_context(conn);
    if (!ws_ctx || !ws_ctx->route) return;

    httpd_ws_event_t event = {
        .type = WS_EVENT_DISCONNECT,
        .data = NULL,
        .len = 0
    };

    ws_ctx->route->handler(&ws_ctx->ws, &event);
    ws_ctx->ws.connected = false;
    ws_ctx->channel_mask = 0;  // Clear all channel subscriptions

    // Clear active WebSocket flag - O(1) using pool_index
    if (g_server) {
        connection_mark_ws_inactive(&g_server->connection_pool, conn->pool_index);
    }
}

static void on_disconnect(connection_t* conn) {
    request_context_t* ctx = get_request_context(conn);

    // Handle disconnect for deferred requests
    if (conn->deferred) {
        if (ctx && ctx->defer.active && ctx->defer.on_done) {
            ESP_LOGW(TAG, "Connection closed during deferred request");
            ctx->defer.on_done(&ctx->req, HTTPD_ERR_CONN_CLOSED);
            ctx->defer.active = false;
            conn->deferred = 0;
        }
    }

    // Handle disconnect for async send
    if (ctx && ctx->async_send.active) {
        httpd_send_cb_t callback = ctx->async_send.on_done;
        ctx->async_send.active = false;
        ctx->async_send.on_done = NULL;
        if (callback) {
            ESP_LOGW(TAG, "Connection closed during async send");
            callback(&ctx->req, HTTPD_ERR_CONN_CLOSED);
        }
    }

    // Handle disconnect for data provider
    if (ctx && ctx->data_provider.active) {
        httpd_send_cb_t callback = ctx->data_provider.on_complete;
        ctx->data_provider.active = false;
        ctx->data_provider.provider = NULL;
        ctx->data_provider.on_complete = NULL;
        ctx->data_provider.eof_reached = false;
        ctx->data_provider.use_chunked = false;
        if (callback) {
            ESP_LOGW(TAG, "Connection closed during data provider send");
            callback(&ctx->req, HTTPD_ERR_CONN_CLOSED);
        }
    }

    // Clean up send buffer
    send_buffer_t* sb = get_send_buffer(conn);
    if (sb->allocated) {
        send_buffer_free(sb, &g_server->send_buffer_pool);
    }
}

// Called by event loop when socket is writable and has pending data
static void on_write_ready(connection_t* conn) {
    send_buffer_t* sb = get_send_buffer(conn);
    request_context_t* ctx = get_request_context(conn);

    // Main send loop - keep filling and sending until EAGAIN or complete
    for (;;) {
        // If streaming a file, refill the buffer first (zero-copy: read directly into ring buffer)
    if (send_buffer_is_streaming(sb)) {
        uint8_t* write_ptr;
        size_t contiguous = send_buffer_write_ptr(sb, &write_ptr);

        if (contiguous > 0 && sb->file_remaining > 0) {
            size_t to_read = contiguous;
            if (to_read > sb->file_remaining) {
                to_read = sb->file_remaining;
            }

            // Read directly into ring buffer - no intermediate copy
            ssize_t bytes_read = read(sb->file_fd, write_ptr, to_read);
            if (bytes_read > 0) {
                send_buffer_commit(sb, bytes_read);
                sb->file_remaining -= bytes_read;

                // Check if file is complete
                if (sb->file_remaining == 0) {
                    send_buffer_stop_file(sb);
                    ESP_LOGD(TAG, "File streaming complete for conn [%d]", conn->pool_index);
                }
            } else if (bytes_read < 0 && errno != EAGAIN) {
                ESP_LOGE(TAG, "File read error: %s", strerror(errno));
                send_buffer_stop_file(sb);
            }
        }
    }

    // If data provider active and not EOF, refill buffer from provider
    if (ctx && ctx->data_provider.active && !ctx->data_provider.eof_reached) {
        uint8_t* write_ptr;
        size_t contiguous = send_buffer_write_ptr(sb, &write_ptr);

        if (contiguous > 0) {
            // For chunked encoding, reserve space for chunk header (max "FFFF\r\n" = 8 bytes)
            // and trailer ("\r\n" = 2 bytes)
            size_t reserved = ctx->data_provider.use_chunked ? 10 : 0;
            if (contiguous > reserved) {
                size_t max_data = contiguous - reserved;
                uint8_t* data_ptr = ctx->data_provider.use_chunked ? write_ptr + 8 : write_ptr;

                // Call user's data provider
                ssize_t bytes = ctx->data_provider.provider(&ctx->req, data_ptr, max_data);

                if (bytes > 0) {
                    if (ctx->data_provider.use_chunked) {
                        // Format chunk: size\r\n data \r\n
                        int header_len = snprintf((char*)write_ptr, 8, "%zx\r\n", bytes);
                        // Move data if header is shorter than 8 bytes
                        if (header_len < 8) {
                            memmove(write_ptr + header_len, data_ptr, bytes);
                        }
                        // Add chunk trailer
                        write_ptr[header_len + bytes] = '\r';
                        write_ptr[header_len + bytes + 1] = '\n';
                        send_buffer_commit(sb, header_len + bytes + 2);
                    } else {
                        send_buffer_commit(sb, bytes);
                    }
                } else if (bytes == 0) {
                    // EOF - provider has no more data
                    ctx->data_provider.eof_reached = true;
                    ESP_LOGD(TAG, "Data provider EOF for conn [%d]", conn->pool_index);

                    // For chunked encoding, queue the final chunk
                    if (ctx->data_provider.use_chunked) {
                        send_buffer_queue(sb, "0\r\n\r\n", 5);
                    }
                } else {
                    // Error from provider
                    ESP_LOGE(TAG, "Data provider error: %zd", bytes);
                    httpd_send_cb_t callback = ctx->data_provider.on_complete;
                    ctx->data_provider.active = false;
                    ctx->data_provider.provider = NULL;
                    ctx->data_provider.on_complete = NULL;
                    if (callback) {
                        callback(&ctx->req, (httpd_err_t)bytes);
                    }
                    conn->state = CONN_STATE_CLOSED;
                    return;
                }
            }
        }
    }

    // Try to send buffered data
    while (send_buffer_has_data(sb)) {
        const uint8_t* data;
        size_t len = send_buffer_peek(sb, &data);
        if (len == 0) break;

        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, will retry on next write-ready
                return;
            }
            // Real error - invoke async callback with error and close connection
            ESP_LOGE(TAG, "Send error on conn [%d]: %s", conn->pool_index, strerror(errno));
            request_context_t* ctx = get_request_context(conn);
            if (ctx && ctx->async_send.active) {
                httpd_send_cb_t callback = ctx->async_send.on_done;
                ctx->async_send.active = false;
                ctx->async_send.on_done = NULL;
                if (callback) {
                    callback(&ctx->req, HTTPD_ERR_IO);
                }
            }
            conn->state = CONN_STATE_CLOSED;
            return;
        }

        send_buffer_consume(sb, sent);
    }

        // Check if data provider is still active and needs more calls
        bool provider_needs_more = ctx && ctx->data_provider.active && !ctx->data_provider.eof_reached;

        // If provider needs more data, continue the loop to refill and send
        if (provider_needs_more) {
            continue;
        }

        // If file streaming needs more data, continue the loop
        if (send_buffer_is_streaming(sb)) {
            continue;
        }

        // No more data to generate - exit the loop
        break;
    }  // End of for(;;) loop

    // All data sent and no more file/provider data, clear write pending
    if (!send_buffer_has_data(sb)) {
        connection_mark_write_pending(&g_server->connection_pool, conn->pool_index, false);

        // If buffer was allocated but now empty, we can free it
        if (sb->allocated) {
            send_buffer_free(sb, &g_server->send_buffer_pool);
        }

        // Check for async send completion
        request_context_t* ctx = get_request_context(conn);
        if (ctx && ctx->async_send.active) {
            httpd_send_cb_t callback = ctx->async_send.on_done;
            ctx->async_send.active = false;
            ctx->async_send.on_done = NULL;

            if (callback) {
                callback(&ctx->req, HTTPD_OK);
            }
        }

        // Check for data provider completion
        if (ctx && ctx->data_provider.active && ctx->data_provider.eof_reached) {
            httpd_send_cb_t callback = ctx->data_provider.on_complete;
            ctx->data_provider.active = false;
            ctx->data_provider.provider = NULL;
            ctx->data_provider.on_complete = NULL;
            ctx->data_provider.eof_reached = false;
            ctx->data_provider.use_chunked = false;

            ESP_LOGD(TAG, "Data provider complete for conn [%d]", conn->pool_index);

            if (callback) {
                callback(&ctx->req, HTTPD_OK);
            }
        }
    }
}
