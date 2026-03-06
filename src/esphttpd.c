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
#ifdef CONFIG_HTTPD_USE_RAW_API
#include "private/raw_tcp.h"
#include "lwip/tcpip.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
// In test mode, tcpip_thread isn't running so LOCK/UNLOCK are no-ops
#ifdef CONFIG_ESPHTTPD_TEST_MODE
#define HTTPD_LOCK_TCPIP()   do {} while(0)
#define HTTPD_UNLOCK_TCPIP() do {} while(0)
#else
#define HTTPD_LOCK_TCPIP()   LOCK_TCPIP_CORE()
#define HTTPD_UNLOCK_TCPIP() UNLOCK_TCPIP_CORE()
#endif
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#ifndef CONFIG_HTTPD_USE_RAW_API
#include <sys/socket.h>
#else
// Define socket flags used as send_nonblocking parameters
// Under raw API these are translated to raw_tcp_write flags
#ifndef MSG_MORE
#define MSG_MORE 0x8000
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40
#endif
#endif
#include <errno.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char TAG[] = "ESPHTTPD";

// ============================================================================
// Internal Structures
// ============================================================================

// Configuration defaults (overridden by Kconfig)
#ifndef CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES
#define CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES 4
#endif
#ifndef CONFIG_HTTPD_MAX_WS_ROUTES
#define CONFIG_HTTPD_MAX_WS_ROUTES 8
#endif
#ifndef CONFIG_HTTPD_MAX_REQ_HEADERS
#define CONFIG_HTTPD_MAX_REQ_HEADERS 16
#endif
#ifndef CONFIG_HTTPD_MAX_QUERY_PARAMS
#define CONFIG_HTTPD_MAX_QUERY_PARAMS 8
#endif
#ifndef CONFIG_HTTPD_MAX_ROUTERS
#define CONFIG_HTTPD_MAX_ROUTERS 8
#endif
#ifndef CONFIG_HTTPD_MAX_WS_CHANNELS
#define CONFIG_HTTPD_MAX_WS_CHANNELS 16
#endif

#define MAX_MIDDLEWARES CONFIG_HTTPD_MAX_SERVER_MIDDLEWARES
#define MAX_WS_ROUTES CONFIG_HTTPD_MAX_WS_ROUTES
#define REQ_HEADER_BUF_SIZE 2048
#define MAX_REQ_HEADERS CONFIG_HTTPD_MAX_REQ_HEADERS
#define MAX_QUERY_PARAMS CONFIG_HTTPD_MAX_QUERY_PARAMS

// Header entry for per-request storage
typedef struct {
    uint16_t key_offset;
    uint16_t value_offset;
    uint8_t key_len;
    uint8_t value_len;
} req_header_entry_t;

// HTTP route entry (new API)
typedef struct {
    const char* pattern;
    http_method_t method;
    httpd_handler_t handler;
    void* user_ctx;
    bool has_params;           // Pattern contains :param
} httpd_route_entry_t;

// WebSocket route entry (new API)
typedef struct {
    const char* pattern;
    httpd_ws_handler_t handler;
    void* user_ctx;
    uint32_t ping_interval_ms;
} httpd_ws_route_entry_t;

// Mounted router entry
typedef struct {
    const char* prefix;
    uint8_t prefix_len;
    httpd_router_t router;  // httpd_router_t is already a pointer type
} mounted_router_t;

// Channel hash table entry (replaces linked list for O(1) lookup)
typedef struct {
    char name[16];              // Channel name (empty string if unused)
    int8_t index;               // Bitmask index (0-31), -1 if empty
    uint8_t subscriber_count;   // Number of connections subscribed to this channel
} channel_hash_entry_t;

#define CHANNEL_HASH_BUCKETS 32  // Power of 2, >= MAX_CHANNELS for acceptable collision

// FNV-1a hash for channel names
static uint32_t channel_hash_fn(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

// Format size_t as hex with \r\n suffix. Returns total bytes written.
static inline int format_hex(char* buf, size_t value) {
    static const char hex_chars[] = "0123456789abcdef";
    char tmp[8]; int n = 0;
    do { tmp[n++] = hex_chars[value & 0xF]; value >>= 4; } while (value);
    for (int i = n - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf++ = '\r'; *buf++ = '\n';
    return n + 2;
}

// Format size_t as decimal digits. Returns number of digits written.
static inline int format_uint(char* buf, size_t value) {
    char tmp[16]; int n = 0;
    do { tmp[n++] = '0' + (value % 10); value /= 10; } while (value);
    for (int i = n - 1; i >= 0; i--) *buf++ = tmp[i];
    return n;
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

    // WebSocket channel registry (hash table for O(1) lookup) - lazy allocated
    channel_hash_entry_t* channel_hash;             // Hash table (NULL until first channel join)
    channel_hash_entry_t* channel_by_index[HTTPD_WS_MAX_CHANNELS];  // Index -> entry mapping
    uint8_t channel_count;                          // Next available channel index

    // State
    bool initialized;
    bool running;
};

// Free channel hash table if allocated
static inline void free_channel_hash(struct httpd_server* server) {
    if (server->channel_hash) {
        free(server->channel_hash);
        server->channel_hash = NULL;
    }
    memset(server->channel_by_index, 0, sizeof(server->channel_by_index));
    server->channel_count = 0;
}

// Lazy-allocate channel hash table (only when first channel is used)
// Returns true if hash is available (already allocated or newly allocated)
static inline bool ensure_channel_hash(struct httpd_server* server) {
    if (server->channel_hash) return true;

    server->channel_hash = (channel_hash_entry_t*)malloc(
        CHANNEL_HASH_BUCKETS * sizeof(channel_hash_entry_t));
    if (!server->channel_hash) {
        ESP_LOGE(TAG, "Failed to allocate channel hash table");
        return false;
    }

    // Initialize: set all slots to empty
    for (int i = 0; i < CHANNEL_HASH_BUCKETS; i++) {
        server->channel_hash[i].index = -1;
        server->channel_hash[i].name[0] = '\0';
        server->channel_hash[i].subscriber_count = 0;
    }
    memset(server->channel_by_index, 0, sizeof(server->channel_by_index));
    server->channel_count = 0;
    return true;
}

// Initialize/reset channel state (no allocation - lazy)
static inline void init_channel_hash(struct httpd_server* server) {
    free_channel_hash(server);
}

// Query parameter cache entry (pointers into query string)
typedef struct {
    const char* key;
    const char* value;
    uint8_t key_len;
    uint8_t value_len;
} query_param_entry_t;

// Per-connection request context
// Layout: fields that need zeroing per-request are grouped first (up to _zero_end),
// followed by scratch buffers that don't need zeroing. init_request_context() only
// memsets up to _zero_end, saving ~768 bytes of unnecessary zeroing per request.
typedef struct {
    // === Fields that NEED zeroing per request (memset target) ===
    httpd_req_t req;                      // Public request struct
    char* header_buf;                     // Header storage (dynamically allocated)
    char* uri_buf;                        // URI storage (dynamically allocated)
    struct httpd_server* server;          // Back pointer to server
    httpd_route_entry_t* matched_route;   // Matched route
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
    // (only accessed up to their respective counts, or written before read)
    req_header_entry_t headers[MAX_REQ_HEADERS];  // Header index (accessed up to header_count)
    uint8_t inline_recv_buf[512];         // Embedded buffer for single-packet requests
    char inline_uri_buf[64];              // Embedded buffer for typical URI lengths (heap fallback for longer)
    query_param_entry_t query_params[MAX_QUERY_PARAMS];  // Lazy parsed (accessed up to query_param_count)
    httpd_middleware_t mw_chain[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];  // Accessed up to middleware count
} request_context_t;

// Per-connection WebSocket context
typedef struct {
    httpd_ws_t ws;                        // Public WebSocket struct
    httpd_ws_route_entry_t* route;        // Associated route
    ws_frame_context_t frame_ctx;         // Frame parsing context
    uint32_t channel_mask;                // Bitmask of subscribed channels
    uint8_t route_index;                  // Index into ws_routes[] for O(1) broadcast filter
} ws_context_t;

// Per-connection contexts (pointer arrays into pre-allocated backing storage)
static request_context_t* request_contexts[MAX_CONNECTIONS];
static ws_context_t* ws_contexts[MAX_CONNECTIONS];
static send_buffer_t* connection_send_buffers[MAX_CONNECTIONS];

// Pre-allocated backing arrays (allocated once at httpd_start, freed at httpd_stop)
// Eliminates per-connect/disconnect malloc/free and prevents heap fragmentation
static request_context_t* preallocated_request_contexts;
static ws_context_t* preallocated_ws_contexts;
static send_buffer_t* preallocated_send_buffers;

// Global server instance (for now - could be made multi-instance later)
static struct httpd_server server_instance;
#ifdef CONFIG_ESPHTTPD_TEST_MODE
struct httpd_server* g_server = NULL;  // Non-static for test access
void* g_test_request_contexts = NULL;  // Non-static for test access
void* g_test_send_buffers = NULL;      // Non-static for test access
#else
static struct httpd_server* g_server = NULL;
#endif
static filesystem_t fs_instance;

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
static void on_connect(connection_t* conn);
static void on_ws_connect(connection_t* conn);
static void on_ws_disconnect(connection_t* conn);
static void on_disconnect(connection_t* conn);
static void on_write_ready(connection_t* conn);
#ifndef CONFIG_HTTPD_USE_RAW_API
static void server_task(void* pvParameters);
#endif

// Get send buffer for connection (returns NULL if not allocated)
static inline __attribute__((always_inline)) send_buffer_t* get_send_buffer(connection_t* conn) {
    return connection_send_buffers[conn->pool_index];
}

// ============================================================================
// File I/O Worker (raw API mode only)
// ============================================================================
#ifdef CONFIG_HTTPD_USE_RAW_API

// File read request sent to the worker task
typedef struct {
    uint8_t pool_index;      // Connection pool index
    int file_fd;             // File descriptor to read from
    uint8_t* dest;           // Destination buffer (inside send_buffer ring)
    size_t max_len;          // Maximum bytes to read
} file_io_request_t;

// File read result returned from worker
typedef struct {
    uint8_t pool_index;      // Connection pool index
    ssize_t bytes_read;      // Bytes read, or -1 on error
} file_io_result_t;

static QueueHandle_t s_file_io_request_queue = NULL;
static QueueHandle_t s_file_io_result_queue = NULL;
static TaskHandle_t s_file_io_task = NULL;

// Worker task: reads file data outside tcpip_thread
static void file_io_worker_task(void* pvParameters) {
    file_io_request_t req;

    for (;;) {
        if (xQueueReceive(s_file_io_request_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        file_io_result_t result = {
            .pool_index = req.pool_index,
            .bytes_read = read(req.file_fd, req.dest, req.max_len)
        };

        // Post result back and trigger on_write_ready via tcpip callback
        xQueueSend(s_file_io_result_queue, &result, portMAX_DELAY);

        // Signal tcpip_thread to process the result
        HTTPD_LOCK_TCPIP();
        if (g_server) {
            connection_t* conn = connection_get(&g_server->connection_pool, req.pool_index);
            if (conn && connection_is_active(&g_server->connection_pool, req.pool_index)) {
                send_buffer_t* sb = get_send_buffer(conn);
                if (sb && result.bytes_read > 0) {
                    send_buffer_commit(sb, result.bytes_read);
                    sb->file_remaining -= result.bytes_read;
                    if (sb->file_remaining == 0) {
                        send_buffer_stop_file(sb);
                    }
                } else if (result.bytes_read < 0) {
                    if (sb) send_buffer_stop_file(sb);
                }
                on_write_ready(conn);
            }
        }
        HTTPD_UNLOCK_TCPIP();
    }
}

static void file_io_worker_start(void) {
    if (s_file_io_task) return;  // Already running

    s_file_io_request_queue = xQueueCreate(4, sizeof(file_io_request_t));
    s_file_io_result_queue = xQueueCreate(4, sizeof(file_io_result_t));

    xTaskCreate(file_io_worker_task, "httpd_fio", 2048, NULL, 5, &s_file_io_task);
    ESP_LOGI(TAG, "File I/O worker task started");
}

static void file_io_worker_stop(void) {
    if (s_file_io_task) {
        vTaskDelete(s_file_io_task);
        s_file_io_task = NULL;
    }
    if (s_file_io_request_queue) {
        vQueueDelete(s_file_io_request_queue);
        s_file_io_request_queue = NULL;
    }
    if (s_file_io_result_queue) {
        vQueueDelete(s_file_io_result_queue);
        s_file_io_result_queue = NULL;
    }
}

// Submit a file read to the worker task (non-blocking)
static bool file_io_submit_read(uint8_t pool_index, int file_fd, uint8_t* dest, size_t max_len) {
    if (!s_file_io_request_queue) {
        file_io_worker_start();
    }
    if (!s_file_io_request_queue) return false;

    file_io_request_t req = {
        .pool_index = pool_index,
        .file_fd = file_fd,
        .dest = dest,
        .max_len = max_len
    };

    return xQueueSend(s_file_io_request_queue, &req, 0) == pdTRUE;
}

#endif // CONFIG_HTTPD_USE_RAW_API

// ============================================================================
// Utility Functions
// ============================================================================

// Drain send buffer - sends as much buffered data as possible
// Returns true if buffer is now empty, false if more data pending
static bool drain_send_buffer(connection_t* conn) {
    send_buffer_t* sb = get_send_buffer(conn);
    if (!sb) return true;  // No buffer, nothing to drain

    while (send_buffer_has_data(sb)) {
        const uint8_t* data;
        size_t len = send_buffer_peek(sb, &data);
        if (len == 0) break;

#ifdef CONFIG_HTTPD_USE_RAW_API
        ssize_t sent = raw_tcp_write(conn, data, len, false);
        if (sent <= 0) {
            return false;  // No space or error
        }
#else
        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // Socket buffer full, more data pending
            }
            ESP_LOGE(TAG, "drain_send_buffer failed: %s", strerror(errno));
            return false;
        }
#endif
        send_buffer_consume(sb, sent);
    }

    return !send_buffer_has_data(sb);
}

// Non-blocking send - tries to send data, queues remainder if socket/tcp buffer would block
// Returns number of bytes sent/queued, or -1 on error
static ssize_t send_nonblocking(connection_t* __restrict conn, const void* __restrict data, size_t len, int flags) {
    if (!conn || !data || len == 0) {
        return 0;
    }

    send_buffer_t* sb = get_send_buffer(conn);
    if (!sb) {
        ESP_LOGE(TAG, "No send buffer for connection");
        return -1;
    }
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
#ifdef CONFIG_HTTPD_USE_RAW_API
        bool more = (flags & MSG_MORE) != 0;
        ssize_t sent = raw_tcp_write(conn, ptr, remaining, more);
        if (sent <= 0) {
            if (sent == 0) {
                // No space available, queue remaining data
                goto queue_data;
            }
            ESP_LOGE(TAG, "send_nonblocking failed: raw_tcp_write error");
            return -1;
        }
#else
        ssize_t sent = send(conn->fd, ptr, remaining, flags | MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, queue remaining data
                goto queue_data;
            }
            ESP_LOGE(TAG, "send_nonblocking failed: %s", strerror(errno));
            return -1;
        }
#endif
        ptr += sent;
        remaining -= sent;
    }

    // All data sent directly
    return (ssize_t)len;

queue_data:
    // Ensure we have a buffer allocated
    if (!sb->allocated) {
        if (!send_buffer_alloc(sb)) {
            ESP_LOGE(TAG, "Failed to allocate send buffer");
            return -1;
        }
    }

    // Queue data in chunks when remaining exceeds buffer capacity.
    // Between chunks, drain the buffer to the socket to free space.
    while (remaining > 0) {
        size_t space = send_buffer_space(sb);
        if (space == 0) {
            // Buffer full - drain to socket to make room
            drain_send_buffer(conn);
            space = send_buffer_space(sb);
            if (space == 0) {
                // Socket also full - no progress possible
                ESP_LOGE(TAG, "Send buffer full, cannot queue %zu bytes", remaining);
                return -1;
            }
        }
        size_t to_queue = (remaining <= space) ? remaining : space;
        if (send_buffer_queue(sb, ptr, to_queue) < 0) {
            return -1;
        }
        ptr += to_queue;
        remaining -= to_queue;
    }

    // Mark connection as having pending writes
    connection_mark_write_pending(&g_server->connection_pool, conn->pool_index, true);

    return (ssize_t)len;
}

// WebSocket send callback - wraps send_nonblocking for use by websocket.c
// Drops the flags parameter (WebSocket frames are complete messages, no MSG_MORE needed)
static ssize_t ws_send_callback(connection_t* conn, const void* data, size_t len) {
    return send_nonblocking(conn, data, len, 0);
}

// Filesystem send callback - wraps send_nonblocking for use by filesystem.c
static ssize_t fs_send_callback(connection_t* conn, const void* data, size_t len) {
    return send_nonblocking(conn, data, len, MSG_MORE);
}

// Filesystem file stream callback - starts non-blocking file streaming via send_buffer
// Ownership of file_fd transfers to send_buffer (it will close it when streaming completes)
static int fs_file_stream_callback(connection_t* conn, int file_fd, size_t file_size) {
    send_buffer_t* sb = get_send_buffer(conn);
    if (!sb) return -1;

    // Ensure send buffer is allocated
    if (!sb->allocated) {
        if (!send_buffer_alloc(sb)) {
            ESP_LOGE(TAG, "Failed to allocate send buffer for file streaming");
            return -1;
        }
    }

    // Start file streaming - send_buffer takes ownership of file_fd
    if (!send_buffer_start_file(sb, file_fd, file_size)) {
        return -1;
    }

    // Mark connection as write-pending to trigger on_write_ready
    connection_mark_write_pending(&g_server->connection_pool, conn->pool_index, true);
    return 0;
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

// Pre-built HTTP status lines for common status codes.
// Returns the status line string and sets *out_len to the string length.
// For uncommon codes, formats into the provided fallback buffer.
#define STATUS_LINE(code, text) "HTTP/1.1 " #code " " text "\r\n"
static const char status_line_200[] = STATUS_LINE(200, "OK");
static const char status_line_201[] = STATUS_LINE(201, "Created");
static const char status_line_204[] = STATUS_LINE(204, "No Content");
static const char status_line_301[] = STATUS_LINE(301, "Moved Permanently");
static const char status_line_302[] = STATUS_LINE(302, "Found");
static const char status_line_304[] = STATUS_LINE(304, "Not Modified");
static const char status_line_400[] = STATUS_LINE(400, "Bad Request");
static const char status_line_401[] = STATUS_LINE(401, "Unauthorized");
static const char status_line_403[] = STATUS_LINE(403, "Forbidden");
static const char status_line_404[] = STATUS_LINE(404, "Not Found");
static const char status_line_405[] = STATUS_LINE(405, "Method Not Allowed");
static const char status_line_500[] = STATUS_LINE(500, "Internal Server Error");
#undef STATUS_LINE

// Get pre-built status line for a status code. For common codes, returns a pointer
// to a static string (zero-cost). For uncommon codes, formats into fallback_buf.
// *out_len is always set to the length of the returned string.
static const char* get_status_line(int status, char* fallback_buf, size_t fallback_size, int* out_len) {
    switch (status) {
        case 200: *out_len = sizeof(status_line_200) - 1; return status_line_200;
        case 201: *out_len = sizeof(status_line_201) - 1; return status_line_201;
        case 204: *out_len = sizeof(status_line_204) - 1; return status_line_204;
        case 301: *out_len = sizeof(status_line_301) - 1; return status_line_301;
        case 302: *out_len = sizeof(status_line_302) - 1; return status_line_302;
        case 304: *out_len = sizeof(status_line_304) - 1; return status_line_304;
        case 400: *out_len = sizeof(status_line_400) - 1; return status_line_400;
        case 401: *out_len = sizeof(status_line_401) - 1; return status_line_401;
        case 403: *out_len = sizeof(status_line_403) - 1; return status_line_403;
        case 404: *out_len = sizeof(status_line_404) - 1; return status_line_404;
        case 405: *out_len = sizeof(status_line_405) - 1; return status_line_405;
        case 500: *out_len = sizeof(status_line_500) - 1; return status_line_500;
        default: {
            static const char prefix[] = "HTTP/1.1 ";
            char* p = fallback_buf;
            memcpy(p, prefix, sizeof(prefix) - 1);
            p += sizeof(prefix) - 1;
            p += format_uint(p, (size_t)status);
            *p++ = ' ';
            const char* text = httpd_status_text(status);
            size_t text_len = strlen(text);
            memcpy(p, text, text_len);
            p += text_len;
            *p++ = '\r'; *p++ = '\n';
            *out_len = (int)(p - fallback_buf);
            return fallback_buf;
        }
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
static const mime_entry_t* const mime_dispatch[26] = {
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

    // Single-pass: lowercase and compute length simultaneously (max 5 chars)
    char lower[6];
    size_t ext_len = 0;
    while (ext[ext_len] && ext_len < 6) {
        lower[ext_len] = ext[ext_len] | 0x20;
        ext_len++;
    }
    // Reject empty, too long (>5), or still has chars beyond our scan
    if (ext_len == 0 || ext_len > 5) return "application/octet-stream";

    // Bounds check for dispatch
    if (lower[0] < 'a' || lower[0] > 'z') return "application/octet-stream";

    // O(1) dispatch to correct group
    const mime_entry_t* group = mime_dispatch[lower[0] - 'a'];
    if (!group) return "application/octet-stream";

    // Search within small group (max 4 entries) using memcmp on pre-lowered extension
    for (const mime_entry_t* e = group; e->ext; e++) {
        if (e->ext_len == ext_len && memcmp(lower, e->ext, ext_len) == 0) {
            return e->mime;
        }
    }

    return "application/octet-stream";
}

// Compute hex digit value from ASCII char (saves 256 bytes vs lookup table)
// Returns 0-15 for valid hex digits, -1 for invalid
static inline int8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

// Length-bounded URL decode for non-null-terminated substrings (e.g., query parameter values)
static int httpd_url_decode_n(const char* src, size_t src_len, char* dst, size_t dst_size) {
    size_t dst_idx = 0;

    for (size_t i = 0; i < src_len && dst_idx < dst_size - 1; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            int8_t hi = hex_val(src[i+1]);
            int8_t lo = hex_val(src[i+2]);
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

int httpd_url_decode(const char* src, char* dst, size_t dst_size) {
    return httpd_url_decode_n(src, strlen(src), dst, dst_size);
}

// Inline case-insensitive comparison for HTTP header names.
// Uses the |0x20 trick which only works correctly for ASCII letters (A-Z/a-z).
// Do NOT use for arbitrary string comparison — non-alpha chars may collide.
static inline bool header_casecmp(const char* a, const char* b, size_t len) {
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
static inline __attribute__((always_inline)) request_context_t* get_req_context(httpd_req_t* req) {
    if (!req || !req->_internal || !g_server) return NULL;
    connection_t* conn = (connection_t*)req->_internal;
    return request_contexts[conn->pool_index];
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

    size_t key_len = strlen(key);

    // Get request context and parse query if needed
    request_context_t* ctx = get_req_context(req);
    if (ctx) {
        parse_query_params(ctx);

        // O(k) lookup in cached parameters
        for (uint8_t i = 0; i < ctx->query_param_count; i++) {
            query_param_entry_t* entry = &ctx->query_params[i];
            if (entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0) {
                // Found - URL decode value into output buffer (bounded by value_len)
                return httpd_url_decode_n(entry->value, entry->value_len, value, value_size);
            }
        }
        return -1;  // Not found in cache
    }

    // Fallback to original linear scan if context unavailable
    const char* p = req->query;
    const char* end = req->query + req->query_len;

    while (p < end) {
        const char* eq = memchr(p, '=', end - p);
        if (!eq) break;

        size_t k_len = eq - p;
        const char* amp = memchr(eq + 1, '&', end - (eq + 1));
        if (k_len == key_len && memcmp(p, key, key_len) == 0) {
            const char* v_start = eq + 1;
            size_t v_len = amp ? (size_t)(amp - v_start) : (size_t)(end - v_start);
            return httpd_url_decode_n(v_start, v_len, value, value_size);
        }
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
// Returns NULL if context not allocated (connection not fully established)
static inline request_context_t* get_request_context(connection_t* conn) {
    if (!g_server) return NULL;
    return request_contexts[conn->pool_index];
}

static inline __attribute__((always_inline)) ws_context_t* get_ws_context(connection_t* conn) {
    if (!g_server) return NULL;
    return ws_contexts[conn->pool_index];
}

static void init_request_context(request_context_t* ctx, connection_t* conn) {
    // On the first request of a connection, _internal is NULL (from calloc
    // or on_disconnect memset), so all buffer pointers are guaranteed NULL.
    // Skip the free() calls to avoid function call overhead.
    if (ctx->req._internal) {
        if (ctx->uri_buf_is_heap) free(ctx->uri_buf);
        if (ctx->header_buf) free(ctx->header_buf);
        if (ctx->body_buf) free(ctx->body_buf);
        if (ctx->recv_buf_is_heap) free(ctx->recv_buf);
    }

    // Zero only the fields that need zeroing (up to _zero_end marker).
    // Skips ~768 bytes of scratch buffers (inline_recv_buf, inline_uri_buf,
    // headers array, query_params, mw_chain) that are written before read.
    memset(ctx, 0, offsetof(request_context_t, _zero_end));

    // Set the few non-zero fields
    ctx->req.fd = conn->fd;
    ctx->req._internal = conn;
    ctx->req.status_code = 200;
    ctx->server = g_server;
}

// Store header in request context (called from http_parser via extern)
void esphttpd_store_header(connection_t* conn,
                           const uint8_t* key, uint8_t key_len,
                           const uint8_t* value, uint8_t value_len) {
    if (!conn || !g_server) return;

    request_context_t* ctx = get_request_context(conn);
    if (!ctx) return;

    store_header_in_req(ctx, key, key_len, value, value_len);
}

// New header storage that works with request context
// Dynamically allocates and grows header buffer as needed
static void store_header_in_req(request_context_t* ctx, const uint8_t* key, uint8_t key_len,
                                const uint8_t* value, uint8_t value_len) {
    if (ctx->req.header_count >= MAX_REQ_HEADERS) return;

    size_t needed = key_len + 1 + value_len + 1;
    size_t required = ctx->req.header_buf_used + needed;

    // Allocate or grow header buffer if needed
    if (required > ctx->req.header_buf_size) {
        // Calculate new capacity - start with 256, grow to fit with margin
        size_t new_capacity = ctx->req.header_buf_size ? ctx->req.header_buf_size : 256;
        while (new_capacity < required) {
            new_capacity = (new_capacity < 1024) ? new_capacity * 2 : new_capacity + 512;
        }
        // Cap at max size
        if (new_capacity > REQ_HEADER_BUF_SIZE) {
            new_capacity = REQ_HEADER_BUF_SIZE;
        }
        if (required > new_capacity) {
            ESP_LOGW(TAG, "Header buffer full, dropping header");
            return;
        }

        char* new_buf = (char*)realloc(ctx->header_buf, new_capacity);
        if (!new_buf) {
            ESP_LOGE(TAG, "Failed to allocate header buffer");
            return;
        }
        ctx->header_buf = new_buf;
        ctx->req.header_buf = new_buf;
        ctx->req.header_buf_size = new_capacity;
    }

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

// Forward declaration for built-in CORS middleware
static httpd_err_t cors_middleware(httpd_req_t* req, httpd_next_t next);

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
#ifndef CONFIG_HTTPD_USE_RAW_API
        .select_timeout_ms = 1000,
        .io_buffer_size = cfg.recv_buffer_size,
#endif
        .nodelay = true,
        .reuseaddr = true
    };

    // Initialize components (event_loop_init also initializes the connection pool)
    event_loop_init(&server->event_loop, &server->connection_pool, &el_config);

    // Pre-allocate per-connection context arrays (one contiguous block each)
    // This eliminates malloc/free on every connect/disconnect and prevents heap fragmentation
    preallocated_request_contexts = (request_context_t*)calloc(MAX_CONNECTIONS, sizeof(request_context_t));
    preallocated_ws_contexts = (ws_context_t*)calloc(MAX_CONNECTIONS, sizeof(ws_context_t));
    preallocated_send_buffers = (send_buffer_t*)calloc(MAX_CONNECTIONS, sizeof(send_buffer_t));

    if (!preallocated_request_contexts || !preallocated_ws_contexts || !preallocated_send_buffers) {
        ESP_LOGE(TAG, "Failed to pre-allocate per-connection contexts");
        free(preallocated_request_contexts);
        free(preallocated_ws_contexts);
        free(preallocated_send_buffers);
        preallocated_request_contexts = NULL;
        preallocated_ws_contexts = NULL;
        preallocated_send_buffers = NULL;
        return HTTPD_ERR_NO_MEM;
    }

    // Point pointer arrays at pre-allocated backing storage and initialize send buffers
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        request_contexts[i] = &preallocated_request_contexts[i];
        ws_contexts[i] = &preallocated_ws_contexts[i];
        connection_send_buffers[i] = &preallocated_send_buffers[i];
        send_buffer_init(connection_send_buffers[i]);
    }

    // Initialize channel hash table
    init_channel_hash(server);

    // Initialize radix tree for legacy routes (O(log n) lookup)
    server->legacy_routes = radix_tree_create();
    if (!server->legacy_routes) {
        ESP_LOGE(TAG, "Failed to create legacy routes radix tree");
        free(preallocated_request_contexts);
        free(preallocated_ws_contexts);
        free(preallocated_send_buffers);
        preallocated_request_contexts = NULL;
        preallocated_ws_contexts = NULL;
        preallocated_send_buffers = NULL;
        memset(request_contexts, 0, sizeof(request_contexts));
        memset(ws_contexts, 0, sizeof(ws_contexts));
        memset(connection_send_buffers, 0, sizeof(connection_send_buffers));
        return HTTPD_ERR_NO_MEM;
    }

    // Register WebSocket send callback so frames route through send_nonblocking
    ws_set_send_func(ws_send_callback);

    // Register filesystem send callbacks so file serving routes through send_nonblocking
    fs_set_send_func(fs_send_callback);
    fs_set_file_stream_func(fs_file_stream_callback);

    // Setup event handlers
    server->handlers.on_http_request = on_http_request;
    server->handlers.on_http_body = on_http_body;
    server->handlers.on_ws_frame = on_ws_frame;
    server->handlers.on_connect = on_connect;
    server->handlers.on_ws_connect = on_ws_connect;
    server->handlers.on_ws_disconnect = on_ws_disconnect;
    server->handlers.on_disconnect = on_disconnect;
    server->handlers.on_write_ready = on_write_ready;

    server->initialized = true;
    g_server = server;
#ifdef CONFIG_ESPHTTPD_TEST_MODE
    g_test_request_contexts = request_contexts;
    g_test_send_buffers = connection_send_buffers;
#endif
    *handle = server;

    // Add built-in CORS middleware if enabled
    if (cfg.enable_cors) {
        httpd_use(server, cors_middleware);
        ESP_LOGI(TAG, "CORS enabled (origin: %s)", cfg.cors_origin ? cfg.cors_origin : "*");
    }

    ESP_LOGI(TAG, "Server initialized on port %d", cfg.port);

#ifdef CONFIG_ESPHTTPD_TEST_MODE
    server->event_loop.running = true;
    server->running = true;
    ESP_LOGI(TAG, "Server started in TEST MODE (no task created)");
#elif defined(CONFIG_HTTPD_USE_RAW_API)
    // Raw API mode: start listening via lwIP raw callbacks (no server task needed)
    HTTPD_LOCK_TCPIP();
    int raw_ret = raw_tcp_listen(&server->event_loop, &server->handlers);
    HTTPD_UNLOCK_TCPIP();
    if (raw_ret < 0) {
        server->initialized = false;
        g_server = NULL;
        free(preallocated_request_contexts);
        free(preallocated_ws_contexts);
        free(preallocated_send_buffers);
        preallocated_request_contexts = NULL;
        preallocated_ws_contexts = NULL;
        preallocated_send_buffers = NULL;
        memset(request_contexts, 0, sizeof(request_contexts));
        memset(ws_contexts, 0, sizeof(ws_contexts));
        memset(connection_send_buffers, 0, sizeof(connection_send_buffers));
        return HTTPD_ERR_IO;
    }
    server->running = true;
    ESP_LOGI(TAG, "Server started in raw TCP API mode (no server task)");
#else
    // Start the server task
    BaseType_t ret = xTaskCreate(server_task, "httpd",
                                  cfg.stack_size, server,
                                  cfg.task_priority, NULL);
    if (ret != pdPASS) {
        server->initialized = false;
        g_server = NULL;
        free(preallocated_request_contexts);
        free(preallocated_ws_contexts);
        free(preallocated_send_buffers);
        preallocated_request_contexts = NULL;
        preallocated_ws_contexts = NULL;
        preallocated_send_buffers = NULL;
        memset(request_contexts, 0, sizeof(request_contexts));
        memset(ws_contexts, 0, sizeof(ws_contexts));
        memset(connection_send_buffers, 0, sizeof(connection_send_buffers));
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

#ifdef CONFIG_HTTPD_USE_RAW_API
    // Stop file I/O worker if running
    file_io_worker_stop();

#ifndef CONFIG_ESPHTTPD_TEST_MODE
    // Raw API: close all connections and listen PCB under tcpip lock
    HTTPD_LOCK_TCPIP();
    raw_tcp_stop(&server->event_loop);
    HTTPD_UNLOCK_TCPIP();
#endif
#else
    event_loop_stop(&server->event_loop);

    // Close all active connections
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_is_active(&server->connection_pool, i)) {
            connection_t* conn = &server->connection_pool.connections[i];
            close(conn->fd);
        }
    }
#endif

    // Free per-connection dynamic sub-buffers
    for (int i = 0; i < MAX_CONNECTIONS; i++) {

        // Free dynamically-allocated sub-buffers within pre-allocated contexts
        if (request_contexts[i]) {
            free(request_contexts[i]->uri_buf);
            free(request_contexts[i]->header_buf);
            free(request_contexts[i]->body_buf);
            free(request_contexts[i]->recv_buf);
            request_contexts[i] = NULL;
        }

        if (ws_contexts[i]) {
            free(ws_contexts[i]->frame_ctx.payload_buffer);
            ws_contexts[i] = NULL;
        }

        if (connection_send_buffers[i]) {
            if (connection_send_buffers[i]->allocated) {
                send_buffer_free(connection_send_buffers[i]);
            }
            connection_send_buffers[i] = NULL;
        }
    }

    // Free pre-allocated backing arrays
    free(preallocated_request_contexts);
    preallocated_request_contexts = NULL;
    free(preallocated_ws_contexts);
    preallocated_ws_contexts = NULL;
    free(preallocated_send_buffers);
    preallocated_send_buffers = NULL;

    server->initialized = false;
    server->running = false;

    // Destroy legacy routes radix tree
    if (server->legacy_routes) {
        radix_tree_destroy(server->legacy_routes);
        server->legacy_routes = NULL;
    }

    // Destroy mounted routers
    for (int i = 0; i < server->mounted_router_count; i++) {
        if (server->mounted_routers[i].router) {
            httpd_router_destroy(server->mounted_routers[i].router);
            server->mounted_routers[i].router = NULL;
        }
    }
    server->mounted_router_count = 0;

    // Reset route counts
    server->ws_route_count = 0;
    server->middleware_count = 0;

    // Reset channel hash table
    init_channel_hash(server);

    // Clear WebSocket send callback
    ws_set_send_func(NULL);

    // Clear filesystem send callbacks
    fs_set_send_func(NULL);
    fs_set_file_stream_func(NULL);

    if (g_server == server) {
        g_server = NULL;
#ifdef CONFIG_ESPHTTPD_TEST_MODE
        g_test_request_contexts = NULL;
        g_test_send_buffers = NULL;
#endif
    }

    return HTTPD_OK;
}

bool httpd_is_running(httpd_handle_t handle) {
    struct httpd_server* server = handle;
    return server && server->running;
}

#ifndef CONFIG_HTTPD_USE_RAW_API
static void server_task(void* pvParameters) {
    struct httpd_server* server = (struct httpd_server*)pvParameters;
    event_loop_run(&server->event_loop, &server->handlers);
    server->running = false;
    vTaskDelete(NULL);
}
#endif

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

    ESP_LOGD(TAG, "Registered route: %s %s",
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
    entry->pattern = route->pattern;
    entry->handler = route->handler;
    entry->user_ctx = route->user_ctx;
    entry->ping_interval_ms = route->ping_interval_ms;

    ESP_LOGD(TAG, "Registered WebSocket route: %s", route->pattern);

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

    // Store the mounted router
    mounted_router_t* entry = &server->mounted_routers[server->mounted_router_count];
    entry->prefix = prefix;
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

// Send a complete CORS preflight response as a single write.
// Combines status line + all CORS headers + Content-Length: 0 + blank line into one buffer.
static httpd_err_t send_cors_preflight(httpd_req_t* req, connection_t* conn, const char* cors_origin) {
    // Pre-built constant parts of CORS preflight response
    static const char cors_headers[] =
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    // Build complete response in one buffer: status + origin header + static headers
    char resp_buf[384];
    int pos = sizeof(status_line_204) - 1;
    memcpy(resp_buf, status_line_204, pos);

    // Add Access-Control-Allow-Origin header with config-dependent origin
    static const char acao_prefix[] = "Access-Control-Allow-Origin: ";
    memcpy(resp_buf + pos, acao_prefix, sizeof(acao_prefix) - 1);
    pos += sizeof(acao_prefix) - 1;
    size_t origin_len = strlen(cors_origin);
    memcpy(resp_buf + pos, cors_origin, origin_len);
    pos += origin_len;
    resp_buf[pos++] = '\r';
    resp_buf[pos++] = '\n';

    // Append the static CORS headers
    memcpy(resp_buf + pos, cors_headers, sizeof(cors_headers) - 1);
    pos += sizeof(cors_headers) - 1;

    // Single send for the entire response
    if (send_nonblocking(conn, resp_buf, pos, 0) < 0) {
        return HTTPD_ERR_IO;
    }
    req->headers_sent = true;
    req->body_started = true;
    return HTTPD_OK;
}

// Built-in CORS middleware
static httpd_err_t cors_middleware(httpd_req_t* req, httpd_next_t next) {
    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    // Get CORS origin from server config
    const char* cors_origin = g_server->config.cors_origin ? g_server->config.cors_origin : "*";

    // Handle OPTIONS preflight requests - send complete response in single write
    if (req->method == HTTP_OPTIONS) {
        return send_cors_preflight(req, conn, cors_origin);
    }

    // For all other requests, add CORS headers and continue
    httpd_resp_set_header(req, "Access-Control-Allow-Origin", cors_origin);

    return next(req);
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
        // Filter by length and first char before expensive header_casecmp
        if (entry->key_len == key_len &&
            (ctx->header_buf[entry->key_offset] | 0x20) == first_lower &&
            header_casecmp(&ctx->header_buf[entry->key_offset], key, key_len)) {
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
    if (ctx->body_buf && ctx->body_buf_pos < ctx->body_buf_len) {
        size_t pre_available = ctx->body_buf_len - ctx->body_buf_pos;
        size_t pre_to_copy = (len < pre_available) ? len : pre_available;
        if (pre_to_copy > remaining) pre_to_copy = remaining;

        memcpy(buf, &ctx->body_buf[ctx->body_buf_pos], pre_to_copy);
        ctx->body_buf_pos += pre_to_copy;
        req->body_received += pre_to_copy;
        total_received = pre_to_copy;

        // Free body buffer early when fully consumed
        if (ctx->body_buf_pos >= ctx->body_buf_len) {
            free(ctx->body_buf);
            ctx->body_buf = NULL;
            ctx->body_buf_len = 0;
            ctx->body_buf_pos = 0;
        }

        // If we've filled the buffer or received all content, return
        if (total_received >= len || req->body_received >= req->content_length) {
            return (int)total_received;
        }
    }

    // If we need more data:
    // - Raw API: only pre-buffered body data is available (no socket to recv from).
    //   Callers must use deferred or continuation handling for large bodies.
    // - Socket API: do a single non-blocking recv from socket.
    remaining = req->content_length - req->body_received;
#ifdef CONFIG_HTTPD_USE_RAW_API
    // Under raw API, all body data arrives via recv_cb and is placed in body_buf.
    // If body_buf is exhausted, caller must use httpd_req_defer() or continuation.
    (void)remaining;  // No additional recv possible
#else
    if (remaining > 0 && total_received < len) {
        size_t to_recv = (len - total_received) < remaining ? (len - total_received) : remaining;

        ssize_t received = recv(conn->fd, (char*)buf + total_received, to_recv, MSG_DONTWAIT);

        if (received > 0) {
            req->body_received += received;
            total_received += received;
        } else if (received == 0) {
            // Connection closed
            if (total_received == 0) return -1;
        } else {
            // received < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available right now - return what we have so far
                // (which may be 0 if no pre-buffered data either)
            } else {
                // Real error
                ESP_LOGE(TAG, "recv error: %s", strerror(errno));
                if (total_received == 0) return -1;
            }
        }
    }
#endif

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
        char fallback_buf[64];
        int len;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &len);
        if (send_nonblocking(conn, sl, len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    char header[256];
    size_t key_len = strlen(key);

    // Track if user is manually setting Content-Length
    // Short-circuit: check length + first char before expensive strcasecmp
    if (key_len == 14 && (key[0] | 0x20) == 'c' && strcasecmp(key, "Content-Length") == 0) {
        req->content_length_set = true;
    }
    size_t val_len = strlen(value);
    size_t total = key_len + 2 + val_len + 2; // "key: value\r\n"
    if (total > sizeof(header)) return HTTPD_ERR_INVALID_ARG;
    memcpy(header, key, key_len);
    header[key_len] = ':';
    header[key_len + 1] = ' ';
    memcpy(header + key_len + 2, value, val_len);
    header[key_len + 2 + val_len] = '\r';
    header[key_len + 2 + val_len + 1] = '\n';
    if (send_nonblocking(conn, header, total, MSG_MORE) < 0) {
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

    // Fast path: coalesce small responses into a single send
    // Only when no headers have been sent yet and Content-Length wasn't manually set
    if (!req->headers_sent && !req->content_length_set && body_len <= 256) {
        char resp_buf[512];
        size_t pos = 0;

        // Status line
        char fallback_buf[64];
        int slen;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &slen);
        memcpy(resp_buf, sl, slen);
        pos += slen;

        // Content-Length header via memcpy (avoid snprintf)
        static const char cl_prefix[] = "Content-Length: ";
        memcpy(resp_buf + pos, cl_prefix, sizeof(cl_prefix) - 1);
        pos += sizeof(cl_prefix) - 1;
        // Convert body_len to digits
        char digits[16];
        int ndigits = 0;
        {
            size_t val = body_len;
            do {
                digits[ndigits++] = '0' + (val % 10);
                val /= 10;
            } while (val > 0);
            // Reverse digits into resp_buf
            for (int i = ndigits - 1; i >= 0; i--) {
                resp_buf[pos++] = digits[i];
            }
        }
        resp_buf[pos++] = '\r';
        resp_buf[pos++] = '\n';

        // End headers
        resp_buf[pos++] = '\r';
        resp_buf[pos++] = '\n';

        // Body
        if (body && body_len > 0) {
            memcpy(resp_buf + pos, body, body_len);
            pos += body_len;
        }

        if (send_nonblocking(conn, resp_buf, pos, 0) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
        req->body_started = true;
        return HTTPD_OK;
    }

    // Standard path: multiple sends for large bodies or when headers were already partially sent

    // Send status line if not sent
    if (!req->headers_sent) {
        char fallback_buf[64];
        int slen;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &slen);
        if (send_nonblocking(conn, sl, slen, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    // Add Content-Length unless the user already set it manually via httpd_resp_set_header
    if (!req->content_length_set) {
        char cl_header[64];
        static const char cl_pfx[] = "Content-Length: ";
        memcpy(cl_header, cl_pfx, sizeof(cl_pfx) - 1);
        int cl_len = sizeof(cl_pfx) - 1;
        cl_len += format_uint(cl_header + cl_len, body_len);
        cl_header[cl_len++] = '\r';
        cl_header[cl_len++] = '\n';
        if (send_nonblocking(conn, cl_header, cl_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    // End headers
    if (send_nonblocking(conn, "\r\n", 2, MSG_MORE) < 0) {
        return HTTPD_ERR_IO;
    }
    req->body_started = true;

    // Send body via non-blocking send. Data is sent directly if the socket accepts it,
    // otherwise queued to the send buffer for draining by on_write_ready in the event loop.
    if (body && body_len > 0) {
        if (send_nonblocking(conn, body, body_len, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* chunk, ssize_t len) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    send_buffer_t* sb = get_send_buffer(conn);
    if (!sb) return HTTPD_ERR_NO_MEM;

    // Finalize headers if body hasn't started yet
    if (!req->body_started) {
        // Build complete header block: status line + Transfer-Encoding + blank line
        static const char te_chunked[] = "Transfer-Encoding: chunked\r\n\r\n";
        char header_block[128];
        int header_len;
        if (!req->headers_sent) {
            char fallback_buf[64];
            int sl_len;
            const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &sl_len);
            memcpy(header_block, sl, sl_len);
            memcpy(header_block + sl_len, te_chunked, sizeof(te_chunked) - 1);
            header_len = sl_len + (int)(sizeof(te_chunked) - 1);
            req->headers_sent = true;
        } else {
            header_len = sizeof(te_chunked) - 1;
            memcpy(header_block, te_chunked, header_len);
        }

        // Queue headers atomically (small enough to always fit)
        if (send_nonblocking(conn, header_block, header_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->body_started = true;
        sb->chunked = 1;
    }

    size_t chunk_len = (len >= 0) ? (size_t)len : (chunk ? strlen(chunk) : 0);

    if (chunk_len == 0) {
        // Final chunk "0\r\n\r\n" - always small, queue atomically
        if (send_nonblocking(conn, "0\r\n\r\n", 5, 0) < 0) {
            return HTTPD_ERR_IO;
        }
    } else {
        // Build complete chunk frame atomically: "size\r\ndata\r\n"
        // For small chunks, use stack buffer. For large chunks, queue in parts
        // but ensure each part is complete before moving to next.

        // Build chunk header: "XXXX\r\n" (max 10 bytes for 32-bit size)
        char size_header[16];
        int header_len = format_hex(size_header, chunk_len);

        // Calculate total frame size
        size_t frame_size = header_len + chunk_len + 2;  // +2 for trailing \r\n

        // For small chunks (<=256 bytes total), build complete frame in stack buffer
        // This ensures atomic queuing for typical use cases
        if (frame_size <= 256) {
            uint8_t frame_buf[256];
            memcpy(frame_buf, size_header, header_len);
            memcpy(frame_buf + header_len, chunk, chunk_len);
            memcpy(frame_buf + header_len + chunk_len, "\r\n", 2);

            // Queue entire frame atomically
            if (send_nonblocking(conn, frame_buf, frame_size, 0) < 0) {
                return HTTPD_ERR_IO;
            }
        } else {
            // Large chunk: ensure send buffer can accommodate at minimum
            // the chunk header before proceeding

            // Ensure buffer is allocated
            if (!sb->allocated && !send_buffer_alloc(sb)) {
                return HTTPD_ERR_NO_MEM;
            }

            // First drain any pending data to maximize available space
            drain_send_buffer(conn);

            // Check if we have enough contiguous space for header + data + trailer
            size_t space = send_buffer_space(sb);
            if (space < frame_size) {
                // Not enough space - try to send header+data in parts
                // but each syscall must be complete before proceeding

                // Send header first
                if (send_nonblocking(conn, size_header, header_len, MSG_MORE) < 0) {
                    return HTTPD_ERR_IO;
                }

                // Send data
                if (send_nonblocking(conn, chunk, chunk_len, MSG_MORE) < 0) {
                    return HTTPD_ERR_IO;
                }

                // Send terminator
                if (send_nonblocking(conn, "\r\n", 2, 0) < 0) {
                    return HTTPD_ERR_IO;
                }
            } else {
                // Enough space - queue atomically using zero-copy write
                uint8_t* write_ptr;
                size_t avail = send_buffer_write_ptr(sb, &write_ptr);

                if (avail >= frame_size) {
                    // Build frame directly in buffer
                    memcpy(write_ptr, size_header, header_len);
                    memcpy(write_ptr + header_len, chunk, chunk_len);
                    memcpy(write_ptr + header_len + chunk_len, "\r\n", 2);
                    send_buffer_commit(sb, frame_size);

                    // Mark for write
                    connection_mark_write_pending(&g_server->connection_pool,
                                                  conn->pool_index, true);
                } else {
                    // Contiguous space not enough (wrap-around) - use queue
                    if (send_buffer_queue(sb, size_header, header_len) < 0 ||
                        send_buffer_queue(sb, chunk, chunk_len) < 0 ||
                        send_buffer_queue(sb, "\r\n", 2) < 0) {
                        return HTTPD_ERR_IO;
                    }
                    connection_mark_write_pending(&g_server->connection_pool,
                                                  conn->pool_index, true);
                }
            }
        }
    }

    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_error(httpd_req_t* req, int status, const char* message) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    req->status_code = status;
    const char* msg = message ? message : httpd_status_text(status);
    size_t msg_len = strlen(msg);

    // Build entire error response in a single buffer to avoid multiple send calls.
    // Format: status_line + Content-Type + Content-Length + blank line + body
    char resp[384];
    size_t pos = 0;

    // Status line
    char fallback_buf[64];
    int slen;
    const char* sl = get_status_line(status, fallback_buf, sizeof(fallback_buf), &slen);
    memcpy(resp + pos, sl, slen);
    pos += slen;

    // Content-Type: text/plain\r\n
    static const char ct_plain[] = "Content-Type: text/plain\r\n";
    memcpy(resp + pos, ct_plain, sizeof(ct_plain) - 1);
    pos += sizeof(ct_plain) - 1;

    // Content-Length: <digits>\r\n
    static const char cl_prefix[] = "Content-Length: ";
    memcpy(resp + pos, cl_prefix, sizeof(cl_prefix) - 1);
    pos += sizeof(cl_prefix) - 1;
    char digits[16];
    int ndigits = 0;
    {
        size_t val = msg_len;
        do {
            digits[ndigits++] = '0' + (val % 10);
            val /= 10;
        } while (val > 0);
        for (int i = ndigits - 1; i >= 0; i--) {
            resp[pos++] = digits[i];
        }
    }
    resp[pos++] = '\r';
    resp[pos++] = '\n';

    // End headers
    resp[pos++] = '\r';
    resp[pos++] = '\n';

    // Body
    memcpy(resp + pos, msg, msg_len);
    pos += msg_len;

    if (send_nonblocking(conn, resp, pos, 0) < 0) {
        return HTTPD_ERR_IO;
    }
    req->headers_sent = true;
    req->body_started = true;
    return HTTPD_OK;
}

httpd_err_t httpd_resp_sendfile(httpd_req_t* req, const char* path) {
    if (!req || !path) return HTTPD_ERR_INVALID_ARG;

    // Validate path for security (no directory traversal)
    if (strstr(path, "..") != NULL) {
        return httpd_resp_send_error(req, 403, "Forbidden");
    }

    // Get file info
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return httpd_resp_send_error(req, 404, "File not found");
    }

    // Open file
    FILE* f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_error(req, 404, "File not found");
    }

    // Set content type based on extension
    const char* mime_type = filesystem_get_mime_type(path);
    httpd_resp_set_type(req, mime_type);

    // Set Content-Length from stat result (avoids chunked transfer encoding overhead)
    char cl_str[24];
    int cl_str_len = format_uint(cl_str, (size_t)st.st_size);
    cl_str[cl_str_len] = '\0';
    httpd_resp_set_header(req, "Content-Length", cl_str);

    // Finalize headers and stream file body with Content-Length encoding
    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        fclose(f);
        return HTTPD_ERR_CONN_CLOSED;
    }

    // End headers (status line + Content-Type + Content-Length already sent by set_header calls)
    if (send_nonblocking(conn, "\r\n", 2, st.st_size > 0 ? MSG_MORE : 0) < 0) {
        fclose(f);
        return HTTPD_ERR_IO;
    }
    req->body_started = true;

    // Stream file data directly (no chunked framing)
    char buf[512];
    size_t bytes_read;
    while ((bytes_read = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_nonblocking(conn, buf, bytes_read, 0) < 0) {
            fclose(f);
            return HTTPD_ERR_IO;
        }
    }

    fclose(f);
    return HTTPD_OK;
}

httpd_err_t httpd_resp_send_json(httpd_req_t* req, const char* json) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

    size_t json_len = json ? strlen(json) : 0;

    // Fast path: coalesce status + Content-Type + Content-Length + body into one send.
    // httpd_resp_set_type() would call httpd_resp_set_header() which sends the status
    // line and sets headers_sent=true, defeating the coalescing in httpd_resp_send().
    // By building everything inline, we avoid that and send in a single syscall.
    if (!req->headers_sent && !req->content_length_set && json_len <= 256) {
        // 512 buf covers: status line (~24) + Content-Type (~38) +
        // Content-Length (~24) + CRLF (2) + body (<=256) = ~344 max
        char resp_buf[512];
        size_t pos = 0;

        // Status line
        char fallback_buf[64];
        int slen;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &slen);
        memcpy(resp_buf, sl, slen);
        pos += slen;

        // Content-Type header (fixed string, avoid snprintf)
        static const char ct_header[] = "Content-Type: application/json\r\n";
        memcpy(resp_buf + pos, ct_header, sizeof(ct_header) - 1);
        pos += sizeof(ct_header) - 1;

        // Content-Length header via memcpy (avoid snprintf)
        static const char cl_prefix[] = "Content-Length: ";
        memcpy(resp_buf + pos, cl_prefix, sizeof(cl_prefix) - 1);
        pos += sizeof(cl_prefix) - 1;
        char digits[16];
        int ndigits = 0;
        {
            size_t val = json_len;
            do {
                digits[ndigits++] = '0' + (val % 10);
                val /= 10;
            } while (val > 0);
            for (int i = ndigits - 1; i >= 0; i--) {
                resp_buf[pos++] = digits[i];
            }
        }
        resp_buf[pos++] = '\r';
        resp_buf[pos++] = '\n';

        // End headers
        resp_buf[pos++] = '\r';
        resp_buf[pos++] = '\n';

        // Body
        if (json && json_len > 0) {
            memcpy(resp_buf + pos, json, json_len);
            pos += json_len;
        }

        if (send_nonblocking(conn, resp_buf, pos, 0) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
        req->body_started = true;
        return HTTPD_OK;
    }

    // Fallback: use standard header + send path for large bodies or
    // when headers were already partially sent
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (ssize_t)json_len);
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
        char header_buf[128];
        char fallback_buf[64];
        int sl_len;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &sl_len);
        memcpy(header_buf, sl, sl_len);
        int header_len;
        if (req->content_length_set) {
            memcpy(header_buf + sl_len, "\r\n", 2);
            header_len = sl_len + 2;
        } else {
            static const char cl_pfx[] = "Content-Length: ";
            char* p = header_buf + sl_len;
            memcpy(p, cl_pfx, sizeof(cl_pfx) - 1);
            p += sizeof(cl_pfx) - 1;
            p += format_uint(p, body_len);
            *p++ = '\r'; *p++ = '\n';
            *p++ = '\r'; *p++ = '\n';
            header_len = (int)(p - header_buf);
        }

        // Single syscall for all headers
        if (send_nonblocking(conn, header_buf, header_len, body_len > 0 ? MSG_MORE : 0) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    } else {
        // Headers already partially sent, send Content-Length (if not already set) and terminator
        if (req->content_length_set) {
            if (send_nonblocking(conn, "\r\n", 2, body_len > 0 ? MSG_MORE : 0) < 0) {
                return HTTPD_ERR_IO;
            }
        } else {
            char cl_buf[80];
            static const char cl_pfx[] = "Content-Length: ";
            memcpy(cl_buf, cl_pfx, sizeof(cl_pfx) - 1);
            int cl_len = sizeof(cl_pfx) - 1;
            cl_len += format_uint(cl_buf + cl_len, body_len);
            cl_buf[cl_len++] = '\r'; cl_buf[cl_len++] = '\n';
            cl_buf[cl_len++] = '\r'; cl_buf[cl_len++] = '\n';
            if (send_nonblocking(conn, cl_buf, cl_len, body_len > 0 ? MSG_MORE : 0) < 0) {
                return HTTPD_ERR_IO;
            }
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

    // filesystem_serve_file queues headers and starts file streaming via send_buffer.
    // The actual file data is sent asynchronously by the event loop's on_write_ready handler.
    int result = filesystem_serve_file(g_server->filesystem, conn, path, false);

    if (result < 0) {
        if (on_done) on_done(req, HTTPD_ERR_NOT_FOUND);
        return HTTPD_ERR_NOT_FOUND;
    }

    // Set up async completion tracking - on_done fires when send buffer is drained
    if (on_done) {
        request_context_t* ctx = get_request_context(conn);
        if (ctx) {
            ctx->async_send.on_done = on_done;
            ctx->async_send.active = true;
        }
    }

    return HTTPD_OK;
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
        char fallback_buf[64];
        int slen;
        const char* sl = get_status_line(req->status_code, fallback_buf, sizeof(fallback_buf), &slen);
        if (send_nonblocking(conn, sl, slen, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
        req->headers_sent = true;
    }

    // Send Content-Length or Transfer-Encoding header (skip if user already set Content-Length)
    char header_buf[64];
    int header_len;
    if (use_chunked) {
        static const char te_chunked[] = "Transfer-Encoding: chunked\r\n";
        memcpy(header_buf, te_chunked, sizeof(te_chunked) - 1);
        header_len = sizeof(te_chunked) - 1;
    } else if (req->content_length_set) {
        header_len = 0;
    } else {
        static const char cl_pfx[] = "Content-Length: ";
        memcpy(header_buf, cl_pfx, sizeof(cl_pfx) - 1);
        header_len = sizeof(cl_pfx) - 1;
        header_len += format_uint(header_buf + header_len, (size_t)content_length);
        header_buf[header_len++] = '\r';
        header_buf[header_len++] = '\n';
    }
    if (header_len > 0) {
        if (send_nonblocking(conn, header_buf, header_len, MSG_MORE) < 0) {
            return HTTPD_ERR_IO;
        }
    }

    // End headers
    if (send_nonblocking(conn, "\r\n", 2, 0) < 0) {
        return HTTPD_ERR_IO;
    }

    // Allocate send buffer for the data provider
    send_buffer_t* sb = get_send_buffer(conn);
    if (!send_buffer_alloc(sb)) {
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
    if (send_nonblocking(conn, response, sizeof(response) - 1, 0) < 0) {
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

// ============================================================================
// Deferred (Async) Request Handling
// ============================================================================

// Internal body callback for defer_to_file
static httpd_err_t defer_file_body_cb(httpd_req_t* req, const void* data, size_t len) {
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    if (!ctx->defer.file_fp) {
        return HTTPD_ERR_IO;
    }

    size_t written = fwrite(data, 1, len, ctx->defer.file_fp);
    if (written != len) {
        ESP_LOGE(TAG, "Failed to write to file: wrote %zu of %zu bytes", written, len);
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

// Internal done callback for defer_to_file
static void defer_file_done_cb(httpd_req_t* req, httpd_err_t err) {
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    if (ctx->defer.file_fp) {
        fclose(ctx->defer.file_fp);
        ctx->defer.file_fp = NULL;
    }
    httpd_done_cb_t user_cb = ctx->defer.file_user_done_cb;
    ctx->defer.file_user_done_cb = NULL;

    // Call user's done callback
    if (user_cb) {
        user_cb(req, err);
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
    if (ctx->body_buf && ctx->body_buf_len > ctx->body_buf_pos && on_body) {
        size_t pre_data_len = ctx->body_buf_len - ctx->body_buf_pos;
        httpd_err_t err = on_body(req, &ctx->body_buf[ctx->body_buf_pos], pre_data_len);
        if (err != HTTPD_OK) {
            on_done(req, err);
            ctx->defer.active = false;
            conn->deferred = 0;
            return err;
        }
        req->body_received += pre_data_len;
        // Free body buffer after consuming
        free(ctx->body_buf);
        ctx->body_buf = NULL;
        ctx->body_buf_len = 0;
        ctx->body_buf_pos = 0;
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

    // Store file context inline in defer struct (no heap allocation needed)
    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    // Open file for writing
    ctx->defer.file_fp = fopen(path, "wb");
    if (!ctx->defer.file_fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", path);
        return HTTPD_ERR_IO;
    }

    ctx->defer.file_user_done_cb = on_done;

    ESP_LOGI(TAG, "Deferring body to file: %s (content_length=%zu)", path, req->content_length);

    return httpd_req_defer(req, defer_file_body_cb, defer_file_done_cb);
}

// ============================================================================
// Continuation-Based Body Reception (Non-Blocking)
// ============================================================================

httpd_err_t httpd_req_continue(httpd_req_t* req, httpd_continuation_t handler,
                               void* handler_state) {
    if (!req || !handler) {
        return HTTPD_ERR_INVALID_ARG;
    }

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn) {
        return HTTPD_ERR_CONN_CLOSED;
    }

    // Can't use both deferred and continuation
    if (conn->deferred) {
        ESP_LOGE(TAG, "Cannot use continuation mode - already in deferred mode");
        return HTTPD_ERR_INVALID_ARG;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));

    // Set up continuation state
    ctx->continuation.handler = handler;
    ctx->continuation.cont.state = handler_state;
    ctx->continuation.cont.phase = 0;
    ctx->continuation.cont.expected_bytes = 0;
    ctx->continuation.cont.received_bytes = 0;
    ctx->continuation.active = true;
    conn->continuation = 1;

    // Transition to body state
    conn->state = CONN_STATE_HTTP_BODY;

    ESP_LOGD(TAG, "Request using continuation mode, content_length=%zu, already_received=%zu",
             req->content_length, req->body_received);

    // Call handler for initial setup (data=NULL, len=0)
    httpd_err_t err = handler(req, NULL, 0, &ctx->continuation.cont);

    // Deliver any pre-received body data that came with headers
    if (err == HTTPD_ERR_WOULD_BLOCK && ctx->body_buf && ctx->body_buf_len > ctx->body_buf_pos) {
        size_t pre_data_len = ctx->body_buf_len - ctx->body_buf_pos;
        err = handler(req, &ctx->body_buf[ctx->body_buf_pos], pre_data_len, &ctx->continuation.cont);
        req->body_received += pre_data_len;
        ctx->continuation.cont.received_bytes += pre_data_len;

        // Free body buffer after consuming
        free(ctx->body_buf);
        ctx->body_buf = NULL;
        ctx->body_buf_len = 0;
        ctx->body_buf_pos = 0;
    }

    // Check result
    if (err == HTTPD_ERR_WOULD_BLOCK) {
        // Handler wants to wait for more data - this is expected
        // Check if body is already complete
        if (req->content_length > 0 && req->body_received >= req->content_length) {
            ESP_LOGD(TAG, "Body already complete, calling handler with completion");
            // Call handler one more time to signal completion
            err = handler(req, NULL, 0, &ctx->continuation.cont);
        }
    }

    if (err != HTTPD_OK && err != HTTPD_ERR_WOULD_BLOCK) {
        // Handler returned error - clean up
        ctx->continuation.active = false;
        conn->continuation = 0;
        return err;
    }

    if (err == HTTPD_OK) {
        // Handler finished immediately
        ctx->continuation.active = false;
        conn->continuation = 0;
    }

    return HTTPD_OK;
}

bool httpd_req_is_continuation(httpd_req_t* req) {
    if (!req) {
        return false;
    }

    request_context_t* ctx = (request_context_t*)((char*)req - offsetof(request_context_t, req));
    return ctx->continuation.active;
}

// ============================================================================
// Authentication
// ============================================================================

#include "mbedtls/base64.h"

/**
 * Constant-time string comparison to prevent timing attacks.
 * Always compares the full length regardless of where differences occur.
 */
static bool constant_time_compare_n(const char* a, size_t len_a, const char* b, size_t len_b) {
    // Use len_b to avoid leaking length of 'a' (the secret)
    volatile uint8_t result = len_a ^ len_b;
    size_t len = len_a < len_b ? len_a : len_b;
    for (size_t i = 0; i < len; i++) {
        result |= (uint8_t)(a[i] ^ b[i]);
    }
    return result == 0;
}

bool httpd_check_basic_auth(httpd_req_t* req, const char* username, const char* password) {
    if (!req || !username || !password) return false;

    const char* auth_header = httpd_req_get_header(req, "Authorization");
    if (!auth_header) return false;

    // Check for "Basic " prefix
    if (memcmp(auth_header, "Basic ", 6) != 0) return false;

    const char* encoded = auth_header + 6;

    // Decode base64 credentials
    unsigned char decoded[128];
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                     (const unsigned char*)encoded, strlen(encoded));
    if (ret != 0) return false;

    decoded[decoded_len] = '\0';

    // Format is "username:password"
    char* colon = strchr((char*)decoded, ':');
    if (!colon) return false;

    // Split at colon - lengths known from decode
    *colon = '\0';
    const char* recv_user = (char*)decoded;
    size_t recv_user_len = (size_t)(colon - (char*)decoded);
    const char* recv_pass = colon + 1;
    size_t recv_pass_len = decoded_len - recv_user_len - 1;

    // Compare credentials using constant-time comparison to prevent timing attacks
    bool user_ok = constant_time_compare_n(username, strlen(username), recv_user, recv_user_len);
    bool pass_ok = constant_time_compare_n(password, strlen(password), recv_pass, recv_pass_len);

    return user_ok && pass_ok;
}

httpd_err_t httpd_resp_send_auth_challenge(httpd_req_t* req, const char* realm) {
    if (!req) return HTTPD_ERR_INVALID_ARG;

    const char* actual_realm = realm ? realm : "Restricted";

    req->status_code = 401;

    // Build WWW-Authenticate header value
    char auth_value[128];
    static const char auth_prefix[] = "Basic realm=\"";
    memcpy(auth_value, auth_prefix, sizeof(auth_prefix) - 1);
    size_t realm_len = strlen(actual_realm);
    memcpy(auth_value + sizeof(auth_prefix) - 1, actual_realm, realm_len);
    auth_value[sizeof(auth_prefix) - 1 + realm_len] = '"';
    auth_value[sizeof(auth_prefix) + realm_len] = '\0';

    httpd_resp_set_header(req, "WWW-Authenticate", auth_value);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "401 Unauthorized", sizeof("401 Unauthorized") - 1);
}

// ============================================================================
// WebSocket Operations
// ============================================================================

httpd_err_t httpd_ws_accept(httpd_req_t* req, httpd_ws_t** ws_out) {
    if (!req || !ws_out) return HTTPD_ERR_INVALID_ARG;

    connection_t* conn = (connection_t*)req->_internal;
    if (!conn || !g_server) return HTTPD_ERR_CONN_CLOSED;

    // Send WebSocket handshake response
    if (ws_send_handshake_response(conn, req->ws_key) < 0) {
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

    connection_t* conn = (connection_t*)ws->_internal;
    if (!conn) return HTTPD_ERR_CONN_CLOSED;

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_LOCK_TCPIP();
#endif
    int ret = ws_send_frame(conn, opcode, (const uint8_t*)data, len, false);
#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_UNLOCK_TCPIP();
#endif

    if (ret < 0) {
        return HTTPD_ERR_IO;
    }

    return HTTPD_OK;
}

httpd_err_t httpd_ws_send_text(httpd_ws_t* ws, const char* text) {
    if (!text) return HTTPD_ERR_INVALID_ARG;
    return httpd_ws_send(ws, text, strlen(text), WS_TYPE_TEXT);
}

int httpd_ws_broadcast(httpd_handle_t handle, const char* pattern,
                       const void* data, size_t len, ws_type_t type) {
    struct httpd_server* server = handle;
    if (!server || !pattern || !data) return -1;

    ws_opcode_internal_t opcode = (type == WS_TYPE_BINARY) ? WS_OPCODE_BINARY : WS_OPCODE_TEXT;
    int sent = 0;

    // Resolve pattern to route_index once, then compare indices in the loop
    bool match_all = (strcmp(pattern, "*") == 0);
    uint8_t target_index = UINT8_MAX;
    if (!match_all) {
        for (int r = 0; r < server->ws_route_count; r++) {
            if (strcmp(server->ws_routes[r].pattern, pattern) == 0) {
                target_index = (uint8_t)r;
                break;
            }
        }
        if (target_index == UINT8_MAX) return 0;  // No matching route
    }

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_LOCK_TCPIP();
#endif

    // O(k) iteration where k = number of active WebSocket connections
    uint32_t mask = server->connection_pool.ws_active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);  // Get index of lowest set bit
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = &server->connection_pool.connections[i];
        ws_context_t* ws_ctx = ws_contexts[i];
        // Filter by route index: skip connections with no route or mismatched index
        if (!ws_ctx || !ws_ctx->route) continue;
        if (!match_all && ws_ctx->route_index != target_index) continue;
        if (ws_send_frame(conn, opcode, (const uint8_t*)data, len, false) >= 0) {
            sent++;
        }
    }

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_UNLOCK_TCPIP();
#endif

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

    connection_t* conn = (connection_t*)ws->_internal;

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_LOCK_TCPIP();
#endif

    ws_send_frame(conn, WS_OPCODE_CLOSE, close_data, close_len, false);

    // Per RFC 6455 section 5.5.1: after sending a Close frame, wait for the
    // client's Close frame before closing the TCP connection.
    if (conn && g_server) {
        conn->state = CONN_STATE_WS_CLOSING;
        conn->last_activity = g_server->event_loop.tick_count;
        // Remove from ws_active_mask so timeout checking applies
        connection_mark_ws_inactive(&g_server->connection_pool, conn->pool_index);
    } else {
        // Fallback: no connection context, close immediately
        ws->connected = false;
    }

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_UNLOCK_TCPIP();
#endif

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
    if (!g_server || !channel || !g_server->channel_hash) return -1;

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

// Find or create a channel index (-1 if full or allocation failed)
static int find_or_create_channel(const char* channel) {
    if (!g_server || !channel) return -1;

    // Lazy-allocate channel hash on first use
    if (!ensure_channel_hash(g_server)) return -1;

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
            entry->subscriber_count = 0;

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
    if (!ws || !channel || channel[0] == '\0') return HTTPD_ERR_INVALID_ARG;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    int idx = find_or_create_channel(channel);
    if (idx < 0) return HTTPD_ERR_NO_MEM;

    uint32_t bit = 1u << idx;
    if (!(ctx->channel_mask & bit)) {
        ctx->channel_mask |= bit;
        // Increment subscriber count on the channel entry
        channel_hash_entry_t* entry = g_server->channel_by_index[idx];
        if (entry) entry->subscriber_count++;
    }
    return HTTPD_OK;
}

httpd_err_t httpd_ws_leave(httpd_ws_t* ws, const char* channel) {
    if (!ws || !channel) return HTTPD_ERR_INVALID_ARG;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return HTTPD_ERR_INVALID_ARG;

    int idx = find_channel(channel);
    if (idx < 0) return HTTPD_ERR_NOT_FOUND;

    uint32_t bit = 1u << idx;
    if (!(ctx->channel_mask & bit)) {
        return HTTPD_ERR_NOT_FOUND;
    }

    ctx->channel_mask &= ~bit;
    // Decrement subscriber count on the channel entry
    channel_hash_entry_t* entry = g_server->channel_by_index[idx];
    if (entry && entry->subscriber_count > 0) entry->subscriber_count--;
    return HTTPD_OK;
}

void httpd_ws_leave_all(httpd_ws_t* ws) {
    if (!ws) return;

    ws_context_t* ctx = get_ws_context_from_ws(ws);
    if (!ctx) return;

    // Decrement subscriber count for each channel the connection was in
    uint32_t mask = ctx->channel_mask;
    while (mask && g_server) {
        int idx = __builtin_ctz(mask);  // Get lowest set bit
        mask &= mask - 1;              // Clear lowest set bit

        channel_hash_entry_t* entry = g_server->channel_by_index[idx];
        if (entry && entry->subscriber_count > 0) {
            entry->subscriber_count--;
        }
    }
    ctx->channel_mask = 0;
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
    if (!server || !channel || !data) return -1;

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

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_LOCK_TCPIP();
#endif

    // O(k) iteration where k = number of active WebSocket connections
    uint32_t ws_mask = server->connection_pool.ws_active_mask;
    while (ws_mask) {
        int i = __builtin_ctz(ws_mask);  // Get index of lowest set bit
        ws_mask &= ws_mask - 1;  // Clear lowest set bit

        ws_context_t* ctx = ws_contexts[i];
        if (ctx && (ctx->channel_mask & channel_mask)) {
            connection_t* conn = &server->connection_pool.connections[i];
            if (ws_send_frame(conn, opcode, (const uint8_t*)data, len, false) >= 0) {
                sent++;
            }
        }
    }

#ifdef CONFIG_HTTPD_USE_RAW_API
    HTTPD_UNLOCK_TCPIP();
#endif

    return sent;
}

unsigned int httpd_ws_channel_size(httpd_handle_t handle, const char* channel) {
    struct httpd_server* server = (struct httpd_server*)handle;
    if (!server || !channel) return 0;

    int idx = find_channel(channel);
    if (idx < 0) return 0;

    // O(1) lookup via maintained subscriber count
    channel_hash_entry_t* entry = server->channel_by_index[idx];
    return entry ? entry->subscriber_count : 0;
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

#ifdef CONFIG_ESPHTTPD_TEST_MODE
// Export _middleware_next for test access
httpd_err_t _middleware_next_test(httpd_req_t* req) {
    return _middleware_next(req);
}
#endif

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

    // Only initialize on the first call for this request (not on continuation)
    if (!ctx->parsing_in_progress) {
        init_request_context(ctx, conn);
        ctx->parsing_in_progress = true;
    }

    // Accumulate incoming data into the per-connection recv buffer.
    // First recv: allocate to exact size (typically 200-500 bytes for most
    // HTTP requests that complete in a single packet, saving ~3.5KB vs 4096).
    // Only grow to max size (4096) on PARSE_NEED_MORE (multi-packet headers).
    size_t new_total = ctx->recv_buf_len + len;
    if (new_total > 4096) {
        // Header data too large
        ESP_LOGE(TAG, "Request headers too large (%zu bytes)", new_total);
        ctx->parsing_in_progress = false;
        httpd_resp_send_error(&ctx->req, 431, "Request Header Fields Too Large");
        return;
    }
    if (!ctx->recv_buf) {
        // First recv: use inline buffer if data fits (avoids malloc for the
        // common single-packet case where headers fit in one recv call).
        if (len <= sizeof(ctx->inline_recv_buf)) {
            ctx->recv_buf = ctx->inline_recv_buf;
            ctx->recv_buf_capacity = sizeof(ctx->inline_recv_buf);
            ctx->recv_buf_is_heap = false;
        } else {
            ctx->recv_buf = (uint8_t*)malloc(len);
            if (!ctx->recv_buf) {
                ESP_LOGE(TAG, "Failed to allocate recv buffer");
                ctx->parsing_in_progress = false;
                httpd_resp_send_error(&ctx->req, 500, "Internal Server Error");
                return;
            }
            ctx->recv_buf_capacity = len;
            ctx->recv_buf_is_heap = true;
        }
    } else if (new_total > ctx->recv_buf_capacity) {
        // Subsequent recv: grow to max size so further appends never realloc.
        // Parser pointers from prior calls reference the old buffer, so we
        // must adjust them after the buffer moves.
        uint8_t* old_buf = ctx->recv_buf;
        uint8_t* new_buf;
        if (!ctx->recv_buf_is_heap) {
            // Currently using inline buffer - must malloc and copy
            new_buf = (uint8_t*)malloc(4096);
            if (!new_buf) {
                ESP_LOGE(TAG, "Failed to grow recv buffer");
                ctx->parsing_in_progress = false;
                httpd_resp_send_error(&ctx->req, 500, "Internal Server Error");
                return;
            }
            memcpy(new_buf, old_buf, ctx->recv_buf_len);
            ctx->recv_buf_is_heap = true;
        } else {
            new_buf = (uint8_t*)realloc(ctx->recv_buf, 4096);
            if (!new_buf) {
                ESP_LOGE(TAG, "Failed to grow recv buffer");
                ctx->parsing_in_progress = false;
                httpd_resp_send_error(&ctx->req, 500, "Internal Server Error");
                return;
            }
        }
        ctx->recv_buf = new_buf;
        ctx->recv_buf_capacity = 4096;
        // Fixup parser pointers that reference the old buffer
        if (new_buf != old_buf) {
            ptrdiff_t delta = new_buf - old_buf;
            if (ctx->parser_ctx.url)
                ctx->parser_ctx.url = (const uint8_t*)((const char*)ctx->parser_ctx.url + delta);
            if (ctx->parser_ctx.method)
                ctx->parser_ctx.method = (const uint8_t*)((const char*)ctx->parser_ctx.method + delta);
            if (ctx->parser_ctx.current_header_key)
                ctx->parser_ctx.current_header_key = (const uint8_t*)((const char*)ctx->parser_ctx.current_header_key + delta);
            if (ctx->parser_ctx.current_header_value)
                ctx->parser_ctx.current_header_value = (const uint8_t*)((const char*)ctx->parser_ctx.current_header_value + delta);
        }
    }
    size_t parse_offset = ctx->recv_buf_len;  // Bytes already parsed
    memcpy(ctx->recv_buf + ctx->recv_buf_len, buffer, len);
    ctx->recv_buf_len = new_total;

    // Parse incrementally: only pass the newly-received bytes to the parser.
    // The parser's state machine resumes from its current state and processes
    // only the new data. Parser pointers from earlier calls point into
    // recv_buf (which is stable/non-reallocating after first growth), and new
    // pointers set during this call point into recv_buf + parse_offset, also
    // valid. Headers are stored exactly once as they are parsed (no re-processing).
    parse_result_t result = http_parse_request(conn,
                                               ctx->recv_buf + parse_offset, len,
                                               &ctx->parser_ctx);

    if (result == PARSE_ERROR) {
        ctx->parsing_in_progress = false;
        httpd_resp_send_error(&ctx->req, 400, "Bad Request");
        return;
    }

    if (result == PARSE_NEED_MORE) {
        // Headers incomplete - wait for more data. The connection stays in
        // CONN_STATE_NEW or CONN_STATE_HTTP_HEADERS so the event loop will
        // call on_http_request again when more data arrives.
        return;
    }

    // Parsing complete (PARSE_OK or PARSE_COMPLETE). Parser pointers now
    // reference ctx->recv_buf which persists for the request lifetime.
    ctx->parsing_in_progress = false;

    // Adjust header_bytes: the parser set it relative to the buffer slice
    // we passed (recv_buf + parse_offset), so add the offset to get the
    // absolute position within the full recv_buf.
    conn->header_bytes += parse_offset;

    // Copy URL to request context (inline buffer or heap-allocated)
    if (ctx->parser_ctx.url && ctx->parser_ctx.url_len > 0) {
        size_t url_buf_needed = ctx->parser_ctx.url_len + 1;
        if (url_buf_needed <= sizeof(ctx->inline_uri_buf)) {
            ctx->uri_buf = ctx->inline_uri_buf;
            ctx->uri_buf_is_heap = false;
        } else {
            ctx->uri_buf = (char*)malloc(url_buf_needed);
            ctx->uri_buf_is_heap = true;
        }
        if (ctx->uri_buf) {
            memcpy(ctx->uri_buf, ctx->parser_ctx.url, ctx->parser_ctx.url_len);
            ctx->uri_buf[ctx->parser_ctx.url_len] = '\0';
            ctx->req.uri = ctx->uri_buf;
            ctx->req.uri_len = ctx->parser_ctx.url_len;

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
        } else {
            ESP_LOGE(TAG, "Failed to allocate URI buffer");
            httpd_resp_send_error(&ctx->req, 500, "Internal Server Error");
            return;
        }
    }

    // Set method
    ctx->req.method = (http_method_t)conn->method;

    // Check for WebSocket upgrade
    ctx->req.is_websocket = conn->upgrade_ws;
    if (conn->upgrade_ws) {
        // Point to WebSocket key in parser context (persists for request lifetime)
        ctx->req.ws_key = ctx->parser_ctx.ws_client_key;
    }

    // Store content length
    ctx->req.content_length = conn->content_length;

    // Save any body data that was received along with headers
    // conn->header_bytes contains the number of header bytes (including final CRLF)
    if (conn->content_length > 0 && ctx->recv_buf_len > conn->header_bytes) {
        size_t body_in_buffer = ctx->recv_buf_len - conn->header_bytes;
        // Dynamically allocate body buffer for pre-received data
        ctx->body_buf = (uint8_t*)malloc(body_in_buffer);
        if (ctx->body_buf) {
            memcpy(ctx->body_buf, ctx->recv_buf + conn->header_bytes, body_in_buffer);
            ctx->body_buf_len = body_in_buffer;
            ctx->body_buf_pos = 0;
        } else {
            // Allocation failed - body data will be lost but request can continue
            ESP_LOGW(TAG, "Failed to allocate body buffer (%zu bytes)", body_in_buffer);
            ctx->body_buf_len = 0;
            ctx->body_buf_pos = 0;
        }
    }

    // Free the recv accumulation buffer now that parsing is complete
    if (ctx->recv_buf) {
        if (ctx->recv_buf_is_heap) free(ctx->recv_buf);
        ctx->recv_buf = NULL;
        ctx->recv_buf_len = 0;
        ctx->recv_buf_capacity = 0;
        ctx->recv_buf_is_heap = false;
    }

    // Save original URL
    ctx->req.original_url = ctx->req.path;
    ctx->req.original_url_len = ctx->req.path_len;
    ctx->req.base_url = NULL;
    ctx->req.base_url_len = 0;

    // Handle CORS preflight requests before route matching
    // This allows OPTIONS requests to succeed even without explicit OPTIONS routes
    // Send complete response in single write (avoids 5x snprintf + 5x send overhead)
    if (g_server->config.enable_cors && ctx->req.method == HTTP_OPTIONS) {
        const char* cors_origin = g_server->config.cors_origin ? g_server->config.cors_origin : "*";
        send_cors_preflight(&ctx->req, conn, cors_origin);
        return;
    }

    // Check WebSocket routes first (before HTTP routes which may have catch-all patterns)
    if (ctx->req.is_websocket) {
        for (int i = 0; i < g_server->ws_route_count; i++) {
            if (strcmp(g_server->ws_routes[i].pattern, ctx->req.path) == 0) {
                // WebSocket route found - set up context
                ws_context_t* ws_ctx = get_ws_context(conn);
                ws_ctx->route = &g_server->ws_routes[i];
                ws_ctx->route_index = (uint8_t)i;
                ws_ctx->ws.fd = conn->fd;
                ws_ctx->ws._internal = conn;

                // Initialize frame context with pre-allocated buffer
                ws_frame_ctx_init(&ws_ctx->frame_ctx);

                // Send WebSocket handshake response FIRST (HTTP 101 Switching Protocols)
                // This MUST happen before the handler can send any WebSocket frames
                if (ws_send_handshake_response(conn, ctx->req.ws_key) < 0) {
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
    radix_match_t match;
    httpd_middleware_t route_mw[CONFIG_HTTPD_MAX_ROUTE_MIDDLEWARE];
    uint8_t route_mw_count = 0;
    httpd_router_t matched_router = NULL;  // httpd_router_t is already a pointer

    // Fast path: single router case (common configuration)
    if (__builtin_expect(g_server->mounted_router_count == 1, 1)) {
        mounted_router_t* mr = &g_server->mounted_routers[0];
        if (ctx->req.path_len >= mr->prefix_len &&
            memcmp(ctx->req.path, mr->prefix, mr->prefix_len) == 0) {
            const char* stripped_path = ctx->req.path + mr->prefix_len;
            if (stripped_path[0] == '\0') stripped_path = "/";
            radix_lookup(mr->router->tree, stripped_path,
                        ctx->req.method, ctx->req.is_websocket, &match,
                        route_mw, &route_mw_count);
            if (match.matched) {
                ctx->req.base_url = mr->prefix;
                ctx->req.base_url_len = mr->prefix_len;
                matched_router = mr->router;
                route_found = true;
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

                radix_lookup(mr->router->tree, stripped_path,
                            ctx->req.method, ctx->req.is_websocket, &match,
                            route_mw, &route_mw_count);

                if (match.matched) {
                    // Set base URL
                    ctx->req.base_url = mr->prefix;
                    ctx->req.base_url_len = mr->prefix_len;
                    matched_router = mr->router;
                    route_found = true;
                    break;
                }
            }
        }
    }

    // Fall back to legacy route table if no router matched (O(log n) radix tree lookup)
    // Reuse the same match variable to avoid a second radix_match_t on the stack
    if (!route_found && g_server->legacy_routes) {
        radix_lookup(g_server->legacy_routes, ctx->req.path,
                    ctx->req.method, false, &match, NULL, NULL);

        if (match.matched && match.handler) {
            route_found = true;

            // Copy parameters from radix match using memcpy (faster than field-by-field)
            uint8_t param_count = match.param_count < 8 ? match.param_count : 8;
            if (param_count > 0) {
                memcpy(ctx->req.params, match.params, param_count * sizeof(httpd_param_t));
            }
            ctx->req.param_count = match.param_count;

            // Build middleware chain: server global only (no router middleware for legacy routes)
            uint8_t mw_count = g_server->middleware_count;

            // Use memcpy instead of loop for middleware chain copying
            if (mw_count > 0) {
                memcpy(ctx->mw_chain, g_server->middlewares, mw_count * sizeof(httpd_middleware_t));
            }

            // Set up middleware context
            ctx->req._mw.chain = ctx->mw_chain;
            ctx->req._mw.chain_len = mw_count;
            ctx->req._mw.current = 0;
            ctx->req._mw.final_handler = match.handler;
            ctx->req._mw.final_user_ctx = match.user_ctx;
            ctx->req._mw.router = NULL;

            // Execute middleware chain
            httpd_err_t err = (mw_count > 0) ? _middleware_next(&ctx->req) : match.handler(&ctx->req);
            if (err != HTTPD_OK) {
                handle_error(err, &ctx->req);
            }

            return;
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
        uint8_t mw_count = 0;

        // Add server global middleware
        uint8_t server_mw = g_server->middleware_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE
                           ? g_server->middleware_count : CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE;
        if (server_mw > 0) {
            memcpy(ctx->mw_chain, g_server->middlewares, server_mw * sizeof(httpd_middleware_t));
            mw_count = server_mw;
        }

        // Add router middleware
        if (matched_router && mw_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t router_mw = matched_router->middleware_count < avail
                               ? matched_router->middleware_count : avail;
            if (router_mw > 0) {
                memcpy(ctx->mw_chain + mw_count, matched_router->middlewares, router_mw * sizeof(httpd_middleware_t));
                mw_count += router_mw;
            }
        }

        // Add route middleware (collected directly by radix_lookup into route_mw)
        if (route_mw_count > 0 && mw_count < CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE) {
            uint8_t avail = CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE - mw_count;
            uint8_t to_copy = route_mw_count < avail ? route_mw_count : avail;
            memcpy(ctx->mw_chain + mw_count, route_mw, to_copy * sizeof(httpd_middleware_t));
            mw_count += to_copy;
        }

        // Set up middleware context
        ctx->req._mw.chain = ctx->mw_chain;
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

        return;
    }

    // No route found
    handle_error(HTTPD_ERR_NOT_FOUND, &ctx->req);
}

static void on_http_body(connection_t* conn, uint8_t* buffer, size_t len) {
    // Quick check using connection bitfields - avoids context lookup for sync handlers
    // This is the hot path optimization: bitfields are O(1) vs get_request_context
    if (!conn->deferred && !conn->continuation) {
        // Non-deferred/non-continuation: body is received via non-blocking httpd_req_recv()
        return;
    }

    request_context_t* ctx = get_request_context(conn);
    if (!ctx) return;

    // Handle continuation-based body reception (non-blocking)
    if (conn->continuation && ctx->continuation.active) {
        ESP_LOGD(TAG, "Continuation body: received %zu bytes", len);

        // Call continuation handler
        httpd_err_t err = ctx->continuation.handler(&ctx->req, buffer, len,
                                                     &ctx->continuation.cont);

        // Track received bytes
        ctx->req.body_received += len;
        ctx->continuation.cont.received_bytes += len;

        ESP_LOGD(TAG, "Continuation body: total %zu/%zu, handler returned %d",
                 ctx->req.body_received, ctx->req.content_length, err);

        if (err == HTTPD_ERR_WOULD_BLOCK) {
            // Handler wants more data - check if body is complete
            if (ctx->req.content_length > 0 &&
                ctx->req.body_received >= ctx->req.content_length) {
                ESP_LOGD(TAG, "Continuation body complete, calling handler");
                // Call handler one more time to signal completion
                err = ctx->continuation.handler(&ctx->req, NULL, 0,
                                                 &ctx->continuation.cont);
            }
        }

        if (err != HTTPD_OK && err != HTTPD_ERR_WOULD_BLOCK) {
            // Handler returned error - clean up
            ESP_LOGW(TAG, "Continuation handler returned error: %d", err);
            ctx->continuation.active = false;
            conn->continuation = 0;
            conn->state = CONN_STATE_CLOSING;
        } else if (err == HTTPD_OK) {
            // Handler finished successfully
            ctx->continuation.active = false;
            conn->continuation = 0;
            // Connection stays open for the response to be sent
        }
        return;
    }

    // Handle deferred body reception
    if (!ctx->defer.active) return;

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
            if (conn->state == CONN_STATE_WS_CLOSING) {
                // Server-initiated close: client acknowledged our close frame.
                // Complete the disconnect now (RFC 6455 close handshake done).
                if (ws_ctx->route && ws_ctx->route->handler) {
                    httpd_ws_event_t disconnect_event = {
                        .type = WS_EVENT_DISCONNECT,
                        .data = NULL,
                        .len = 0
                    };
                    ws_ctx->route->handler(&ws_ctx->ws, &disconnect_event);
                }
                ws_ctx->ws.connected = false;
                ws_ctx->channel_mask = 0;
                conn->state = CONN_STATE_CLOSED;
            }
            // Client-initiated close (CONN_STATE_WEBSOCKET):
            // ws_handle_control_frame already echoed the close frame.
            // Wait for the client to close TCP (recv returns 0), which
            // triggers on_ws_disconnect via handle_connection_data.
            break;
        } else if (result == WS_FRAME_NEED_MORE) {
            // Need more data - wait for next read
            break;
        }
    }
}

// Called when a new connection is accepted - reset pre-allocated per-connection contexts
static void on_connect(connection_t* conn) {
    int idx = conn->pool_index;

    // No need to zero request_contexts or ws_contexts here:
    // - on_disconnect already NULLed freed pointers
    // - init_request_context will memset the full struct before first use

    // Reset pre-allocated send buffer (keeps buffer dealloc'd until needed)
    if (connection_send_buffers[idx]) {
        send_buffer_init(connection_send_buffers[idx]);
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
    int idx = conn->pool_index;
    request_context_t* ctx = get_request_context(conn);

    // Handle WebSocket disconnect if the WS disconnect handler was not
    // already called (e.g., WS_CLOSING timed out waiting for client ack)
    if (conn->is_websocket) {
        ws_context_t* ws_ctx = get_ws_context(conn);
        if (ws_ctx && ws_ctx->ws.connected) {
            on_ws_disconnect(conn);
        }
    }

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

    // Free dynamically-sized sub-buffers but only NULL the freed pointers.
    // init_request_context will memset the full struct before first use,
    // so a full memset here would be redundant.
    if (request_contexts[idx]) {
        if (request_contexts[idx]->uri_buf_is_heap) free(request_contexts[idx]->uri_buf);
        request_contexts[idx]->uri_buf = NULL;
        request_contexts[idx]->uri_buf_is_heap = false;
        free(request_contexts[idx]->header_buf);
        request_contexts[idx]->header_buf = NULL;
        free(request_contexts[idx]->body_buf);
        request_contexts[idx]->body_buf = NULL;
        if (request_contexts[idx]->recv_buf_is_heap) free(request_contexts[idx]->recv_buf);
        request_contexts[idx]->recv_buf = NULL;
        request_contexts[idx]->recv_buf_is_heap = false;
    }

    // Reset WebSocket context - free payload buffer, zero struct.
    // ws_context_t is small (~60 bytes) and has no init_* equivalent,
    // so memset here is the single source of truth for a clean slate.
    if (ws_contexts[idx]) {
        free(ws_contexts[idx]->frame_ctx.payload_buffer);
        memset(ws_contexts[idx], 0, sizeof(ws_context_t));
    }

    // Reset send buffer - free internal ring buffer, keep struct
    if (connection_send_buffers[idx]) {
        if (connection_send_buffers[idx]->allocated) {
            send_buffer_free(connection_send_buffers[idx]);
        } else {
            send_buffer_init(connection_send_buffers[idx]);
        }
    }
}

// Called by event loop when socket is writable and has pending data
static void on_write_ready(connection_t* conn) {
    send_buffer_t* sb = get_send_buffer(conn);
    request_context_t* ctx = get_request_context(conn);

    // Safety check for pre-allocated contexts
    if (!sb || !ctx) return;

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

#ifdef CONFIG_HTTPD_USE_RAW_API
            // Cannot block in tcpip_thread - submit to file I/O worker
            // Worker will call on_write_ready again after read completes
            if (!send_buffer_has_data(sb)) {
                // Only submit if we don't already have data to send
                file_io_submit_read(conn->pool_index, sb->file_fd, write_ptr, to_read);
                return;  // Will resume when worker signals back
            }
            // If we have data to send, send it first - worker will refill later
#else
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
#endif
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
                        int header_len = format_hex((char*)write_ptr, (size_t)bytes);
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

#ifdef CONFIG_HTTPD_USE_RAW_API
        ssize_t sent = raw_tcp_write(conn, data, len, false);
        if (sent <= 0) {
            if (sent == 0) {
                // No send buffer space, will retry on next sent callback
                return;
            }
            // Error
            ESP_LOGE(TAG, "Send error on conn [%d]", conn->pool_index);
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
#else
        ssize_t sent = send(conn->fd, data, len, MSG_DONTWAIT);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, will retry on next write-ready
                return;
            }
            // Real error - invoke async callback with error and close connection
            ESP_LOGE(TAG, "Send error on conn [%d]: %s", conn->pool_index, strerror(errno));
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
#endif

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
            send_buffer_free(sb);
        }

        // Check for async send completion
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
