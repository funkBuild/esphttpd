#include "unity.h"
#include "http_parser.h"
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

void test_http_parser_run(void)
{
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

    ESP_LOGI(TAG, "HTTP Parser tests completed");
}