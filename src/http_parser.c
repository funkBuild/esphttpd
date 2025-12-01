#include "private/http_parser.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "HTTP_PARSER";

// External WebSocket key buffer defined in esphttpd.c
extern char ws_client_key[32];

// External function to store headers (defined in esphttpd.c)
extern void esphttpd_store_header(const uint8_t* key, uint8_t key_len,
                                  const uint8_t* value, uint8_t value_len);

http_method_t http_parse_method(const uint8_t* method, uint8_t len) {
    // Guard against NULL or zero length
    if (method == NULL || len == 0) {
        return HTTP_ANY;
    }
    // Fast path using first character + length to minimize comparisons
    // Most common method (GET) is checked first with inline char comparison
    if (len == 3 && method[0] == 'G' && method[1] == 'E' && method[2] == 'T') {
        return HTTP_GET;
    }
    if (len == 4 && method[0] == 'P' && method[1] == 'O' && method[2] == 'S' && method[3] == 'T') {
        return HTTP_POST;
    }
    // Remaining methods use first-char dispatch
    switch (method[0]) {
        case 'P':
            if (len == 3 && method[1] == 'U' && method[2] == 'T') return HTTP_PUT;
            if (len == 5 && memcmp(method + 1, "ATCH", 4) == 0) return HTTP_PATCH;
            break;
        case 'H':
            if (len == 4 && memcmp(method + 1, "EAD", 3) == 0) return HTTP_HEAD;
            break;
        case 'D':
            if (len == 6 && memcmp(method + 1, "ELETE", 5) == 0) return HTTP_DELETE;
            break;
        case 'O':
            if (len == 7 && memcmp(method + 1, "PTIONS", 6) == 0) return HTTP_OPTIONS;
            break;
    }
    return HTTP_ANY;  // HTTP_ANY serves as unknown/any
}

header_type_t http_identify_header(const uint8_t* key, uint8_t len) {
    // Guard against NULL or zero length
    if (key == NULL || len == 0) {
        return HEADER_UNKNOWN;
    }
    // Normalize first char to lowercase once (eliminates duplicate case labels)
    uint8_t first = key[0] | 0x20;

    // Fast path for common headers using first character + length
    switch (first) {
        case 'h':
            if (len == 4 && header_equals(key, len, "Host")) {
                return HEADER_HOST;
            }
            break;
        case 'c':
            // Use nested switch for length-based dispatch (better branch prediction)
            switch (len) {
                case 14:
                    if (header_equals(key, len, "Content-Length"))
                        return HEADER_CONTENT_LENGTH;
                    break;
                case 12:
                    if (header_equals(key, len, "Content-Type"))
                        return HEADER_CONTENT_TYPE;
                    break;
                case 10:
                    if (header_equals(key, len, "Connection"))
                        return HEADER_CONNECTION;
                    break;
                case 6:
                    if (header_equals(key, len, "Cookie"))
                        return HEADER_COOKIE;
                    break;
            }
            break;
        case 'u':
            switch (len) {
                case 7:
                    if (header_equals(key, len, "Upgrade"))
                        return HEADER_UPGRADE;
                    break;
                case 10:
                    if (header_equals(key, len, "User-Agent"))
                        return HEADER_USER_AGENT;
                    break;
            }
            break;
        case 's':
            switch (len) {
                case 17:
                    if (header_equals(key, len, "Sec-WebSocket-Key"))
                        return HEADER_SEC_WEBSOCKET_KEY;
                    break;
                case 21:
                    if (header_equals(key, len, "Sec-WebSocket-Version"))
                        return HEADER_SEC_WEBSOCKET_VERSION;
                    break;
            }
            break;
        case 'a':
            switch (len) {
                case 13:
                    if (header_equals(key, len, "Authorization"))
                        return HEADER_AUTHORIZATION;
                    break;
                case 6:
                    if (header_equals(key, len, "Accept"))
                        return HEADER_ACCEPT;
                    break;
            }
            break;
        case 'o':
            if (len == 6 && header_equals(key, len, "Origin")) {
                return HEADER_ORIGIN;
            }
            break;
    }
    return HEADER_UNKNOWN;
}

uint32_t http_parse_content_length(const uint8_t* value, uint8_t len) {
    // Guard against NULL or zero length
    if (value == NULL || len == 0) {
        return 0;
    }
    // Fast path: if length > 10 digits, definitely overflows uint32_t (max 4294967295)
    if (len > 10) {
        return UINT32_MAX;
    }

    uint64_t result = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t c = value[i];
        if (c >= '0' && c <= '9') {
            result = result * 10 + (c - '0');
            // Early exit on overflow
            if (result > UINT32_MAX) {
                return UINT32_MAX;
            }
        } else {
            // Stop at first non-digit (handles trailing whitespace, etc.)
            break;
        }
    }

    return (uint32_t)result;
}

bool http_parse_keep_alive(const uint8_t* value, uint8_t len) {
    // Guard against NULL or zero length - default to keep-alive
    if (value == NULL || len == 0) {
        return true;
    }
    // Fast path: check for exact common values first (most HTTP clients send these)
    if (len == 10 && header_equals(value, 10, "keep-alive")) {
        return true;
    }
    if (len == 5 && header_equals(value, 5, "close")) {
        return false;
    }

    // Fallback: search for embedded values (e.g., "keep-alive, upgrade")
    for (uint8_t i = 0; i < len; i++) {
        uint8_t c = value[i] | 0x20;  // Fast lowercase
        if (c == 'k' && i + 10 <= len) {
            if (header_equals(value + i, 10, "keep-alive")) {
                return true;
            }
        } else if (c == 'c' && i + 5 <= len) {
            if (header_equals(value + i, 5, "close")) {
                return false;
            }
        }
    }
    return true; // Default to keep-alive for HTTP/1.1
}

void http_process_header(connection_t* conn,
                        const uint8_t* key, uint8_t key_len,
                        const uint8_t* value, uint8_t value_len) {
    // Store all headers for user access
    esphttpd_store_header(key, key_len, value, value_len);

    header_type_t type = http_identify_header(key, key_len);

    switch (type) {
        case HEADER_CONTENT_LENGTH:
            conn->content_length = http_parse_content_length(value, value_len);
            break;

        case HEADER_CONNECTION:
            conn->keep_alive = http_parse_keep_alive(value, value_len);
            break;

        case HEADER_UPGRADE:
            if (value_len >= 9 && header_equals(value, 9, "websocket")) {
                conn->upgrade_ws = 1;
            }
            break;

        case HEADER_SEC_WEBSOCKET_KEY:
            // Store WebSocket key for handshake (always store, regardless of header order)
            if (value_len < 32) {
                memcpy(ws_client_key, value, value_len);
                ws_client_key[value_len] = '\0';
                conn->is_websocket = 1;
            }
            break;

        default:
            // Other headers can be processed as needed
            break;
    }
}

parse_result_t http_parse_request(connection_t* __restrict conn,
                                 const uint8_t* __restrict buffer,
                                 size_t buffer_len,
                                 http_parser_context_t* __restrict ctx) {
    size_t i = 0;

    while (i < buffer_len) {
        uint8_t c = buffer[i];

        switch (ctx->state) {
            case PARSE_STATE_METHOD:
                if (!ctx->method) {
                    ctx->method = &buffer[i];
                    ctx->method_len = 0;
                }
                if (c == ' ') {
                    if (__builtin_expect(ctx->method_len == 0, 0)) {
                        return PARSE_ERROR;
                    }
                    // Parse the method
                    conn->method = http_parse_method(ctx->method, ctx->method_len);
                    ctx->state = PARSE_STATE_URL;
                    ctx->url = NULL;
                    ctx->url_len = 0;
                } else if (is_token_char(c)) {
                    ctx->method_len++;
                    if (__builtin_expect(ctx->method_len > 7, 0)) { // Max method length
                        return PARSE_ERROR;
                    }
                } else {
                    return PARSE_ERROR;
                }
                break;

            case PARSE_STATE_URL:
                if (!ctx->url) {
                    ctx->url = &buffer[i];
                    ctx->url_len = 0;
                }
                if (c == ' ') {
                    if (__builtin_expect(ctx->url_len == 0, 0)) {
                        return PARSE_ERROR;
                    }
                    // Store URL info in connection
                    // Note: In production, would copy URL to persistent storage
                    conn->url_len = ctx->url_len; // Already limited to uint8_t max (255)
                    ctx->state = PARSE_STATE_VERSION;
                } else if (__builtin_expect(c == '\r' || c == '\n', 0)) {
                    return PARSE_ERROR; // Invalid request line
                } else {
                    ctx->url_len++;
                    if (__builtin_expect(ctx->url_len == 255, 0)) { // Max URL length for uint8_t
                        return PARSE_ERROR;
                    }
                }
                break;

            case PARSE_STATE_VERSION:
                // Skip until end of line (we assume HTTP/1.1)
                if (c == '\r') {
                    // Expect \n next
                } else if (c == '\n') {
                    ctx->state = PARSE_STATE_HEADER_KEY;
                    ctx->current_header_key = NULL;
                    ctx->header_key_len = 0;
                    conn->header_bytes = 0;
                }
                break;

            case PARSE_STATE_HEADER_KEY:
                // Most common: regular header character (not \r, \n, or :)
                if (__builtin_expect(c == '\r', 0)) {
                    // Empty line, headers complete
                    if (ctx->header_key_len == 0) {
                        ctx->state = PARSE_STATE_HEADERS_COMPLETE;
                    }
                } else if (__builtin_expect(c == '\n' && ctx->header_key_len == 0, 0)) {
                    // Headers complete
                    ctx->state = PARSE_STATE_HEADERS_COMPLETE;
                } else if (c == ':') {
                    if (__builtin_expect(ctx->header_key_len == 0, 0)) {
                        return PARSE_ERROR;
                    }
                    ctx->state = PARSE_STATE_HEADER_VALUE;
                    ctx->current_header_value = NULL;
                    ctx->header_value_len = 0;
                } else {
                    if (!ctx->current_header_key) {
                        ctx->current_header_key = &buffer[i];
                        ctx->header_key_len = 0;
                    }
                    if (!is_whitespace(c)) {
                        ctx->header_key_len++;
                        if (__builtin_expect(ctx->header_key_len > 64, 0)) { // Max header key length
                            return PARSE_ERROR;
                        }
                    }
                }
                break;

            case PARSE_STATE_HEADER_VALUE:
                // Most common: regular header value character (not \r or \n)
                if (__builtin_expect(c == '\r', 0)) {
                    // End of header value
                    if (ctx->current_header_key && ctx->current_header_value) {
                        http_process_header(conn,
                                          ctx->current_header_key, ctx->header_key_len,
                                          ctx->current_header_value, ctx->header_value_len);
                    }
                    ctx->header_count++;
                } else if (__builtin_expect(c == '\n', 0)) {
                    ctx->state = PARSE_STATE_HEADER_KEY;
                    ctx->current_header_key = NULL;
                    ctx->header_key_len = 0;
                } else {
                    if (!ctx->current_header_value && !is_whitespace(c)) {
                        ctx->current_header_value = &buffer[i];
                        ctx->header_value_len = 0;
                    }
                    if (ctx->current_header_value) {
                        ctx->header_value_len++;
                        if (__builtin_expect(ctx->header_value_len == 255, 0)) { // Max header value length
                            return PARSE_ERROR;
                        }
                    }
                }
                break;

            case PARSE_STATE_HEADERS_COMPLETE:
                conn->state = CONN_STATE_HTTP_HEADERS;
                conn->header_bytes = i + 1;

                // Check if we expect a body
                if ((conn->method == HTTP_POST ||
                     conn->method == HTTP_PUT ||
                     conn->method == HTTP_PATCH) &&
                    conn->content_length > 0) {
                    conn->state = CONN_STATE_HTTP_BODY;
                    conn->bytes_received = 0;
                    ctx->state = PARSE_STATE_BODY;
                    // Return OK to indicate headers complete, body handling needed
                    return PARSE_OK;
                } else if (conn->is_websocket && conn->upgrade_ws) {
                    // WebSocket upgrade
                    conn->state = CONN_STATE_WEBSOCKET;
                    return PARSE_COMPLETE;
                } else {
                    // Request complete (no body)
                    return PARSE_COMPLETE;
                }
                break;

            case PARSE_STATE_BODY:
                // Body is handled elsewhere
                return PARSE_OK;

            case PARSE_STATE_COMPLETE:
            case PARSE_STATE_ERROR:
                return ctx->state == PARSE_STATE_COMPLETE ? PARSE_COMPLETE : PARSE_ERROR;
        }

        i++;

        // Prevent excessive header size (use loop counter directly instead of redundant field)
        if (i > 4096) {
            ESP_LOGE(TAG, "Headers too large");
            return PARSE_ERROR;
        }
    }

    return PARSE_NEED_MORE;
}

bool http_parse_url_params(const uint8_t* url, uint8_t len,
                          uint16_t* path_len,
                          const uint8_t** params) {
    // Use memchr for O(n) with optimized byte scanning
    const uint8_t* qmark = (const uint8_t*)memchr(url, '?', len);
    if (qmark) {
        *path_len = qmark - url;
        if (*path_len + 1 < len) {
            *params = qmark + 1;
            return true;
        }
        return false;
    }
    *path_len = len;
    *params = NULL;
    return false;
}