#ifndef _ESPHTTPD_H_
#define _ESPHTTPD_H_

/**
 * @file esphttpd.h
 * @brief High-performance HTTP/WebSocket server for ESP32
 *
 * Modern C API with proper error handling, thread safety, and extensibility.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Error Codes
// ============================================================================

typedef enum {
    HTTPD_OK = 0,                    ///< Success
    HTTPD_ERR_INVALID_ARG = -1,      ///< Invalid argument
    HTTPD_ERR_NO_MEM = -2,           ///< Out of memory
    HTTPD_ERR_NOT_FOUND = -3,        ///< Resource not found
    HTTPD_ERR_ROUTE_FULL = -4,       ///< Route table full
    HTTPD_ERR_ALREADY_RUNNING = -5,  ///< Server already running
    HTTPD_ERR_NOT_RUNNING = -6,      ///< Server not running
    HTTPD_ERR_CONN_CLOSED = -7,      ///< Connection closed
    HTTPD_ERR_TIMEOUT = -8,          ///< Operation timed out
    HTTPD_ERR_IO = -9,               ///< I/O error
    HTTPD_ERR_PARSE = -10,           ///< Parse error
    HTTPD_ERR_WS_REJECTED = -11,     ///< WebSocket rejected
    HTTPD_ERR_MIDDLEWARE = -12,      ///< Middleware error
} httpd_err_t;

// ============================================================================
// HTTP Methods (properly namespaced)
// ============================================================================

typedef enum {
    HTTP_GET     = 0,
    HTTP_POST    = 1,
    HTTP_PUT     = 2,
    HTTP_DELETE  = 3,
    HTTP_HEAD    = 4,
    HTTP_OPTIONS = 5,
    HTTP_PATCH   = 6,
    HTTP_ANY     = 7,   ///< Match any method (for middleware)
} http_method_t;

// ============================================================================
// WebSocket Types
// ============================================================================

typedef enum {
    WS_TYPE_TEXT       = 0x1,
    WS_TYPE_BINARY     = 0x2,
    WS_TYPE_CLOSE      = 0x8,
    WS_TYPE_PING       = 0x9,
    WS_TYPE_PONG       = 0xA,
} ws_type_t;

typedef enum {
    WS_EVENT_CONNECT,
    WS_EVENT_DISCONNECT,
    WS_EVENT_MESSAGE,
    WS_EVENT_ERROR,
} ws_event_type_t;

// ============================================================================
// Opaque Handle Types
// ============================================================================

typedef struct httpd_server* httpd_handle_t;
// httpd_req_t forward declared above with handler types
typedef struct httpd_ws_client httpd_ws_t;
typedef struct httpd_router* httpd_router_t;

// ============================================================================
// Configuration
// ============================================================================

/**
 * @brief Server configuration structure
 */
typedef struct {
    uint16_t port;                   ///< Port to listen on (default: 80)
    uint16_t max_connections;        ///< Maximum concurrent connections (default: 16)
    uint32_t timeout_ms;             ///< Connection timeout in ms (default: 30000)
    uint32_t recv_timeout_ms;        ///< Receive timeout in ms (default: 5000)
    size_t recv_buffer_size;         ///< Receive buffer size (default: 1024)
    size_t send_buffer_size;         ///< Send buffer size (default: 1024)
    size_t max_uri_len;              ///< Maximum URI length (default: 256)
    size_t max_header_len;           ///< Maximum single header length (default: 512)
    uint8_t max_headers;             ///< Maximum number of headers (default: 32)
    uint16_t backlog;                ///< Listen backlog (default: 5)
    size_t stack_size;               ///< Server task stack size (default: 8192)
    uint8_t task_priority;           ///< Server task priority (default: 5)
    bool enable_cors;                ///< Enable automatic CORS handling
    const char* cors_origin;         ///< CORS allowed origin (default: "*")
} httpd_config_t;

/**
 * @brief Default configuration initializer
 */
#define HTTPD_DEFAULT_CONFIG() {        \
    .port = 80,                         \
    .max_connections = 16,              \
    .timeout_ms = 30000,                \
    .recv_timeout_ms = 5000,            \
    .recv_buffer_size = 1024,           \
    .send_buffer_size = 1024,           \
    .max_uri_len = 256,                 \
    .max_header_len = 512,              \
    .max_headers = 32,                  \
    .backlog = 5,                       \
    .stack_size = 8192,                 \
    .task_priority = 5,                 \
    .enable_cors = false,               \
    .cors_origin = "*",                 \
}

// ============================================================================
// Forward Declarations
// ============================================================================

typedef struct httpd_request httpd_req_t;
struct httpd_ws_client;

// ============================================================================
// WebSocket Event (needed for handler typedef)
// ============================================================================

/**
 * @brief WebSocket event data
 */
typedef struct {
    ws_event_type_t type;            ///< Event type
    const uint8_t* data;             ///< Message data (for MESSAGE events)
    size_t len;                      ///< Data length
    ws_type_t frame_type;            ///< Frame type (text/binary)
} httpd_ws_event_t;

// ============================================================================
// Handler Types (must be defined before httpd_request struct)
// ============================================================================

/**
 * @brief HTTP request handler function
 * @param req Request context
 * @return HTTPD_OK on success, error code otherwise
 */
typedef httpd_err_t (*httpd_handler_t)(httpd_req_t* req);

/**
 * @brief Middleware next function type
 * @param req Request context
 * @return HTTPD_OK to continue, error to stop chain
 */
typedef httpd_err_t (*httpd_next_t)(httpd_req_t* req);

/**
 * @brief Middleware function type
 * @param req Request context
 * @param next Next handler in chain
 * @return HTTPD_OK to continue, error to stop chain
 */
typedef httpd_err_t (*httpd_middleware_t)(httpd_req_t* req, httpd_next_t next);

/**
 * @brief WebSocket event handler function
 * @param ws WebSocket client context
 * @param event Event data
 * @return HTTPD_OK on success, error code otherwise
 */
typedef httpd_err_t (*httpd_ws_handler_t)(httpd_ws_t* ws, httpd_ws_event_t* event);

/**
 * @brief Error handler function type
 * @param err Error code
 * @param req Request context
 * @return HTTPD_OK if error was handled, error code otherwise
 */
typedef httpd_err_t (*httpd_err_handler_t)(httpd_err_t err, httpd_req_t* req);

// ============================================================================
// Request Context (per-request state)
// ============================================================================

/**
 * @brief Route parameter (extracted from URI pattern)
 */
typedef struct {
    const char* key;                 ///< Parameter name (e.g., "id")
    const char* value;               ///< Parameter value (e.g., "123")
    uint8_t key_len;
    uint8_t value_len;
} httpd_param_t;

/**
 * @brief Request context with per-request state
 *
 * This structure holds all request-specific data, ensuring thread safety.
 * Users should not access fields directly; use accessor functions.
 */
struct httpd_request {
    // Connection info
    int fd;                          ///< Socket file descriptor
    void* _internal;                 ///< Internal connection pointer

    // Request line
    http_method_t method;            ///< HTTP method
    char* uri;                       ///< Full URI (owned by request)
    uint16_t uri_len;
    char* path;                      ///< Path portion of URI (may be stripped)
    uint16_t path_len;
    char* query;                     ///< Query string (after ?)
    uint16_t query_len;

    // Path properties (for mounted routers)
    const char* original_url;        ///< Full URL before prefix stripping
    uint16_t original_url_len;
    const char* base_url;            ///< Mount prefix (e.g., "/api")
    uint16_t base_url_len;

    // Headers (stored in request-local buffer)
    char* header_buf;                ///< Header storage buffer
    size_t header_buf_size;
    size_t header_buf_used;
    uint8_t header_count;

    // Route parameters
    httpd_param_t params[8];         ///< Extracted route parameters
    uint8_t param_count;

    // Body handling
    size_t content_length;           ///< Content-Length header value
    size_t body_received;            ///< Bytes of body received

    // Response state
    bool headers_sent;               ///< Response headers already sent
    int status_code;                 ///< Response status code

    // User data
    void* user_data;                 ///< User-defined context

    // Middleware execution context (internal)
    struct {
        httpd_middleware_t* chain;
        uint8_t chain_len;
        uint8_t current;
        httpd_handler_t final_handler;
        void* final_user_ctx;
        httpd_router_t router;       ///< Matched router (for error handler)
    } _mw;

    // WebSocket upgrade
    bool is_websocket;               ///< WebSocket upgrade requested
    char ws_key[32];                 ///< WebSocket key for handshake
};

// ============================================================================
// WebSocket Context
// ============================================================================

/**
 * @brief WebSocket client context
 */
struct httpd_ws_client {
    int fd;                          ///< Socket file descriptor
    void* _internal;                 ///< Internal connection pointer
    void* user_data;                 ///< User-defined context
    bool connected;                  ///< Connection active
};

// ============================================================================
// Route Definition
// ============================================================================

/**
 * @brief HTTP route definition
 */
typedef struct {
    http_method_t method;            ///< HTTP method to match
    const char* pattern;             ///< URI pattern (supports :param and *)
    httpd_handler_t handler;         ///< Request handler
    void* user_ctx;                  ///< User context passed to handler
} httpd_route_t;

/**
 * @brief WebSocket route definition
 */
typedef struct {
    const char* pattern;             ///< URI pattern
    httpd_ws_handler_t handler;      ///< WebSocket event handler
    void* user_ctx;                  ///< User context
    uint32_t ping_interval_ms;       ///< Auto-ping interval (0 = disabled)
} httpd_ws_route_t;

// ============================================================================
// Server Lifecycle
// ============================================================================

/**
 * @brief Start the HTTP server
 * @param[out] handle Server handle (set on success)
 * @param config Server configuration (NULL for defaults)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config);

/**
 * @brief Stop the HTTP server
 * @param handle Server handle
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_stop(httpd_handle_t handle);

/**
 * @brief Check if server is running
 * @param handle Server handle
 * @return true if running
 */
bool httpd_is_running(httpd_handle_t handle);

// ============================================================================
// Route Management
// ============================================================================

/**
 * @brief Register an HTTP route
 * @param handle Server handle
 * @param route Route definition
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_register_route(httpd_handle_t handle, const httpd_route_t* route);

/**
 * @brief Register a WebSocket route
 * @param handle Server handle
 * @param route WebSocket route definition
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_register_ws_route(httpd_handle_t handle, const httpd_ws_route_t* route);

/**
 * @brief Unregister a route by pattern
 * @param handle Server handle
 * @param method HTTP method
 * @param pattern URI pattern
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_unregister_route(httpd_handle_t handle, http_method_t method, const char* pattern);

// ============================================================================
// Middleware
// ============================================================================

/**
 * @brief Add global server-level middleware (applies to all routes)
 * @param handle Server handle
 * @param middleware Middleware function
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_use(httpd_handle_t handle, httpd_middleware_t middleware);

// ============================================================================
// Router API (Express-like nested routers)
// ============================================================================

/**
 * @brief Create a new router
 * @return Router handle, or NULL on failure
 */
httpd_router_t httpd_router_create(void);

/**
 * @brief Destroy a router and all its resources
 * @param router Router handle
 */
void httpd_router_destroy(httpd_router_t router);

/**
 * @brief Register GET route on router
 * @param router Router handle
 * @param pattern URI pattern (supports :param and *)
 * @param handler Request handler
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_router_get(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler);

/**
 * @brief Register POST route on router
 */
httpd_err_t httpd_router_post(httpd_router_t router, const char* pattern,
                              httpd_handler_t handler);

/**
 * @brief Register PUT route on router
 */
httpd_err_t httpd_router_put(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler);

/**
 * @brief Register DELETE route on router
 */
httpd_err_t httpd_router_delete(httpd_router_t router, const char* pattern,
                                httpd_handler_t handler);

/**
 * @brief Register PATCH route on router
 */
httpd_err_t httpd_router_patch(httpd_router_t router, const char* pattern,
                               httpd_handler_t handler);

/**
 * @brief Register route for all HTTP methods
 */
httpd_err_t httpd_router_all(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler);

/**
 * @brief Register route with context
 * @param router Router handle
 * @param pattern URI pattern
 * @param method HTTP method
 * @param handler Request handler
 * @param user_ctx User context passed to handler
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_router_route(httpd_router_t router, const char* pattern,
                               http_method_t method, httpd_handler_t handler,
                               void* user_ctx);

/**
 * @brief Register WebSocket route on router
 * @param router Router handle
 * @param pattern URI pattern
 * @param handler WebSocket handler
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_router_websocket(httpd_router_t router, const char* pattern,
                                   httpd_ws_handler_t handler);

/**
 * @brief Register WebSocket route with context
 */
httpd_err_t httpd_router_ws_route(httpd_router_t router, const char* pattern,
                                  httpd_ws_handler_t handler, void* user_ctx,
                                  uint32_t ping_interval_ms);

/**
 * @brief Add router-level middleware
 * @param router Router handle
 * @param middleware Middleware function
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_router_use(httpd_router_t router, httpd_middleware_t middleware);

/**
 * @brief Set error handler for router
 * @param router Router handle
 * @param handler Error handler function
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_router_on_error(httpd_router_t router, httpd_err_handler_t handler);

/**
 * @brief Mount a router at a prefix on the server
 * @param handle Server handle
 * @param prefix Mount path (e.g., "/api/v1")
 * @param router Router to mount
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_mount(httpd_handle_t handle, const char* prefix,
                        httpd_router_t router);

/**
 * @brief Set global error handler for server
 * @param handle Server handle
 * @param handler Error handler function
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_on_error(httpd_handle_t handle, httpd_err_handler_t handler);

// ============================================================================
// Request Information
// ============================================================================

/**
 * @brief Get request method
 */
http_method_t httpd_req_get_method(httpd_req_t* req);

/**
 * @brief Get full URI
 */
const char* httpd_req_get_uri(httpd_req_t* req);

/**
 * @brief Get URI path (without query string)
 */
const char* httpd_req_get_path(httpd_req_t* req);

/**
 * @brief Get original unmodified URL (before any prefix stripping)
 */
const char* httpd_req_get_original_url(httpd_req_t* req);

/**
 * @brief Get base URL (mount prefix, e.g., "/api")
 */
const char* httpd_req_get_base_url(httpd_req_t* req);

/**
 * @brief Get request header value
 * @param req Request context
 * @param key Header name (case-insensitive)
 * @return Header value or NULL if not found
 */
const char* httpd_req_get_header(httpd_req_t* req, const char* key);

/**
 * @brief Get route parameter (from :param in pattern)
 * @param req Request context
 * @param key Parameter name
 * @return Parameter value or NULL if not found
 */
const char* httpd_req_get_param(httpd_req_t* req, const char* key);

/**
 * @brief Get query string parameter
 * @param req Request context
 * @param key Parameter name
 * @param[out] value Buffer to store value
 * @param value_size Size of value buffer
 * @return Length of value, or -1 if not found
 */
int httpd_req_get_query(httpd_req_t* req, const char* key, char* value, size_t value_size);

/**
 * @brief Get raw query string
 */
const char* httpd_req_get_query_string(httpd_req_t* req);

/**
 * @brief Get Content-Length
 */
size_t httpd_req_get_content_length(httpd_req_t* req);

/**
 * @brief Receive request body data
 * @param req Request context
 * @param buf Buffer to receive data
 * @param len Maximum bytes to receive
 * @return Number of bytes received, 0 on EOF, negative on error
 */
int httpd_req_recv(httpd_req_t* req, void* buf, size_t len);

/**
 * @brief Get/set user data on request
 */
void* httpd_req_get_user_data(httpd_req_t* req);
void httpd_req_set_user_data(httpd_req_t* req, void* data);

// ============================================================================
// Response Building
// ============================================================================

/**
 * @brief Set response status code
 * @param req Request context
 * @param status HTTP status code
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_set_status(httpd_req_t* req, int status);

/**
 * @brief Set response header
 * @param req Request context
 * @param key Header name
 * @param value Header value
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_set_header(httpd_req_t* req, const char* key, const char* value);

/**
 * @brief Set Content-Type header
 */
httpd_err_t httpd_resp_set_type(httpd_req_t* req, const char* type);

/**
 * @brief Send complete response
 * @param req Request context
 * @param body Response body (can be NULL)
 * @param len Body length (-1 to use strlen)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send(httpd_req_t* req, const char* body, ssize_t len);

/**
 * @brief Send response chunk (for chunked transfer)
 * @param req Request context
 * @param chunk Chunk data
 * @param len Chunk length (-1 to use strlen, 0 to end)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send_chunk(httpd_req_t* req, const char* chunk, ssize_t len);

/**
 * @brief Send error response
 * @param req Request context
 * @param status HTTP status code
 * @param message Error message (NULL for default)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send_error(httpd_req_t* req, int status, const char* message);

/**
 * @brief Send file from filesystem
 * @param req Request context
 * @param path File path
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_sendfile(httpd_req_t* req, const char* path);

/**
 * @brief Send JSON response
 * @param req Request context
 * @param json JSON string
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send_json(httpd_req_t* req, const char* json);

// ============================================================================
// Asynchronous Response Sending
// ============================================================================

/**
 * @brief Callback invoked when async send completes
 * @param req Request context
 * @param err HTTPD_OK if sent successfully, error code otherwise
 */
typedef void (*httpd_send_cb_t)(httpd_req_t* req, httpd_err_t err);

/**
 * @brief Send response asynchronously (non-blocking)
 *
 * Queues the response for sending and returns immediately. The completion
 * callback is invoked when all data has been sent or an error occurs.
 * This allows the event loop to handle other connections while sending.
 *
 * The handler MUST return HTTPD_OK after calling this function successfully.
 * The connection remains open until the callback is invoked.
 *
 * @param req Request context
 * @param body Response body (can be NULL)
 * @param len Body length (-1 to use strlen)
 * @param on_done Callback when send completes (can be NULL)
 * @return HTTPD_OK if queued successfully, error code otherwise
 *
 * Example:
 * @code
 * static void send_done(httpd_req_t* req, httpd_err_t err) {
 *     if (err != HTTPD_OK) {
 *         ESP_LOGE(TAG, "Send failed: %d", err);
 *     }
 *     // Connection will be recycled/closed automatically
 * }
 *
 * static httpd_err_t handler(httpd_req_t* req) {
 *     char* large_data = generate_large_response();
 *     httpd_resp_set_status(req, 200);
 *     httpd_resp_set_type(req, "application/octet-stream");
 *     httpd_err_t err = httpd_resp_send_async(req, large_data, -1, send_done);
 *     free(large_data);
 *     return err;
 * }
 * @endcode
 */
httpd_err_t httpd_resp_send_async(httpd_req_t* req, const char* body, ssize_t len,
                                   httpd_send_cb_t on_done);

/**
 * @brief Send file with completion callback
 *
 * Sends a file from the filesystem. The callback is invoked when
 * the operation completes.
 *
 * @note Currently implemented using blocking I/O internally.
 * The callback is invoked before this function returns.
 * Future versions may implement true async streaming.
 *
 * @param req Request context
 * @param path File path (relative to filesystem base)
 * @param on_done Callback when send completes (can be NULL)
 * @return HTTPD_OK if sent successfully, error code otherwise
 */
httpd_err_t httpd_resp_sendfile_async(httpd_req_t* req, const char* path,
                                       httpd_send_cb_t on_done);

// ============================================================================
// Data Provider API (Streaming Response Generation)
// ============================================================================

/**
 * @brief Data provider callback - called when send buffer needs more data
 *
 * The server calls this when the send buffer has space available.
 * The callback writes data into the provided buffer and returns bytes written.
 *
 * @param req Request context (use httpd_req_get_user_data for state)
 * @param buf Buffer to write data into
 * @param max_len Maximum bytes to write
 * @return Bytes written (>0), 0 for EOF, or negative httpd_err_t on error
 *
 * Important:
 * - Must be non-blocking (no mutexes, no slow I/O)
 * - Return 0 to signal end of data
 * - Return negative error code to abort (connection will be closed)
 */
typedef ssize_t (*httpd_data_provider_t)(httpd_req_t* req, uint8_t* buf, size_t max_len);

/**
 * @brief Send response using a data provider callback
 *
 * Sets up streaming response where user code provides data on demand.
 * The provider callback is called whenever the send buffer has space.
 * This enables efficient streaming of large files, flash data, or
 * dynamically generated content without buffering the entire response.
 *
 * Flow:
 * 1. Handler calls httpd_resp_send_provider() with callbacks
 * 2. Handler returns HTTPD_OK immediately
 * 3. Event loop calls provider when socket writable and buffer has space
 * 4. Provider writes data to buffer and returns bytes written
 * 5. Server sends buffered data
 * 6. Repeat 3-5 until provider returns 0 (EOF)
 * 7. on_complete callback invoked when all data sent
 *
 * @param req Request context
 * @param content_length Total response size, or -1 for chunked encoding
 * @param provider Callback to provide data (required)
 * @param on_complete Callback when send completes (can be NULL)
 * @return HTTPD_OK if set up successfully
 *
 * Example - streaming from flash:
 * @code
 * typedef struct {
 *     const esp_partition_t* part;
 *     size_t offset;
 *     size_t remaining;
 * } flash_ctx_t;
 *
 * static ssize_t flash_provider(httpd_req_t* req, uint8_t* buf, size_t max_len) {
 *     flash_ctx_t* ctx = httpd_req_get_user_data(req);
 *     size_t to_read = (ctx->remaining < max_len) ? ctx->remaining : max_len;
 *     if (to_read == 0) return 0;  // EOF
 *     esp_partition_read(ctx->part, ctx->offset, buf, to_read);
 *     ctx->offset += to_read;
 *     ctx->remaining -= to_read;
 *     return to_read;
 * }
 *
 * static void flash_complete(httpd_req_t* req, httpd_err_t err) {
 *     free(httpd_req_get_user_data(req));
 * }
 *
 * static httpd_err_t handle_flash(httpd_req_t* req) {
 *     flash_ctx_t* ctx = malloc(sizeof(flash_ctx_t));
 *     ctx->part = esp_partition_find_first(...);
 *     ctx->offset = 0;
 *     ctx->remaining = ctx->part->size;
 *     httpd_req_set_user_data(req, ctx);
 *     httpd_resp_set_type(req, "application/octet-stream");
 *     return httpd_resp_send_provider(req, ctx->remaining, flash_provider, flash_complete);
 * }
 * @endcode
 */
httpd_err_t httpd_resp_send_provider(httpd_req_t* req, ssize_t content_length,
                                      httpd_data_provider_t provider,
                                      httpd_send_cb_t on_complete);

// ============================================================================
// Request Body Handling
// ============================================================================

/**
 * @brief Stream request body directly to a file
 *
 * Efficiently pipes the request body to a file, handling chunked reads
 * internally. Useful for file uploads and firmware OTA updates.
 *
 * @param req Request context
 * @param path File path to write to
 * @return Number of bytes written, or negative error code
 */
ssize_t httpd_req_pipe_to_file(httpd_req_t* req, const char* path);

/**
 * @brief Send HTTP 100 Continue response
 *
 * Used to acknowledge an "Expect: 100-continue" header from the client,
 * signaling that the server is ready to receive the request body.
 * Important for large uploads where client waits for server confirmation.
 *
 * @param req Request context
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send_continue(httpd_req_t* req);

// ============================================================================
// Deferred (Async) Request Handling
// ============================================================================

/**
 * @brief Body data callback for deferred requests
 *
 * Called each time a chunk of body data is received. The data pointer
 * is only valid during the callback - copy if you need to retain it.
 *
 * @param req Request context
 * @param data Pointer to received data
 * @param len Length of data
 * @return HTTPD_OK to continue, error code to abort
 */
typedef httpd_err_t (*httpd_body_cb_t)(httpd_req_t* req, const void* data, size_t len);

/**
 * @brief Completion callback for deferred requests
 *
 * Called when all body data has been received, or on error/disconnect.
 * This callback MUST send a response (or the connection will hang).
 * It is also responsible for cleaning up any user_data.
 *
 * @param req Request context
 * @param err HTTPD_OK if body received successfully, error code otherwise
 */
typedef void (*httpd_done_cb_t)(httpd_req_t* req, httpd_err_t err);

/**
 * @brief Defer request body handling to async callbacks
 *
 * Call this from a handler to switch from synchronous to asynchronous
 * body handling. After calling this, the handler should return HTTPD_OK
 * and body data will be delivered via the on_body callback.
 *
 * Typical usage:
 * 1. Handler validates headers, authenticates, etc. (sync)
 * 2. Handler calls httpd_req_defer() to set up async body handling
 * 3. Handler returns HTTPD_OK
 * 4. Body data arrives via on_body callback
 * 5. When complete, on_done is called to send response
 *
 * @param req Request context
 * @param on_body Called for each chunk of body data (NULL to discard body)
 * @param on_done Called when body complete or on error (required)
 * @return HTTPD_OK on success
 *
 * @code
 * static httpd_err_t on_body(httpd_req_t* req, const void* data, size_t len) {
 *     FILE* fp = httpd_req_get_user_data(req);
 *     fwrite(data, 1, len, fp);
 *     return HTTPD_OK;
 * }
 *
 * static void on_done(httpd_req_t* req, httpd_err_t err) {
 *     FILE* fp = httpd_req_get_user_data(req);
 *     fclose(fp);
 *     if (err == HTTPD_OK) {
 *         httpd_resp_send_json(req, "{\"status\":\"ok\"}");
 *     } else {
 *         httpd_resp_send_error(req, 500, "Upload failed");
 *     }
 * }
 *
 * static httpd_err_t handle_upload(httpd_req_t* req) {
 *     if (!httpd_check_basic_auth(req, "admin", "secret")) {
 *         return httpd_resp_send_auth_challenge(req, "Upload");
 *     }
 *     FILE* fp = fopen("/data/upload.bin", "wb");
 *     httpd_req_set_user_data(req, fp);
 *     return httpd_req_defer(req, on_body, on_done);
 * }
 * @endcode
 */
httpd_err_t httpd_req_defer(httpd_req_t* req, httpd_body_cb_t on_body, httpd_done_cb_t on_done);

/**
 * @brief Pause receiving body data (flow control)
 *
 * Use when processing can't keep up with receiving (e.g., slow storage).
 * Body callbacks will stop until httpd_req_defer_resume() is called.
 * TCP flow control will naturally back-pressure the client.
 *
 * @param req Request context
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_req_defer_pause(httpd_req_t* req);

/**
 * @brief Resume receiving body data
 *
 * @param req Request context
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_req_defer_resume(httpd_req_t* req);

/**
 * @brief Check if request is in deferred mode
 *
 * @param req Request context
 * @return true if httpd_req_defer() was called
 */
bool httpd_req_is_deferred(httpd_req_t* req);

/**
 * @brief Defer body directly to file (convenience helper)
 *
 * Opens the file, writes all body data to it, then calls on_done.
 * The on_done callback must send a response.
 *
 * @param req Request context
 * @param path File path to write
 * @param on_done Called when complete (must send response)
 * @return HTTPD_OK on success, HTTPD_ERR_IO if file can't be opened
 *
 * @code
 * static void upload_done(httpd_req_t* req, httpd_err_t err) {
 *     if (err == HTTPD_OK) {
 *         httpd_resp_send_json(req, "{\"status\":\"ok\"}");
 *     } else {
 *         httpd_resp_send_error(req, 500, "Upload failed");
 *     }
 * }
 *
 * static httpd_err_t handle_upload(httpd_req_t* req) {
 *     return httpd_req_defer_to_file(req, "/data/upload.bin", upload_done);
 * }
 * @endcode
 */
httpd_err_t httpd_req_defer_to_file(httpd_req_t* req, const char* path, httpd_done_cb_t on_done);

// ============================================================================
// Authentication
// ============================================================================

/**
 * @brief Verify HTTP Basic Authentication credentials
 *
 * Checks the Authorization header for valid Basic auth credentials.
 * Compares against provided username and password.
 *
 * @param req Request context
 * @param username Expected username
 * @param password Expected password
 * @return true if credentials match, false otherwise
 */
bool httpd_check_basic_auth(httpd_req_t* req, const char* username, const char* password);

/**
 * @brief Send HTTP 401 Unauthorized response with WWW-Authenticate header
 *
 * Sends a 401 response with the WWW-Authenticate header set to request
 * Basic authentication from the client.
 *
 * @param req Request context
 * @param realm Authentication realm (displayed to user in browser)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_resp_send_auth_challenge(httpd_req_t* req, const char* realm);

// ============================================================================
// WebSocket Operations
// ============================================================================

/**
 * @brief Accept WebSocket upgrade
 * @param req Request context (from HTTP handler)
 * @param[out] ws WebSocket context (set on success)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_ws_accept(httpd_req_t* req, httpd_ws_t** ws);

/**
 * @brief Reject WebSocket upgrade with status code
 * @param req Request context
 * @param status HTTP status code (e.g., 403, 401)
 * @param reason Optional reason message
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_ws_reject(httpd_req_t* req, int status, const char* reason);

/**
 * @brief Send WebSocket message
 * @param ws WebSocket context
 * @param data Message data
 * @param len Data length
 * @param type Frame type (WS_TYPE_TEXT or WS_TYPE_BINARY)
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_ws_send(httpd_ws_t* ws, const void* data, size_t len, ws_type_t type);

/**
 * @brief Send WebSocket text message
 */
httpd_err_t httpd_ws_send_text(httpd_ws_t* ws, const char* text);

/**
 * @brief Broadcast to all WebSocket clients on a route
 * @param handle Server handle
 * @param pattern Route pattern to match
 * @param data Message data
 * @param len Data length
 * @param type Frame type
 * @return Number of clients sent to, negative on error
 */
int httpd_ws_broadcast(httpd_handle_t handle, const char* pattern,
                       const void* data, size_t len, ws_type_t type);

/**
 * @brief Close WebSocket connection
 * @param ws WebSocket context
 * @param code Close status code
 * @param reason Close reason
 * @return HTTPD_OK on success
 */
httpd_err_t httpd_ws_close(httpd_ws_t* ws, uint16_t code, const char* reason);

/**
 * @brief Get number of active WebSocket connections
 */
unsigned int httpd_ws_get_connection_count(httpd_handle_t handle);

/**
 * @brief Get/set WebSocket user data
 */
void* httpd_ws_get_user_data(httpd_ws_t* ws);
void httpd_ws_set_user_data(httpd_ws_t* ws, void* data);

// ============================================================================
// WebSocket Channels (Pub/Sub)
// ============================================================================

/**
 * @brief Maximum number of channels supported
 */
#define HTTPD_WS_MAX_CHANNELS 32

/**
 * @brief Subscribe a WebSocket connection to a channel
 *
 * Channels are created lazily on first subscription. Channel names are
 * case-sensitive and stored internally (no need to keep the string).
 *
 * @param ws WebSocket connection
 * @param channel Channel name (max 31 chars)
 * @return HTTPD_OK on success, error code on failure
 */
httpd_err_t httpd_ws_join(httpd_ws_t* ws, const char* channel);

/**
 * @brief Unsubscribe a WebSocket connection from a channel
 *
 * @param ws WebSocket connection
 * @param channel Channel name
 * @return HTTPD_OK on success, HTTPD_ERR_NOT_FOUND if not subscribed
 */
httpd_err_t httpd_ws_leave(httpd_ws_t* ws, const char* channel);

/**
 * @brief Unsubscribe a WebSocket connection from all channels
 *
 * @param ws WebSocket connection
 */
void httpd_ws_leave_all(httpd_ws_t* ws);

/**
 * @brief Check if a WebSocket is subscribed to a channel
 *
 * @param ws WebSocket connection
 * @param channel Channel name
 * @return true if subscribed, false otherwise
 */
bool httpd_ws_in_channel(httpd_ws_t* ws, const char* channel);

/**
 * @brief Broadcast message to all subscribers of a channel
 *
 * @param handle Server handle
 * @param channel Channel name
 * @param data Message data
 * @param len Message length
 * @param type Message type (WS_TYPE_TEXT or WS_TYPE_BINARY)
 * @return Number of connections message was sent to
 */
int httpd_ws_publish(httpd_handle_t handle, const char* channel,
                     const void* data, size_t len, ws_type_t type);

/**
 * @brief Get number of subscribers in a channel
 *
 * @param handle Server handle
 * @param channel Channel name
 * @return Number of subscribers, 0 if channel doesn't exist
 */
unsigned int httpd_ws_channel_size(httpd_handle_t handle, const char* channel);

/**
 * @brief Get list of channels a WebSocket is subscribed to
 *
 * @param ws WebSocket connection
 * @param channels Array to store channel names (pointers to internal strings)
 * @param max_channels Maximum number of channels to return
 * @return Number of channels the connection is subscribed to
 */
unsigned int httpd_ws_get_channels(httpd_ws_t* ws, const char** channels, unsigned int max_channels);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * @brief URL decode a string
 * @param src Source string
 * @param dst Destination buffer
 * @param dst_size Destination buffer size
 * @return Length of decoded string, negative on error
 */
int httpd_url_decode(const char* src, char* dst, size_t dst_size);

/**
 * @brief Get status text for HTTP status code
 */
const char* httpd_status_text(int status);

/**
 * @brief Get MIME type for file extension
 */
const char* httpd_get_mime_type(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* _ESPHTTPD_H_ */
