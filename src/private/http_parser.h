#ifndef _HTTP_PARSER_H_
#define _HTTP_PARSER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parser states for streaming
typedef enum {
    PARSE_STATE_METHOD,
    PARSE_STATE_URL,
    PARSE_STATE_VERSION,
    PARSE_STATE_HEADER_KEY,
    PARSE_STATE_HEADER_VALUE,
    PARSE_STATE_HEADERS_COMPLETE,
    PARSE_STATE_BODY,
    PARSE_STATE_COMPLETE,
    PARSE_STATE_ERROR
} parse_state_t;

// Parser result codes
typedef enum {
    PARSE_OK,              // Parsing successful, continue
    PARSE_NEED_MORE,       // Need more data to continue
    PARSE_COMPLETE,        // Request/headers complete
    PARSE_ERROR            // Parse error occurred
} parse_result_t;

// Temporary parsing context (stack allocated)
typedef struct {
    parse_state_t state;
    uint16_t line_pos;      // Position in current line
    uint16_t header_count;  // Number of headers parsed
    bool expect_body;       // Expecting body based on method/headers

    // Pointers into the buffer being parsed (zero-copy)
    const uint8_t* method;
    uint8_t method_len;
    const uint8_t* url;
    uint8_t url_len;
    const uint8_t* current_header_key;
    uint8_t header_key_len;
    const uint8_t* current_header_value;
    uint8_t header_value_len;
} http_parser_context_t;

// Known header types for efficient processing
typedef enum {
    HEADER_UNKNOWN,
    HEADER_HOST,
    HEADER_CONTENT_LENGTH,
    HEADER_CONTENT_TYPE,
    HEADER_CONNECTION,
    HEADER_UPGRADE,
    HEADER_SEC_WEBSOCKET_KEY,
    HEADER_SEC_WEBSOCKET_VERSION,
    HEADER_AUTHORIZATION,
    HEADER_COOKIE,
    HEADER_ACCEPT,
    HEADER_USER_AGENT,
    HEADER_ORIGIN,
    HEADER_ACCESS_CONTROL_REQUEST_METHOD,
    HEADER_ACCESS_CONTROL_REQUEST_HEADERS
} header_type_t;

// Parse HTTP request line and headers from buffer
// Returns: Number of bytes consumed from buffer
parse_result_t http_parse_request(connection_t* conn,
                                 const uint8_t* buffer,
                                 size_t buffer_len,
                                 http_parser_context_t* ctx);

// Process a single header (called by parser)
void http_process_header(connection_t* conn,
                        const uint8_t* key, uint8_t key_len,
                        const uint8_t* value, uint8_t value_len);

// Utility functions for header processing
header_type_t http_identify_header(const uint8_t* key, uint8_t len);
http_method_t http_parse_method(const uint8_t* method, uint8_t len);

// Fast header value parsers
uint32_t http_parse_content_length(const uint8_t* value, uint8_t len);
bool http_parse_keep_alive(const uint8_t* value, uint8_t len);

// URL parsing utilities
bool http_parse_url_params(const uint8_t* url, uint8_t len,
                          uint16_t* path_len,
                          const uint8_t** params);

// Inline utilities for fast character checking
static inline bool is_token_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '!' ||
           c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
}

static inline bool is_whitespace(uint8_t c) {
    return c == ' ' || c == '\t';
}

static inline bool is_crlf(uint8_t c) {
    return c == '\r' || c == '\n';
}

// Case-insensitive comparison for header names
// Uses bitwise OR with 0x20 for fast ASCII lowercase conversion
// Optimized: uses single index for both arrays, reducing register pressure
static inline bool header_equals(const uint8_t* header, uint8_t len, const char* str) {
    size_t i = 0;
    while (str[i]) {
        if (__builtin_expect(i >= len, 0)) return false;
        // Fast case-insensitive compare using bitwise OR
        // 'A'-'Z' (0x41-0x5A) | 0x20 = 'a'-'z' (0x61-0x7A)
        uint8_t c1 = header[i] | 0x20;
        uint8_t c2 = str[i] | 0x20;
        if (c1 != c2) return false;
        i++;
    }
    return i == len;
}

#ifdef __cplusplus
}
#endif

#endif // _HTTP_PARSER_H_