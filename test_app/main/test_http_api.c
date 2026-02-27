/**
 * @file test_http_api.c
 * @brief Unit tests for HTTP response API and request parameter functions
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "send_buffer.h"
#include "http_parser.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_HTTP_API";

// Test server handle
static httpd_handle_t test_server = NULL;

// Mock send buffer for tests that need send_nonblocking
static send_buffer_t mock_send_buf;
static bool mock_send_buf_installed = false;

// Helper to start test server
static void start_test_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;
    httpd_start(&test_server, &config);
}

// Helper to stop test server
static void stop_test_server(void) {
    if (test_server) {
        httpd_stop(test_server);
        test_server = NULL;
    }
}

// Install a mock send buffer at pool_index 0 for tests that use mock connections.
// We seed the buffer with 1 byte so send_nonblocking queues data instead of
// attempting a send() syscall on the mock fd=-1.
static void install_mock_send_buffer(void) {
    if (!g_test_send_buffers) return;
    send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
    send_buffer_init(&mock_send_buf);
    send_buffer_alloc(&mock_send_buf);
    // Seed with a byte so send_nonblocking takes the queue path (skips send())
    uint8_t dummy = 0;
    send_buffer_queue(&mock_send_buf, &dummy, 1);
    bufs[0] = &mock_send_buf;
    mock_send_buf_installed = true;
}

// Remove mock send buffer
static void remove_mock_send_buffer(void) {
    if (!mock_send_buf_installed || !g_test_send_buffers) return;
    send_buffer_t** bufs = (send_buffer_t**)g_test_send_buffers;
    send_buffer_free(&mock_send_buf);
    bufs[0] = NULL;
    mock_send_buf_installed = false;
}

// Helper to create a mock request context
static void setup_mock_request(httpd_req_t* req, connection_t* conn) {
    memset(req, 0, sizeof(*req));
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1; // Mock FD (won't actually send)
    conn->state = CONN_STATE_HTTP_HEADERS;
    conn->pool_index = 0;
    req->_internal = conn;
    req->status_code = 200; // Default status
}

// ==================== Response Status Tests ====================

static void test_resp_set_status_ok(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_status(&req, 200);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(200, req.status_code);
}

static void test_resp_set_status_various_codes(void) {
    httpd_req_t req;
    connection_t conn;

    // Test common status codes
    int codes[] = {200, 201, 204, 301, 302, 304, 400, 401, 403, 404, 405, 500, 501, 502, 503};

    for (size_t i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        setup_mock_request(&req, &conn);
        httpd_err_t err = httpd_resp_set_status(&req, codes[i]);
        TEST_ASSERT_EQUAL(HTTPD_OK, err);
        TEST_ASSERT_EQUAL(codes[i], req.status_code);
    }
}

static void test_resp_set_status_null_req(void) {
    httpd_err_t err = httpd_resp_set_status(NULL, 200);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Response Header Tests ====================

static void test_resp_set_header_basic(void) {
    start_test_server();
    install_mock_send_buffer();

    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_header(&req, "X-Custom-Header", "custom-value");
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    remove_mock_send_buffer();
    stop_test_server();
}

static void test_resp_set_header_null_req(void) {
    httpd_err_t err = httpd_resp_set_header(NULL, "Key", "Value");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_resp_set_header_null_key(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_header(&req, NULL, "Value");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_resp_set_header_null_value(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_header(&req, "Key", NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Response Type Tests ====================

static void test_resp_set_type_json(void) {
    start_test_server();
    install_mock_send_buffer();

    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_type(&req, "application/json");
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    remove_mock_send_buffer();
    stop_test_server();
}

static void test_resp_set_type_html(void) {
    start_test_server();
    install_mock_send_buffer();

    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_err_t err = httpd_resp_set_type(&req, "text/html");
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    remove_mock_send_buffer();
    stop_test_server();
}

static void test_resp_set_type_null_req(void) {
    httpd_err_t err = httpd_resp_set_type(NULL, "text/plain");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Response Send Tests ====================

static void test_resp_send_null_req(void) {
    httpd_err_t err = httpd_resp_send(NULL, "body", 4);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_resp_send_error_null_req(void) {
    httpd_err_t err = httpd_resp_send_error(NULL, 500, "Error");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_resp_send_chunk_null_req(void) {
    httpd_err_t err = httpd_resp_send_chunk(NULL, "chunk", 5);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Query Parameter Tests ====================

static void test_req_get_query_not_found(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    char query[] = "name=test";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];
    int result = httpd_req_get_query(&req, "missing", buf, sizeof(buf));
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_null_req(void) {
    char buf[64];
    int result = httpd_req_get_query(NULL, "key", buf, sizeof(buf));
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_null_key(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    char query[] = "name=test";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];
    int result = httpd_req_get_query(&req, NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_null_buffer(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    char query[] = "name=test";
    req.query = query;
    req.query_len = strlen(query);

    int result = httpd_req_get_query(&req, "name", NULL, 64);
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_zero_buffer_size(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    char query[] = "name=test";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];
    int result = httpd_req_get_query(&req, "name", buf, 0);
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_no_query_string(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    req.query = NULL;
    req.query_len = 0;

    char buf[64];
    int result = httpd_req_get_query(&req, "key", buf, sizeof(buf));
    TEST_ASSERT_EQUAL(-1, result);
}

static void test_req_get_query_single_param(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // Single parameter - no & separator issues
    char query[] = "name=test";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];
    int result = httpd_req_get_query(&req, "name", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("test", buf);
}

static void test_req_get_query_multi_params(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // Multiple parameters - verifies url_decode is bounded to each value
    char query[] = "a=hello&b=world&c=foo";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];

    // First param should NOT include "&b=world&c=foo"
    int result = httpd_req_get_query(&req, "a", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("hello", buf);

    // Middle param
    result = httpd_req_get_query(&req, "b", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("world", buf);

    // Last param
    result = httpd_req_get_query(&req, "c", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("foo", buf);
}

static void test_req_get_query_encoded_multi_params(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // URL-encoded values across multiple params
    char query[] = "name=hello+world&tag=%23test";
    req.query = query;
    req.query_len = strlen(query);

    char buf[64];

    // '+' should decode to space, and stop at '&'
    int result = httpd_req_get_query(&req, "name", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("hello world", buf);

    // %23 should decode to '#'
    result = httpd_req_get_query(&req, "tag", buf, sizeof(buf));
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_EQUAL_STRING("#test", buf);
}

static void test_req_get_query_string_basic(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    char query[] = "name=test&value=123";
    req.query = query;
    req.query_len = strlen(query);

    const char* result = httpd_req_get_query_string(&req);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING("name=test&value=123", result);
}

static void test_req_get_query_string_null_req(void) {
    const char* result = httpd_req_get_query_string(NULL);
    TEST_ASSERT_NULL(result);
}

// ==================== URL Parameter Tests ====================

static void test_req_get_param_basic(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // Set up route parameters
    httpd_param_t params[2] = {
        {.key = "id", .value = "123", .key_len = 2, .value_len = 3},
        {.key = "name", .value = "test", .key_len = 4, .value_len = 4}
    };
    req.params[0] = params[0];
    req.params[1] = params[1];
    req.param_count = 2;

    const char* value = httpd_req_get_param(&req, "id");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("123", value);

    value = httpd_req_get_param(&req, "name");
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_STRING("test", value);
}

static void test_req_get_param_not_found(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_param_t params[1] = {
        {.key = "id", .value = "123", .key_len = 2, .value_len = 3}
    };
    req.params[0] = params[0];
    req.param_count = 1;

    const char* value = httpd_req_get_param(&req, "missing");
    TEST_ASSERT_NULL(value);
}

static void test_req_get_param_null_req(void) {
    const char* value = httpd_req_get_param(NULL, "id");
    TEST_ASSERT_NULL(value);
}

static void test_req_get_param_null_key(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    httpd_param_t params[1] = {
        {.key = "id", .value = "123", .key_len = 2, .value_len = 3}
    };
    req.params[0] = params[0];
    req.param_count = 1;

    const char* value = httpd_req_get_param(&req, NULL);
    TEST_ASSERT_NULL(value);
}

static void test_req_get_param_empty_params(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);
    req.param_count = 0;

    const char* value = httpd_req_get_param(&req, "id");
    TEST_ASSERT_NULL(value);
}

// ==================== HTTP Status Text Tests ====================

static void test_status_text_common_codes(void) {
    TEST_ASSERT_EQUAL_STRING("OK", httpd_status_text(200));
    TEST_ASSERT_EQUAL_STRING("Created", httpd_status_text(201));
    TEST_ASSERT_EQUAL_STRING("No Content", httpd_status_text(204));
    TEST_ASSERT_EQUAL_STRING("Moved Permanently", httpd_status_text(301));
    TEST_ASSERT_EQUAL_STRING("Found", httpd_status_text(302));
    TEST_ASSERT_EQUAL_STRING("Not Modified", httpd_status_text(304));
    TEST_ASSERT_EQUAL_STRING("Bad Request", httpd_status_text(400));
    TEST_ASSERT_EQUAL_STRING("Unauthorized", httpd_status_text(401));
    TEST_ASSERT_EQUAL_STRING("Forbidden", httpd_status_text(403));
    TEST_ASSERT_EQUAL_STRING("Not Found", httpd_status_text(404));
    TEST_ASSERT_EQUAL_STRING("Method Not Allowed", httpd_status_text(405));
    TEST_ASSERT_EQUAL_STRING("Internal Server Error", httpd_status_text(500));
    TEST_ASSERT_EQUAL_STRING("Not Implemented", httpd_status_text(501));
    TEST_ASSERT_EQUAL_STRING("Service Unavailable", httpd_status_text(503));
}

static void test_status_text_unknown_code(void) {
    TEST_ASSERT_EQUAL_STRING("Unknown", httpd_status_text(999));
    TEST_ASSERT_EQUAL_STRING("Unknown", httpd_status_text(0));
    TEST_ASSERT_EQUAL_STRING("Unknown", httpd_status_text(-1));
}

// ==================== MIME Type Tests ====================

static void test_mime_type_common_extensions(void) {
    // httpd_get_mime_type expects a file path, not just extension
    TEST_ASSERT_EQUAL_STRING("text/html", httpd_get_mime_type("index.html"));
    TEST_ASSERT_EQUAL_STRING("text/html", httpd_get_mime_type("page.htm"));
    TEST_ASSERT_EQUAL_STRING("text/css", httpd_get_mime_type("style.css"));
    TEST_ASSERT_EQUAL_STRING("application/javascript", httpd_get_mime_type("app.js"));
    TEST_ASSERT_EQUAL_STRING("application/json", httpd_get_mime_type("data.json"));
    TEST_ASSERT_EQUAL_STRING("image/png", httpd_get_mime_type("image.png"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", httpd_get_mime_type("photo.jpg"));
    TEST_ASSERT_EQUAL_STRING("image/jpeg", httpd_get_mime_type("photo.jpeg"));
    TEST_ASSERT_EQUAL_STRING("image/gif", httpd_get_mime_type("anim.gif"));
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", httpd_get_mime_type("icon.svg"));
    TEST_ASSERT_EQUAL_STRING("text/plain", httpd_get_mime_type("readme.txt"));
    TEST_ASSERT_EQUAL_STRING("application/xml", httpd_get_mime_type("config.xml"));
    TEST_ASSERT_EQUAL_STRING("application/pdf", httpd_get_mime_type("document.pdf"));
}

static void test_mime_type_unknown_extension(void) {
    const char* mime = httpd_get_mime_type("file.xyz");
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime);
}

static void test_mime_type_no_extension(void) {
    // File without extension
    const char* mime = httpd_get_mime_type("Makefile");
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime);
}

static void test_mime_type_empty_path(void) {
    const char* mime = httpd_get_mime_type("");
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mime);
}

// ==================== Basic Auth Tests ====================

static void test_check_basic_auth_null_req(void) {
    bool result = httpd_check_basic_auth(NULL, "user", "pass");
    TEST_ASSERT_FALSE(result);
}

static void test_check_basic_auth_null_user(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    bool result = httpd_check_basic_auth(&req, NULL, "pass");
    TEST_ASSERT_FALSE(result);
}

static void test_check_basic_auth_null_pass(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    bool result = httpd_check_basic_auth(&req, "user", NULL);
    TEST_ASSERT_FALSE(result);
}

static void test_check_basic_auth_no_header(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // No Authorization header set
    bool result = httpd_check_basic_auth(&req, "user", "pass");
    TEST_ASSERT_FALSE(result);
}

static void test_send_auth_challenge_null_req(void) {
    httpd_err_t err = httpd_resp_send_auth_challenge(NULL, "Test Realm");
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Content Length Tests ====================

static void test_req_get_content_length_set(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // httpd_req_get_content_length reads from req->content_length directly
    req.content_length = 1024;

    size_t len = httpd_req_get_content_length(&req);
    TEST_ASSERT_EQUAL(1024, len);
}

static void test_req_get_content_length_zero(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    req.content_length = 0;

    size_t len = httpd_req_get_content_length(&req);
    TEST_ASSERT_EQUAL(0, len);
}

static void test_req_get_content_length_null_req(void) {
    // Returns 0 for NULL (size_t is unsigned, can't return -1)
    size_t len = httpd_req_get_content_length(NULL);
    TEST_ASSERT_EQUAL(0, len);
}

// ==================== Request Method Tests ====================

static void test_req_get_method(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    // httpd_req_get_method reads from req->method directly
    req.method = HTTP_GET;
    http_method_t method = httpd_req_get_method(&req);
    TEST_ASSERT_EQUAL(HTTP_GET, method);

    req.method = HTTP_POST;
    method = httpd_req_get_method(&req);
    TEST_ASSERT_EQUAL(HTTP_POST, method);

    req.method = HTTP_PUT;
    method = httpd_req_get_method(&req);
    TEST_ASSERT_EQUAL(HTTP_PUT, method);

    req.method = HTTP_DELETE;
    method = httpd_req_get_method(&req);
    TEST_ASSERT_EQUAL(HTTP_DELETE, method);
}

// ==================== Test Runner ====================

void test_http_api_run(void) {
    ESP_LOGI(TAG, "Running HTTP API tests");

    // Response status tests
    RUN_TEST(test_resp_set_status_ok);
    RUN_TEST(test_resp_set_status_various_codes);
    RUN_TEST(test_resp_set_status_null_req);

    // Response header tests
    RUN_TEST(test_resp_set_header_basic);
    RUN_TEST(test_resp_set_header_null_req);
    RUN_TEST(test_resp_set_header_null_key);
    RUN_TEST(test_resp_set_header_null_value);

    // Response type tests
    RUN_TEST(test_resp_set_type_json);
    RUN_TEST(test_resp_set_type_html);
    RUN_TEST(test_resp_set_type_null_req);

    // Response send tests
    RUN_TEST(test_resp_send_null_req);
    RUN_TEST(test_resp_send_error_null_req);
    RUN_TEST(test_resp_send_chunk_null_req);

    // Query parameter tests
    RUN_TEST(test_req_get_query_not_found);
    RUN_TEST(test_req_get_query_null_req);
    RUN_TEST(test_req_get_query_null_key);
    RUN_TEST(test_req_get_query_null_buffer);
    RUN_TEST(test_req_get_query_zero_buffer_size);
    RUN_TEST(test_req_get_query_no_query_string);
    RUN_TEST(test_req_get_query_single_param);
    RUN_TEST(test_req_get_query_multi_params);
    RUN_TEST(test_req_get_query_encoded_multi_params);
    RUN_TEST(test_req_get_query_string_basic);
    RUN_TEST(test_req_get_query_string_null_req);

    // URL parameter tests
    RUN_TEST(test_req_get_param_basic);
    RUN_TEST(test_req_get_param_not_found);
    RUN_TEST(test_req_get_param_null_req);
    RUN_TEST(test_req_get_param_null_key);
    RUN_TEST(test_req_get_param_empty_params);

    // Status text tests
    RUN_TEST(test_status_text_common_codes);
    RUN_TEST(test_status_text_unknown_code);

    // MIME type tests
    RUN_TEST(test_mime_type_common_extensions);
    RUN_TEST(test_mime_type_unknown_extension);
    RUN_TEST(test_mime_type_no_extension);
    RUN_TEST(test_mime_type_empty_path);

    // Basic auth tests
    RUN_TEST(test_check_basic_auth_null_req);
    RUN_TEST(test_check_basic_auth_null_user);
    RUN_TEST(test_check_basic_auth_null_pass);
    RUN_TEST(test_check_basic_auth_no_header);
    RUN_TEST(test_send_auth_challenge_null_req);

    // Content length tests
    RUN_TEST(test_req_get_content_length_set);
    RUN_TEST(test_req_get_content_length_zero);
    RUN_TEST(test_req_get_content_length_null_req);

    // Request method tests
    RUN_TEST(test_req_get_method);

    ESP_LOGI(TAG, "HTTP API tests completed");
}
