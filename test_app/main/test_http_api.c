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

// Headers must STAGE (not transmit) so the status line is emitted with the
// handler's final status code - regression for the CORS middleware freezing
// "200 OK" on the wire before the handler chose 404
static void test_resp_set_header_stages_until_send(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    uint8_t stage[128];
    req._resp_hdr_buf = stage;
    req._resp_hdr_cap = sizeof(stage);
    req._resp_hdr_len = 0;

    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_resp_set_header(&req, "X-Test", "abc"));
    // Nothing on the wire yet: status line not locked in
    TEST_ASSERT_FALSE(req.headers_sent);
    TEST_ASSERT_EQUAL(strlen("X-Test: abc\r\n"), req._resp_hdr_len);
    TEST_ASSERT_EQUAL_MEMORY("X-Test: abc\r\n", stage, req._resp_hdr_len);

    // Content-Type via set_header is tracked (suppresses implicit CT later)
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_resp_set_header(&req, "Content-Type", "text/csv"));
    TEST_ASSERT_TRUE(req.content_type_set);

    // Manual Content-Length is tracked
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_resp_set_header(&req, "Content-Length", "10"));
    TEST_ASSERT_TRUE(req.content_length_set);
}

// CR/LF in keys or values is response splitting - must be rejected
static void test_resp_set_header_rejects_crlf_injection(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    uint8_t stage[128];
    req._resp_hdr_buf = stage;
    req._resp_hdr_cap = sizeof(stage);
    req._resp_hdr_len = 0;

    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG,
        httpd_resp_set_header(&req, "X-Evil", "x\r\nInjected: 1"));
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG,
        httpd_resp_set_header(&req, "X\nBad", "v"));
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG,
        httpd_resp_set_header(&req, "X-Evil2", "trailing\r"));
    // Nothing staged from rejected headers
    TEST_ASSERT_EQUAL(0, req._resp_hdr_len);
}

// Staging buffer exhaustion reports NO_MEM instead of overflowing
static void test_resp_set_header_staging_overflow(void) {
    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    uint8_t stage[32];
    req._resp_hdr_buf = stage;
    req._resp_hdr_cap = sizeof(stage);
    req._resp_hdr_len = 0;

    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_resp_set_header(&req, "A", "12345678"));
    TEST_ASSERT_EQUAL(HTTPD_ERR_NO_MEM,
        httpd_resp_set_header(&req, "B", "this-value-does-not-fit-anymore"));
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

// Scatter-gather regression: a mid-size (1KB) response must arrive intact and
// in order — header block immediately followed by the body, behind any bytes
// already pending (the mock's seed byte). This drives send_nonblocking2's
// pure queue path (ring non-empty, fd=-1), which must work without writev
// ever being called. A forced-partial writev cannot be exercised here: the
// QEMU harness has no send/writev interposer, and mock connections
// deliberately avoid syscalls by keeping the ring non-empty.
static void test_resp_send_midsize_two_buffer_ordering(void) {
    start_test_server();
    install_mock_send_buffer();

    httpd_req_t req;
    connection_t conn;
    setup_mock_request(&req, &conn);

    static char body[1024];
    for (size_t i = 0; i < sizeof(body); i++) {
        body[i] = (char)('A' + (i % 23));  // Printable, no NULs (strstr-safe)
    }

    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_resp_send(&req, body, (ssize_t)sizeof(body)));
    TEST_ASSERT_TRUE(req.headers_sent);
    TEST_ASSERT_TRUE(req.body_started);

    // Drain the mock ring into a flat buffer (handles wrap segments)
    static uint8_t out[2048];
    size_t out_len = 0;
    const uint8_t* d;
    size_t l;
    while ((l = send_buffer_peek(&mock_send_buf, &d)) > 0) {
        TEST_ASSERT_TRUE(out_len + l <= sizeof(out));
        memcpy(out + out_len, d, l);
        out_len += l;
        send_buffer_consume(&mock_send_buf, l);
    }

    // The seed byte queued BEFORE the response must still be first: new bytes
    // may never be reordered ahead of pending ones
    TEST_ASSERT_TRUE(out_len > 1 + sizeof(body));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);

    // Header block starts with the status line
    char* resp = (char*)&out[1];
    size_t resp_len = out_len - 1;
    static const char status[] = "HTTP/1.1 200 OK\r\n";
    TEST_ASSERT_EQUAL_MEMORY(status, resp, sizeof(status) - 1);

    // NUL-terminate for strstr (body bytes are printable letters)
    TEST_ASSERT_TRUE(out_len < sizeof(out));
    resp[resp_len] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(resp, "Content-Length: 1024\r\n"));

    // Body immediately follows the header terminator, byte-exact, and is the
    // last thing in the stream
    char* hdr_end = strstr(resp, "\r\n\r\n");
    TEST_ASSERT_NOT_NULL(hdr_end);
    char* got_body = hdr_end + 4;
    TEST_ASSERT_EQUAL(sizeof(body), resp_len - (size_t)(got_body - resp));
    TEST_ASSERT_EQUAL_MEMORY(body, got_body, sizeof(body));

    remove_mock_send_buffer();
    stop_test_server();
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

// ==================== Data Provider (Fix 7) ====================

static bool s_provider_completed = false;
static httpd_err_t s_provider_complete_err = HTTPD_OK;

static void provider_on_complete(httpd_req_t* req, httpd_err_t err) {
    (void)req;
    s_provider_completed = true;
    s_provider_complete_err = err;
}

static ssize_t dummy_provider(httpd_req_t* req, uint8_t* buf, size_t max) {
    (void)req; (void)buf; (void)max;
    return 0;  // immediate EOF (only invoked from on_write_ready, not here)
}

// Fix 7: httpd_resp_send_provider must not leak the provider context. On the
// post-header send_buffer_alloc() failure path it now calls on_complete with an
// error and closes the connection (data_provider.active was never set, so
// on_disconnect would otherwise skip it). That malloc-failure branch is not
// directly reachable in the QEMU harness (no allocation-failure injection, and
// any earlier framing send already requires the buffer), so this guards the
// adjacent contract: the normal setup path stores on_complete and arms the
// provider so completion is guaranteed to run later.
static void test_resp_send_provider_arms_completion(void) {
    start_test_server();
    install_mock_send_buffer();
    s_provider_completed = false;
    s_provider_complete_err = HTTPD_OK;

    // Use pool slot 0 so get_request_context()/get_send_buffer() resolve to the
    // server's preallocated backing (the mock send buffer lives at slot 0).
    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_HTTP_HEADERS;

    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs);
    test_request_context_t* ctx = ctxs[0];
    TEST_ASSERT_NOT_NULL(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->req._internal = conn;
    ctx->req.status_code = 200;
    ctx->req.headers_sent = true;        // skip status/staged-header build
    ctx->req.content_length_set = true;  // framing block reduces to just CRLF

    httpd_err_t err = httpd_resp_send_provider(&ctx->req, 0, dummy_provider, provider_on_complete);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    // Provider armed; completion deferred to on_write_ready drain.
    TEST_ASSERT_TRUE(ctx->data_provider.active);
    TEST_ASSERT_FALSE(s_provider_completed);

    // Clear provider state so teardown/other tests are unaffected.
    ctx->data_provider.active = false;
    remove_mock_send_buffer();
    stop_test_server();
}

// Fix 2: a handler that arms a data provider and returns must NOT have its
// connection re-armed by finish_sync_request while the provider is active
// (re-arming let a pipelined request N+1 reach init_request_context, which
// memset the live provider state without firing on_complete). Once the
// response completes cleanly in on_write_ready, the completion path must
// fire on_complete AND re-arm the connection for the next keep-alive request.
static httpd_err_t prov_route_handler(httpd_req_t* req) {
    // content_length 0 + provider returning immediate EOF: the response
    // completes on the first on_write_ready pass without needing a real
    // socket to accept body bytes.
    return httpd_resp_send_provider(req, 0, dummy_provider, provider_on_complete);
}

static void test_provider_defers_rearm_until_completion(void) {
    start_test_server();
    install_mock_send_buffer();
    s_provider_completed = false;
    s_provider_complete_err = HTTPD_ERR_IO;

    httpd_route_t route = {
        .method = HTTP_GET, .pattern = "/prov2", .handler = prov_route_handler };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(test_server, &route));

    // Use the pool's slot-0 connection so context/send-buffer lookups resolve
    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    char req_bytes[] = "GET /prov2 HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Connection: keep-alive\r\n"
                       "\r\n";
    g_server->handlers.on_http_request(conn, (uint8_t*)req_bytes, sizeof(req_bytes) - 1);

    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs);
    test_request_context_t* ctx = ctxs[0];
    TEST_ASSERT_NOT_NULL(ctx);

    // Provider armed by the handler; completion has not fired yet
    TEST_ASSERT_TRUE(ctx->data_provider.active);
    TEST_ASSERT_FALSE(s_provider_completed);
    // NOT re-armed while the async response is in flight
    TEST_ASSERT_NOT_EQUAL(CONN_STATE_NEW, conn->state);
    TEST_ASSERT_NOT_EQUAL(CONN_STATE_CLOSED, conn->state);

    // Drain the queued header bytes (stands in for the socket accepting
    // them), then drive the write-ready path: the provider returns EOF
    // immediately, so the response completes on this pass.
    {
        const uint8_t* d;
        size_t l;
        while ((l = send_buffer_peek(&mock_send_buf, &d)) > 0) {
            send_buffer_consume(&mock_send_buf, l);
        }
    }
    g_server->handlers.on_write_ready(conn);

    // Completion fired with success, provider disarmed, and the connection
    // re-armed for the next keep-alive request
    TEST_ASSERT_TRUE(s_provider_completed);
    TEST_ASSERT_EQUAL(HTTPD_OK, s_provider_complete_err);
    TEST_ASSERT_FALSE(ctx->data_provider.active);
    TEST_ASSERT_EQUAL(CONN_STATE_NEW, conn->state);

    remove_mock_send_buffer();
    stop_test_server();
}

// ==================== In-Place (Zero-Copy) Header Storage ====================
// Headers are no longer copied into a separate heap buffer: keys/values are
// NUL-terminated in place inside recv_buf and the index stores offsets.

#define HDR_SNAP_LEN 64
static int hdr_handler_calls = 0;

static void hdr_snap(char* dst, const char* val) {
    snprintf(dst, HDR_SNAP_LEN, "%s", val ? val : "(null)");
}

static char hdr_snap_host[HDR_SNAP_LEN];
static char hdr_snap_pad[HDR_SNAP_LEN];
static char hdr_snap_case[HDR_SNAP_LEN];
static bool hdr_snap_empty_was_null = false;

static httpd_err_t hdr_route_handler(httpd_req_t* req) {
    hdr_handler_calls++;
    hdr_snap(hdr_snap_host, httpd_req_get_header(req, "Host"));
    hdr_snap(hdr_snap_pad, httpd_req_get_header(req, "X-Pad"));
    hdr_snap(hdr_snap_case, httpd_req_get_header(req, "x-cUsToM"));  // case-insensitive lookup
    hdr_snap_empty_was_null = (httpd_req_get_header(req, "X-Empty") == NULL);
    return HTTPD_OK;
}

// Headers must be readable via httpd_req_get_header during normal dispatch,
// with the exact semantics of the old copying path: leading OWS stripped,
// trailing OWS preserved, empty values never indexed, lookup case-insensitive.
static void test_req_get_header_in_place_during_dispatch(void) {
    start_test_server();

    httpd_route_t route = { .method = HTTP_GET, .pattern = "/hdr1", .handler = hdr_route_handler };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    hdr_handler_calls = 0;
    char req_bytes[] =
        "GET /hdr1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Pad:   padded value   \r\n"
        "X-Empty:\r\n"
        "X-Custom: MixedCase\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    g_server->handlers.on_http_request(conn, (uint8_t*)req_bytes, sizeof(req_bytes) - 1);

    TEST_ASSERT_EQUAL(1, hdr_handler_calls);
    TEST_ASSERT_EQUAL_STRING("localhost", hdr_snap_host);
    // Leading OWS excluded by the parser, trailing OWS included in the value
    TEST_ASSERT_EQUAL_STRING("padded value   ", hdr_snap_pad);
    TEST_ASSERT_EQUAL_STRING("MixedCase", hdr_snap_case);
    // An empty header value is never indexed (the parser never emits it)
    TEST_ASSERT_TRUE(hdr_snap_empty_was_null);

    stop_test_server();
}

static char hdr_iso_first[2][HDR_SNAP_LEN];
static char hdr_iso_second[2][HDR_SNAP_LEN];

static httpd_err_t hdr_iso_handler(httpd_req_t* req) {
    int call = hdr_handler_calls < 2 ? hdr_handler_calls : 1;
    hdr_handler_calls++;
    hdr_snap(hdr_iso_first[call], httpd_req_get_header(req, "X-First"));
    hdr_snap(hdr_iso_second[call], httpd_req_get_header(req, "X-Second"));
    return HTTPD_OK;
}

// Keep-alive: request N's headers must not leak into request N+1 - the
// header index (header_count) is reset per request even though the recv
// buffer is retained across the dispatch boundary.
static void test_req_get_header_keep_alive_isolation(void) {
    start_test_server();

    httpd_route_t route = { .method = HTTP_GET, .pattern = "/iso", .handler = hdr_iso_handler };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    hdr_handler_calls = 0;
    char req1[] =
        "GET /iso HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-First: one\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    g_server->handlers.on_http_request(conn, (uint8_t*)req1, sizeof(req1) - 1);
    TEST_ASSERT_EQUAL(1, hdr_handler_calls);
    TEST_ASSERT_EQUAL_STRING("one", hdr_iso_first[0]);
    TEST_ASSERT_EQUAL_STRING("(null)", hdr_iso_second[0]);
    // Connection re-armed for the next keep-alive request
    TEST_ASSERT_EQUAL(CONN_STATE_NEW, conn->state);

    char req2[] =
        "GET /iso HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Second: two\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    g_server->handlers.on_http_request(conn, (uint8_t*)req2, sizeof(req2) - 1);
    TEST_ASSERT_EQUAL(2, hdr_handler_calls);
    // Request 2 sees only its own headers
    TEST_ASSERT_EQUAL_STRING("(null)", hdr_iso_first[1]);
    TEST_ASSERT_EQUAL_STRING("two", hdr_iso_second[1]);

    // Index holds exactly request 2's headers (Host, X-Second, Connection)
    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs[0]);
    TEST_ASSERT_EQUAL(3, ctxs[0]->req.header_count);

    stop_test_server();
}

static char hdr_many_h15[HDR_SNAP_LEN];
static char hdr_many_h16[HDR_SNAP_LEN];

static httpd_err_t hdr_many_handler(httpd_req_t* req) {
    hdr_handler_calls++;
    hdr_snap(hdr_many_h15, httpd_req_get_header(req, "X-H15"));   // 16th indexed header
    hdr_snap(hdr_many_h16, httpd_req_get_header(req, "X-H16"));   // 17th: dropped
    return HTTPD_OK;
}

// A request with more headers than the index holds (MAX_REQ_HEADERS = 16)
// still parses and dispatches; the first 16 are readable, the rest dropped.
static void test_req_get_header_index_full(void) {
    start_test_server();

    httpd_route_t route = { .method = HTTP_GET, .pattern = "/many", .handler = hdr_many_handler };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    // Host + X-H01..X-H17 = 18 headers; Host + X-H01..X-H15 fill the index
    char req_bytes[1024];
    int n = snprintf(req_bytes, sizeof(req_bytes),
                     "GET /many HTTP/1.1\r\nHost: localhost\r\n");
    for (int i = 1; i <= 17 && n < (int)sizeof(req_bytes); i++) {
        n += snprintf(req_bytes + n, sizeof(req_bytes) - n, "X-H%02d: v%02d\r\n", i, i);
    }
    n += snprintf(req_bytes + n, sizeof(req_bytes) - n, "\r\n");
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(req_bytes));

    hdr_handler_calls = 0;
    g_server->handlers.on_http_request(conn, (uint8_t*)req_bytes, (size_t)n);

    TEST_ASSERT_EQUAL(1, hdr_handler_calls);
    TEST_ASSERT_EQUAL_STRING("v15", hdr_many_h15);
    TEST_ASSERT_EQUAL_STRING("(null)", hdr_many_h16);

    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs[0]);
    TEST_ASSERT_EQUAL(MAX_REQ_HEADERS, ctxs[0]->req.header_count);

    stop_test_server();
}

static char hdr_mig_early[HDR_SNAP_LEN];
static char hdr_mig_split[HDR_SNAP_LEN];
static char hdr_mig_late[HDR_SNAP_LEN];
static size_t hdr_mig_fill_len = 0;
static char hdr_mig_fill_first = 0;
static char hdr_mig_fill_last = 0;

static httpd_err_t hdr_mig_handler(httpd_req_t* req) {
    hdr_handler_calls++;
    hdr_snap(hdr_mig_early, httpd_req_get_header(req, "X-Early"));
    hdr_snap(hdr_mig_split, httpd_req_get_header(req, "X-Split"));
    hdr_snap(hdr_mig_late, httpd_req_get_header(req, "X-Late"));
    const char* fill = httpd_req_get_header(req, "X-Fill");
    if (fill) {
        hdr_mig_fill_len = strlen(fill);
        hdr_mig_fill_first = fill[0];
        hdr_mig_fill_last = fill[hdr_mig_fill_len - 1];
    } else {
        hdr_mig_fill_len = 0;
    }
    return HTTPD_OK;
}

// Multi-packet headers force the inline(512B)->heap migration of recv_buf.
// Offsets indexed BEFORE the migration must resolve correctly against the
// final heap base (contents move as a block), a value split across the two
// packets must reassemble, and the oversized heap buffer must be shrunk to
// the used size at dispatch.
static void test_req_get_header_survives_recv_buf_migration(void) {
    start_test_server();

    httpd_route_t route = { .method = HTTP_GET, .pattern = "/mig", .handler = hdr_mig_handler };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(test_server, &route));

    connection_t* conn = connection_get(&g_server->connection_pool, 0);
    TEST_ASSERT_NOT_NULL(conn);
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->pool_index = 0;
    conn->state = CONN_STATE_NEW;

    // Chunk 1 (fits inline, ends mid-value of X-Split, no line terminator)
    char chunk1[] =
        "GET /mig HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Early: before-move\r\n"
        "X-Split: part1";

    // Chunk 2: completes X-Split, then a 400-byte value to push the total
    // past the 512-byte inline buffer (forces malloc + memcpy + rebase)
    char chunk2[600];
    int n = snprintf(chunk2, sizeof(chunk2), "part2\r\nX-Fill: ");
    memset(chunk2 + n, 'b', 400);
    n += 400;
    n += snprintf(chunk2 + n, sizeof(chunk2) - n,
                  "\r\nX-Late: after-move\r\nConnection: keep-alive\r\n\r\n");
    TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(chunk2));

    hdr_handler_calls = 0;
    g_server->handlers.on_http_request(conn, (uint8_t*)chunk1, sizeof(chunk1) - 1);
    TEST_ASSERT_EQUAL(0, hdr_handler_calls);  // headers incomplete, no dispatch yet
    g_server->handlers.on_http_request(conn, (uint8_t*)chunk2, (size_t)n);
    TEST_ASSERT_EQUAL(1, hdr_handler_calls);

    // Header indexed before the migration resolves against the final base
    TEST_ASSERT_EQUAL_STRING("before-move", hdr_mig_early);
    // Value split across the two packets reassembles
    TEST_ASSERT_EQUAL_STRING("part1part2", hdr_mig_split);
    TEST_ASSERT_EQUAL_STRING("after-move", hdr_mig_late);
    TEST_ASSERT_EQUAL(400, hdr_mig_fill_len);
    TEST_ASSERT_EQUAL('b', hdr_mig_fill_first);
    TEST_ASSERT_EQUAL('b', hdr_mig_fill_last);

    // recv_buf migrated to heap and was shrunk from 4096 to the used size
    test_request_context_t** ctxs = (test_request_context_t**)g_test_request_contexts;
    TEST_ASSERT_NOT_NULL(ctxs[0]);
    TEST_ASSERT_TRUE(ctxs[0]->recv_buf_is_heap);
    TEST_ASSERT_EQUAL(ctxs[0]->recv_buf_len, ctxs[0]->recv_buf_capacity);
    TEST_ASSERT_TRUE(ctxs[0]->recv_buf_capacity < 4096);

    // Headers stay readable AFTER on_http_request returned (request-lifetime
    // retention on the sync path; the shrink realloc kept offsets valid)
    const char* early = httpd_req_get_header(&ctxs[0]->req, "X-Early");
    TEST_ASSERT_NOT_NULL(early);
    TEST_ASSERT_EQUAL_STRING("before-move", early);

    stop_test_server();
}

void test_http_api_run(void) {
    ESP_LOGI(TAG, "Running HTTP API tests");

    // Data provider setup / completion arming
    RUN_TEST(test_resp_send_provider_arms_completion);
    RUN_TEST(test_provider_defers_rearm_until_completion);

    // In-place (zero-copy) header storage
    RUN_TEST(test_req_get_header_in_place_during_dispatch);
    RUN_TEST(test_req_get_header_keep_alive_isolation);
    RUN_TEST(test_req_get_header_index_full);
    RUN_TEST(test_req_get_header_survives_recv_buf_migration);

    // Response status tests
    RUN_TEST(test_resp_set_status_ok);
    RUN_TEST(test_resp_set_status_various_codes);
    RUN_TEST(test_resp_set_status_null_req);

    // Response header tests
    RUN_TEST(test_resp_set_header_basic);
    RUN_TEST(test_resp_set_header_null_req);
    RUN_TEST(test_resp_set_header_stages_until_send);
    RUN_TEST(test_resp_set_header_rejects_crlf_injection);
    RUN_TEST(test_resp_set_header_staging_overflow);
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
    RUN_TEST(test_resp_send_midsize_two_buffer_ordering);

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
