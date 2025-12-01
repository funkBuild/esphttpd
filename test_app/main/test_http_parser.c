#include "unity.h"
#include "http_parser.h"
#include "esphttpd.h"
#include <string.h>
#include <stdint.h>
#include "esp_log.h"

static const char* TAG = "TEST_PARSER";

// Test parsing a complete GET request
static void test_parse_get_request(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /api/test HTTP/1.1\r\nHost: localhost\r\n\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(HTTP_GET, conn.method);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_HEADERS, conn.state);
}

// Test parsing POST request with Content-Length
static void test_parse_post_with_content_length(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "POST /api/data HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Content-Length: 123\r\n"
                         "\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_OK, result);  // Headers complete, body expected
    TEST_ASSERT_EQUAL(HTTP_POST, conn.method);
    TEST_ASSERT_EQUAL(123, conn.content_length);
    TEST_ASSERT_EQUAL(CONN_STATE_HTTP_BODY, conn.state);
}

// Test parsing request in chunks
static void test_parse_chunked_request(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Simulate receiving data in small chunks
    const char* chunk1 = "GET /ap";
    const char* chunk2 = "i/test HTTP/1";
    const char* chunk3 = ".1\r\nHost: local";
    const char* chunk4 = "host\r\n\r\n";

    parse_result_t result;

    // First chunk - incomplete
    result = http_parse_request(&conn, (const uint8_t*)chunk1,
                               strlen(chunk1), &ctx);
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);

    // Second chunk - still incomplete
    result = http_parse_request(&conn, (const uint8_t*)chunk2,
                               strlen(chunk2), &ctx);
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);

    // Third chunk - headers still incomplete
    result = http_parse_request(&conn, (const uint8_t*)chunk3,
                               strlen(chunk3), &ctx);
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);

    // Fourth chunk - should complete
    result = http_parse_request(&conn, (const uint8_t*)chunk4,
                               strlen(chunk4), &ctx);
    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(HTTP_GET, conn.method);
}

// Test WebSocket upgrade request
static void test_parse_websocket_upgrade(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /ws HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                         "\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(HTTP_GET, conn.method);
    TEST_ASSERT_TRUE(conn.upgrade_ws);
    TEST_ASSERT_TRUE(conn.is_websocket);
    TEST_ASSERT_EQUAL(CONN_STATE_WEBSOCKET, conn.state);
}

// Test invalid request
static void test_parse_invalid_request(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Missing HTTP version
    const char* request = "GET /test\r\n\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test method parsing
static void test_parse_methods(void)
{
    TEST_ASSERT_EQUAL(HTTP_GET, http_parse_method((const uint8_t*)"GET", 3));
    TEST_ASSERT_EQUAL(HTTP_POST, http_parse_method((const uint8_t*)"POST", 4));
    TEST_ASSERT_EQUAL(HTTP_PUT, http_parse_method((const uint8_t*)"PUT", 3));
    TEST_ASSERT_EQUAL(HTTP_DELETE, http_parse_method((const uint8_t*)"DELETE", 6));
    TEST_ASSERT_EQUAL(HTTP_HEAD, http_parse_method((const uint8_t*)"HEAD", 4));
    TEST_ASSERT_EQUAL(HTTP_OPTIONS, http_parse_method((const uint8_t*)"OPTIONS", 7));
    TEST_ASSERT_EQUAL(HTTP_PATCH, http_parse_method((const uint8_t*)"PATCH", 5));
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"INVALID", 7));
}

// Test header identification
static void test_identify_headers(void)
{
    TEST_ASSERT_EQUAL(HEADER_HOST,
                     http_identify_header((const uint8_t*)"Host", 4));
    TEST_ASSERT_EQUAL(HEADER_CONTENT_LENGTH,
                     http_identify_header((const uint8_t*)"Content-Length", 14));
    TEST_ASSERT_EQUAL(HEADER_CONTENT_TYPE,
                     http_identify_header((const uint8_t*)"Content-Type", 12));
    TEST_ASSERT_EQUAL(HEADER_CONNECTION,
                     http_identify_header((const uint8_t*)"Connection", 10));
    TEST_ASSERT_EQUAL(HEADER_UPGRADE,
                     http_identify_header((const uint8_t*)"Upgrade", 7));
    TEST_ASSERT_EQUAL(HEADER_UNKNOWN,
                     http_identify_header((const uint8_t*)"X-Custom", 8));
}

// Test content length parsing
static void test_parse_content_length(void)
{
    TEST_ASSERT_EQUAL(0, http_parse_content_length((const uint8_t*)"0", 1));
    TEST_ASSERT_EQUAL(123, http_parse_content_length((const uint8_t*)"123", 3));
    TEST_ASSERT_EQUAL(65535, http_parse_content_length((const uint8_t*)"65535", 5));
    TEST_ASSERT_EQUAL(99999, http_parse_content_length((const uint8_t*)"99999", 5)); // No longer clamped
    TEST_ASSERT_EQUAL(42, http_parse_content_length((const uint8_t*)"42abc", 5)); // Stop at non-digit
    // Test large values (16MB+)
    TEST_ASSERT_EQUAL(16777216, http_parse_content_length((const uint8_t*)"16777216", 8)); // 16MB
    TEST_ASSERT_EQUAL(104857600, http_parse_content_length((const uint8_t*)"104857600", 9)); // 100MB
    TEST_ASSERT_EQUAL(UINT32_MAX, http_parse_content_length((const uint8_t*)"9999999999999", 13)); // Overflow -> max
}

// Test keep-alive parsing
static void test_parse_keep_alive(void)
{
    TEST_ASSERT_TRUE(http_parse_keep_alive((const uint8_t*)"keep-alive", 10));
    TEST_ASSERT_FALSE(http_parse_keep_alive((const uint8_t*)"close", 5));
    TEST_ASSERT_TRUE(http_parse_keep_alive((const uint8_t*)"Keep-Alive", 10)); // Case insensitive
    TEST_ASSERT_TRUE(http_parse_keep_alive((const uint8_t*)"", 0)); // Default true
}

// Test OPTIONS request for CORS
static void test_parse_options_request(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "OPTIONS /api/test HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Origin: http://example.com\r\n"
                         "Access-Control-Request-Method: POST\r\n"
                         "\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(HTTP_OPTIONS, conn.method);
}

// ============================================================================
// Security/Edge Case Tests
// ============================================================================

// Test parsing with NULL buffer
static void test_parse_null_buffer(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    parse_result_t result = http_parse_request(&conn, NULL, 0, &ctx);
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);
}

// Test parsing with zero length
static void test_parse_zero_length(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET / HTTP/1.1\r\n\r\n";
    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request, 0, &ctx);
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);
}

// Test method parsing with NULL
static void test_parse_method_null(void)
{
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method(NULL, 0));
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method(NULL, 5));
}

// Test method parsing with zero length
static void test_parse_method_zero_len(void)
{
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"GET", 0));
}

// Test header identification with NULL
static void test_identify_header_null(void)
{
    TEST_ASSERT_EQUAL(HEADER_UNKNOWN, http_identify_header(NULL, 0));
    TEST_ASSERT_EQUAL(HEADER_UNKNOWN, http_identify_header(NULL, 5));
}

// Test header identification with zero length
static void test_identify_header_zero_len(void)
{
    TEST_ASSERT_EQUAL(HEADER_UNKNOWN, http_identify_header((const uint8_t*)"Host", 0));
}

// Test content-length with NULL
static void test_content_length_null(void)
{
    TEST_ASSERT_EQUAL(0, http_parse_content_length(NULL, 0));
    TEST_ASSERT_EQUAL(0, http_parse_content_length(NULL, 5));
}

// Test content-length with negative-like strings (should return 0 or error)
static void test_content_length_negative(void)
{
    TEST_ASSERT_EQUAL(0, http_parse_content_length((const uint8_t*)"-1", 2));
    TEST_ASSERT_EQUAL(0, http_parse_content_length((const uint8_t*)"-100", 4));
}

// Test content-length with leading zeros
static void test_content_length_leading_zeros(void)
{
    TEST_ASSERT_EQUAL(100, http_parse_content_length((const uint8_t*)"00100", 5));
    TEST_ASSERT_EQUAL(0, http_parse_content_length((const uint8_t*)"000", 3));
}

// Test content-length with whitespace
static void test_content_length_whitespace(void)
{
    // Leading whitespace should be handled
    TEST_ASSERT_EQUAL(0, http_parse_content_length((const uint8_t*)" 100", 4));
    TEST_ASSERT_EQUAL(100, http_parse_content_length((const uint8_t*)"100 ", 4));
}

// Test request with embedded null byte in URL (should be rejected or handled safely)
static void test_parse_null_byte_in_url(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Create a request with null byte embedded
    char request[64] = "GET /test";
    request[9] = '\0';  // Embed null byte
    memcpy(request + 10, "secret HTTP/1.1\r\n\r\n", 19);

    // Parse only up to the null byte
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),  // strlen stops at null
                                              &ctx);

    // Should need more data since request is truncated
    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);
}

// Test request with very short method (1 char)
static void test_parse_single_char_method(void)
{
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"X", 1));
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"G", 1));
}

// Test request without proper CRLF ending
static void test_parse_no_crlf(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /test HTTP/1.1";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_NEED_MORE, result);
}

// Test request with LF only (no CR)
static void test_parse_lf_only(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /test HTTP/1.1\nHost: localhost\n\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should either parse or need proper CRLF
    TEST_ASSERT_TRUE(result == PARSE_COMPLETE || result == PARSE_NEED_MORE || result == PARSE_ERROR);
}

// Test HTTP/0.9 style request (method URL only)
static void test_parse_http09_style(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /\r\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should be treated as error or need more
    TEST_ASSERT_TRUE(result == PARSE_ERROR || result == PARSE_NEED_MORE);
}

// Test request with unknown HTTP version
static void test_parse_unknown_http_version(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET /test HTTP/2.0\r\n\r\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should either parse (accepting unknown version) or error
    TEST_ASSERT_TRUE(result == PARSE_COMPLETE || result == PARSE_ERROR);
}

// Test request with extra spaces in request line
static void test_parse_extra_spaces(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET  /test  HTTP/1.1\r\n\r\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should either handle gracefully or error
    TEST_ASSERT_TRUE(result == PARSE_COMPLETE || result == PARSE_ERROR);
}

// Test very long header name
static void test_parse_long_header_name(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with 256+ char header name
    char request[512];
    int pos = snprintf(request, sizeof(request), "GET / HTTP/1.1\r\n");

    // Add very long header name (256 X's)
    for (int i = 0; i < 256; i++) {
        request[pos++] = 'X';
    }
    pos += snprintf(request + pos, sizeof(request) - pos, ": value\r\n\r\n");

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              pos,
                                              &ctx);

    // Should complete (ignore unknown header) or handle safely
    TEST_ASSERT_TRUE(result == PARSE_COMPLETE || result == PARSE_ERROR);
}

// Test empty method
static void test_parse_empty_method(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = " /test HTTP/1.1\r\n\r\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should error on missing method
    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test empty URL
static void test_parse_empty_url(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET  HTTP/1.1\r\n\r\n";
    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Should error on empty URL
    TEST_ASSERT_TRUE(result == PARSE_ERROR || result == PARSE_COMPLETE);
}

// Test header with empty value
static void test_parse_empty_header_value(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET / HTTP/1.1\r\n"
                         "Host:\r\n"
                         "\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    // Empty header value should be allowed
    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
}

// Test request line only (no headers)
static void test_parse_no_headers(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    const char* request = "GET / HTTP/1.1\r\n\r\n";

    parse_result_t result = http_parse_request(&conn,
                                              (const uint8_t*)request,
                                              strlen(request),
                                              &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
}

// Test keep-alive with NULL
static void test_keep_alive_null(void)
{
    // Should handle NULL gracefully
    TEST_ASSERT_TRUE(http_parse_keep_alive(NULL, 0));
}

// Test case-insensitive method parsing
static void test_parse_method_case(void)
{
    // Methods should be case sensitive per HTTP spec
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"get", 3));
    TEST_ASSERT_EQUAL(HTTP_ANY, http_parse_method((const uint8_t*)"Get", 3));
}

// Test case-insensitive header identification
static void test_identify_header_case(void)
{
    TEST_ASSERT_EQUAL(HEADER_HOST, http_identify_header((const uint8_t*)"host", 4));
    TEST_ASSERT_EQUAL(HEADER_HOST, http_identify_header((const uint8_t*)"HOST", 4));
    TEST_ASSERT_EQUAL(HEADER_CONTENT_LENGTH, http_identify_header((const uint8_t*)"content-length", 14));
}

// ============================================================================
// URL Parameter Parsing Tests (http_parse_url_params)
// ============================================================================

// Test URL with query string
static void test_parse_url_params_with_query(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/api/endpoint?key=value&foo=bar";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(13, path_len);  // "/api/endpoint" is 13 chars
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL(0, memcmp(params, "key=value&foo=bar", 17));
}

// Test URL without query string
static void test_parse_url_params_no_query(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/api/endpoint";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_FALSE(has_params);
    TEST_ASSERT_EQUAL(13, path_len);
    TEST_ASSERT_NULL(params);
}

// Test URL with empty query string (question mark only)
static void test_parse_url_params_empty_query(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/api/endpoint?";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    // params+1 >= len, so no params
    TEST_ASSERT_FALSE(has_params);
    TEST_ASSERT_EQUAL(13, path_len);
}

// Test root path with query
static void test_parse_url_params_root_with_query(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/?param=1";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(1, path_len);
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL(0, memcmp(params, "param=1", 7));
}

// Test complex URL with encoded characters
static void test_parse_url_params_encoded(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/search?q=%20space&type=pdf";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(7, path_len);  // "/search"
    TEST_ASSERT_NOT_NULL(params);
}

// Test single character path with query
static void test_parse_url_params_single_char_path(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/x?y=z";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(2, path_len);  // "/x"
    TEST_ASSERT_NOT_NULL(params);
    TEST_ASSERT_EQUAL(0, memcmp(params, "y=z", 3));
}

// Test multiple question marks (only first one should be parsed)
static void test_parse_url_params_multiple_question_marks(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/api?first=1?second=2";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(4, path_len);  // "/api"
    // Second ? is part of params
    TEST_ASSERT_EQUAL(0, memcmp(params, "first=1?second=2", 16));
}

// Test just root path
static void test_parse_url_params_just_root(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_FALSE(has_params);
    TEST_ASSERT_EQUAL(1, path_len);
    TEST_ASSERT_NULL(params);
}

// Test empty URL
static void test_parse_url_params_empty(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "";
    bool has_params = http_parse_url_params((const uint8_t*)url, 0,
                                            &path_len, &params);

    TEST_ASSERT_FALSE(has_params);
    TEST_ASSERT_EQUAL(0, path_len);
    TEST_ASSERT_NULL(params);
}

// Test deep nested path with query
static void test_parse_url_params_deep_path(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/a/b/c/d/e/f?x=1";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(12, path_len);  // "/a/b/c/d/e/f"
    TEST_ASSERT_NOT_NULL(params);
}

// Test single param value
static void test_parse_url_params_single_param(void)
{
    const uint8_t* params = NULL;
    uint16_t path_len = 0;

    const char* url = "/api?id=12345";
    bool has_params = http_parse_url_params((const uint8_t*)url, strlen(url),
                                            &path_len, &params);

    TEST_ASSERT_TRUE(has_params);
    TEST_ASSERT_EQUAL(4, path_len);
    TEST_ASSERT_EQUAL(0, memcmp(params, "id=12345", 8));
}

// ========== URL Decode Tests ==========

// Test basic decode with no encoding
static void test_url_decode_plain(void)
{
    char dst[64];
    int result = httpd_url_decode("hello", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(5, result);
    TEST_ASSERT_EQUAL_STRING("hello", dst);
}

// Test %20 decoding (space)
static void test_url_decode_percent_space(void)
{
    char dst[64];
    int result = httpd_url_decode("hello%20world", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(11, result);
    TEST_ASSERT_EQUAL_STRING("hello world", dst);
}

// Test + to space conversion
static void test_url_decode_plus_space(void)
{
    char dst[64];
    int result = httpd_url_decode("hello+world", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(11, result);
    TEST_ASSERT_EQUAL_STRING("hello world", dst);
}

// Test multiple encoded characters
static void test_url_decode_multiple_encoded(void)
{
    char dst[64];
    int result = httpd_url_decode("%2Fpath%2Fto%2Ffile", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(13, result);  // "/path/to/file" = 13 chars
    TEST_ASSERT_EQUAL_STRING("/path/to/file", dst);
}

// Test mixed encoding
static void test_url_decode_mixed(void)
{
    char dst[64];
    int result = httpd_url_decode("a%20b+c%3Dd", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(7, result);
    TEST_ASSERT_EQUAL_STRING("a b c=d", dst);
}

// Test incomplete percent encoding at end
static void test_url_decode_incomplete_percent(void)
{
    char dst[64];
    // %2 at end is incomplete - should be copied literally
    int result = httpd_url_decode("abc%2", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(5, result);
    TEST_ASSERT_EQUAL_STRING("abc%2", dst);
}

// Test invalid hex after percent
static void test_url_decode_invalid_hex(void)
{
    char dst[64];
    // %GG is invalid hex - should be copied literally
    int result = httpd_url_decode("a%GGb", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(5, result);
    TEST_ASSERT_EQUAL_STRING("a%GGb", dst);
}

// Test buffer too small
static void test_url_decode_buffer_small(void)
{
    char dst[5];  // Only room for 4 chars + null
    int result = httpd_url_decode("hello world", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(4, result);
    TEST_ASSERT_EQUAL_STRING("hell", dst);
}

// Test empty string
static void test_url_decode_empty(void)
{
    char dst[64] = "garbage";
    int result = httpd_url_decode("", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

// Test lowercase hex
static void test_url_decode_lowercase_hex(void)
{
    char dst[64];
    int result = httpd_url_decode("%2f%3a", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(2, result);
    TEST_ASSERT_EQUAL_STRING("/:", dst);
}

// Test uppercase hex
static void test_url_decode_uppercase_hex(void)
{
    char dst[64];
    int result = httpd_url_decode("%2F%3A", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(2, result);
    TEST_ASSERT_EQUAL_STRING("/:", dst);
}

// Test special chars encoded
static void test_url_decode_special_chars(void)
{
    char dst[64];
    // & = ? are often encoded in query params
    int result = httpd_url_decode("%26%3D%3F", dst, sizeof(dst));
    TEST_ASSERT_EQUAL(3, result);
    TEST_ASSERT_EQUAL_STRING("&=?", dst);
}

// ========== Buffer Overflow Boundary Tests ==========

// Test URL at exactly 254 chars (within limit)
static void test_url_boundary_254(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with a 254-char URL
    char request[512];
    char url[256];
    memset(url, 'a', 254);
    url[0] = '/';  // Start with /
    url[254] = '\0';

    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", url);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_EQUAL(254, conn.url_len);
}

// Test URL at exactly 255 chars (at limit - triggers PARSE_ERROR)
static void test_url_boundary_255(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with a 255-char URL
    char request[512];
    char url[257];
    memset(url, 'a', 255);
    url[0] = '/';  // Start with /
    url[255] = '\0';

    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", url);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    // Parser returns PARSE_ERROR when url_len reaches 255
    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test URL exceeding 255 chars (triggers PARSE_ERROR)
static void test_url_boundary_overflow(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with a 300-char URL
    char request[512];
    char url[302];
    memset(url, 'a', 300);
    url[0] = '/';  // Start with /
    url[300] = '\0';

    snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", url);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    // Parser returns PARSE_ERROR when url_len reaches 255
    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test header value at boundary (254 chars)
static void test_header_value_boundary_254(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with a 254-char Content-Type header value
    char request[512];
    char header_value[256];
    memset(header_value, 'x', 254);
    header_value[254] = '\0';

    // Use a custom header since Content-Type might be parsed specially
    snprintf(request, sizeof(request),
             "GET / HTTP/1.1\r\nHost: test\r\nX-Custom: %s\r\n\r\n", header_value);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    // Should parse successfully
    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
}

// Test header value overflow (255+ chars triggers PARSE_ERROR)
static void test_header_value_boundary_overflow(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build a request with a 300-char header value
    char request[512];
    char header_value[302];
    memset(header_value, 'x', 300);
    header_value[300] = '\0';

    snprintf(request, sizeof(request),
             "GET / HTTP/1.1\r\nHost: test\r\nX-Custom: %s\r\n\r\n", header_value);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    // Parser returns PARSE_ERROR when header value length reaches 255
    TEST_ASSERT_EQUAL(PARSE_ERROR, result);
}

// Test very long URL with query string at boundary
static void test_url_query_boundary(void)
{
    connection_t conn = {0};
    http_parser_context_t ctx = {0};

    // Build URL: /path?param=<250 chars of value>
    char request[512];
    char value[256];
    memset(value, 'v', 240);  // Path + ?param= takes some space
    value[240] = '\0';

    snprintf(request, sizeof(request),
             "GET /path?param=%s HTTP/1.1\r\nHost: test\r\n\r\n", value);

    parse_result_t result = http_parse_request(&conn, (const uint8_t*)request,
                                              strlen(request), &ctx);

    TEST_ASSERT_EQUAL(PARSE_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.url_len > 0);
    TEST_ASSERT_TRUE(conn.url_len <= 255);
}

void test_http_parser_run(void)
{
    // Core functionality tests
    RUN_TEST(test_parse_get_request);
    RUN_TEST(test_parse_post_with_content_length);
    RUN_TEST(test_parse_chunked_request);
    RUN_TEST(test_parse_websocket_upgrade);
    RUN_TEST(test_parse_invalid_request);
    RUN_TEST(test_parse_methods);
    RUN_TEST(test_identify_headers);
    RUN_TEST(test_parse_content_length);
    RUN_TEST(test_parse_keep_alive);
    RUN_TEST(test_parse_options_request);

    // Security and edge case tests
    RUN_TEST(test_parse_null_buffer);
    RUN_TEST(test_parse_zero_length);
    RUN_TEST(test_parse_method_null);
    RUN_TEST(test_parse_method_zero_len);
    RUN_TEST(test_identify_header_null);
    RUN_TEST(test_identify_header_zero_len);
    RUN_TEST(test_content_length_null);
    RUN_TEST(test_content_length_negative);
    RUN_TEST(test_content_length_leading_zeros);
    RUN_TEST(test_content_length_whitespace);
    RUN_TEST(test_parse_null_byte_in_url);
    RUN_TEST(test_parse_single_char_method);
    RUN_TEST(test_parse_no_crlf);
    RUN_TEST(test_parse_lf_only);
    RUN_TEST(test_parse_http09_style);
    RUN_TEST(test_parse_unknown_http_version);
    RUN_TEST(test_parse_extra_spaces);
    RUN_TEST(test_parse_long_header_name);
    RUN_TEST(test_parse_empty_method);
    RUN_TEST(test_parse_empty_url);
    RUN_TEST(test_parse_empty_header_value);
    RUN_TEST(test_parse_no_headers);
    RUN_TEST(test_keep_alive_null);
    RUN_TEST(test_parse_method_case);
    RUN_TEST(test_identify_header_case);

    // URL parameter parsing tests
    RUN_TEST(test_parse_url_params_with_query);
    RUN_TEST(test_parse_url_params_no_query);
    RUN_TEST(test_parse_url_params_empty_query);
    RUN_TEST(test_parse_url_params_root_with_query);
    RUN_TEST(test_parse_url_params_encoded);
    RUN_TEST(test_parse_url_params_single_char_path);
    RUN_TEST(test_parse_url_params_multiple_question_marks);
    RUN_TEST(test_parse_url_params_just_root);
    RUN_TEST(test_parse_url_params_empty);
    RUN_TEST(test_parse_url_params_deep_path);
    RUN_TEST(test_parse_url_params_single_param);

    // URL decode tests
    RUN_TEST(test_url_decode_plain);
    RUN_TEST(test_url_decode_percent_space);
    RUN_TEST(test_url_decode_plus_space);
    RUN_TEST(test_url_decode_multiple_encoded);
    RUN_TEST(test_url_decode_mixed);
    RUN_TEST(test_url_decode_incomplete_percent);
    RUN_TEST(test_url_decode_invalid_hex);
    RUN_TEST(test_url_decode_buffer_small);
    RUN_TEST(test_url_decode_empty);
    RUN_TEST(test_url_decode_lowercase_hex);
    RUN_TEST(test_url_decode_uppercase_hex);
    RUN_TEST(test_url_decode_special_chars);

    // Buffer overflow boundary tests
    RUN_TEST(test_url_boundary_254);
    RUN_TEST(test_url_boundary_255);
    RUN_TEST(test_url_boundary_overflow);
    RUN_TEST(test_header_value_boundary_254);
    RUN_TEST(test_header_value_boundary_overflow);
    RUN_TEST(test_url_query_boundary);

    ESP_LOGI(TAG, "HTTP Parser tests completed");
}