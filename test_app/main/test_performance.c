#include "unity.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

// Include internal headers for benchmarking
#include "esphttpd.h"
#include "http_parser.h"
#include "websocket.h"
#include "template.h"
#include "connection.h"
#include "radix_tree.h"

static const char* TAG = "TEST_PERF";

// ============================================================================
// Benchmark Macros
// ============================================================================

#define PERF_START() int64_t _perf_start = esp_timer_get_time()

#define PERF_END(name, iters) do { \
    int64_t _perf_elapsed = esp_timer_get_time() - _perf_start; \
    int64_t _ns_per_op = (_perf_elapsed * 1000) / (iters); \
    printf("PERF: %s: %lld us total, %lld ns/op (%d iterations)\n", \
           (name), _perf_elapsed, _ns_per_op, (iters)); \
    TEST_ASSERT_MESSAGE(_perf_elapsed > 0, "Timer returned 0 - benchmark invalid"); \
} while(0)

// Warm up the cache by running the operation once before timing
#define PERF_WARMUP(code) do { code; } while(0)

// ============================================================================
// Dummy Handlers
// ============================================================================

static httpd_err_t dummy_handler(httpd_req_t* req) {
    (void)req;
    return HTTPD_OK;
}

static int dummy_var_callback(const char* var_name, uint8_t* output,
                              size_t output_size, void* user_data) {
    (void)user_data;
    const char* value = NULL;
    if (strcmp(var_name, "title") == 0) value = "Hello World";
    else if (strcmp(var_name, "user") == 0) value = "TestUser";
    else if (strcmp(var_name, "count") == 0) value = "42";
    else return 0;

    size_t len = strlen(value);
    if (len > output_size) len = output_size;
    memcpy(output, value, len);
    return (int)len;
}

// ============================================================================
// HTTP Parser Benchmarks
// ============================================================================

static void perf_http_parse_method(void) {
    const int ITERS = 10000;

    PERF_WARMUP({
        http_parse_method((const uint8_t*)"GET", 3);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile http_method_t m;
        m = http_parse_method((const uint8_t*)"GET", 3);
        m = http_parse_method((const uint8_t*)"POST", 4);
        m = http_parse_method((const uint8_t*)"DELETE", 6);
        (void)m;
    }
    PERF_END("http_parse_method", ITERS * 3);
}

static void perf_http_identify_header(void) {
    const int ITERS = 10000;

    PERF_WARMUP({
        http_identify_header((const uint8_t*)"Content-Length", 14);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile header_type_t h;
        h = http_identify_header((const uint8_t*)"Content-Length", 14);
        h = http_identify_header((const uint8_t*)"Host", 4);
        h = http_identify_header((const uint8_t*)"Connection", 10);
        h = http_identify_header((const uint8_t*)"User-Agent", 10);
        (void)h;
    }
    PERF_END("http_identify_header", ITERS * 4);
}

static void perf_http_parse_content_length(void) {
    const int ITERS = 10000;

    PERF_WARMUP({
        http_parse_content_length((const uint8_t*)"12345", 5);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile uint16_t len;
        len = http_parse_content_length((const uint8_t*)"0", 1);
        len = http_parse_content_length((const uint8_t*)"123", 3);
        len = http_parse_content_length((const uint8_t*)"65535", 5);
        (void)len;
    }
    PERF_END("http_parse_content_length", ITERS * 3);
}

static void perf_http_parse_request(void) {
    const int ITERS = 1000;
    const char* request = "GET /api/users?id=123 HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: keep-alive\r\n"
                         "User-Agent: TestClient/1.0\r\n"
                         "\r\n";
    size_t req_len = strlen(request);

    connection_t conn;
    http_parser_context_t ctx;

    PERF_WARMUP({
        memset(&conn, 0, sizeof(conn));
        memset(&ctx, 0, sizeof(ctx));
        http_parse_request(&conn, (const uint8_t*)request, req_len, &ctx);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        memset(&conn, 0, sizeof(conn));
        memset(&ctx, 0, sizeof(ctx));
        http_parse_request(&conn, (const uint8_t*)request, req_len, &ctx);
    }
    PERF_END("http_parse_request", ITERS);
}

static void perf_http_parse_url_params(void) {
    const int ITERS = 10000;
    const uint8_t* url = (const uint8_t*)"/api/users?id=123&name=test";
    uint8_t url_len = 27;

    uint16_t path_len;
    const uint8_t* params;

    PERF_WARMUP({
        http_parse_url_params(url, url_len, &path_len, &params);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        http_parse_url_params(url, url_len, &path_len, &params);
    }
    PERF_END("http_parse_url_params", ITERS);
}

// ============================================================================
// Radix Tree Benchmarks
// ============================================================================

static void perf_radix_lookup_static(void) {
    const int ITERS = 10000;

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert several routes
    radix_insert(tree, "/api/users", HTTP_GET, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/api/posts", HTTP_GET, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/api/comments", HTTP_GET, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/api/auth/login", HTTP_POST, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/api/auth/logout", HTTP_POST, dummy_handler, NULL, NULL, 0);

    PERF_WARMUP({
        radix_match_t m = radix_lookup(tree, "/api/users", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        radix_match_t m = radix_lookup(tree, "/api/users", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    }
    PERF_END("radix_lookup_static", ITERS);

    radix_tree_destroy(tree);
}

static void perf_radix_lookup_param(void) {
    const int ITERS = 10000;

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_insert(tree, "/users/:id", HTTP_GET, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/users/:id/posts", HTTP_GET, dummy_handler, NULL, NULL, 0);

    PERF_WARMUP({
        radix_match_t m = radix_lookup(tree, "/users/12345", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        radix_match_t m = radix_lookup(tree, "/users/12345", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    }
    PERF_END("radix_lookup_param", ITERS);

    radix_tree_destroy(tree);
}

static void perf_radix_lookup_deep(void) {
    const int ITERS = 10000;

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_insert(tree, "/api/v1/users/profile/settings", HTTP_GET, dummy_handler, NULL, NULL, 0);

    PERF_WARMUP({
        radix_match_t m = radix_lookup(tree, "/api/v1/users/profile/settings", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        radix_match_t m = radix_lookup(tree, "/api/v1/users/profile/settings", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    }
    PERF_END("radix_lookup_deep", ITERS);

    radix_tree_destroy(tree);
}

static void perf_radix_lookup_miss(void) {
    const int ITERS = 10000;

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_insert(tree, "/api/users", HTTP_GET, dummy_handler, NULL, NULL, 0);
    radix_insert(tree, "/api/posts", HTTP_GET, dummy_handler, NULL, NULL, 0);

    PERF_WARMUP({
        radix_match_t m = radix_lookup(tree, "/api/nonexistent", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        radix_match_t m = radix_lookup(tree, "/api/nonexistent", HTTP_GET, false);
        if (m.middlewares) free(m.middlewares);
    }
    PERF_END("radix_lookup_miss", ITERS);

    radix_tree_destroy(tree);
}

static void perf_radix_insert(void) {
    const int ITERS = 1000;

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        radix_tree_t* tree = radix_tree_create();
        radix_insert(tree, "/api/users", HTTP_GET, dummy_handler, NULL, NULL, 0);
        radix_insert(tree, "/api/posts", HTTP_GET, dummy_handler, NULL, NULL, 0);
        radix_insert(tree, "/users/:id", HTTP_GET, dummy_handler, NULL, NULL, 0);
        radix_tree_destroy(tree);
    }
    PERF_END("radix_insert (3 routes)", ITERS);
}

// ============================================================================
// WebSocket Frame Benchmarks
// ============================================================================

static void perf_ws_mask_payload(void) {
    const int ITERS = 10000;
    uint8_t payload[128];
    uint32_t mask_key = 0x12345678;

    memset(payload, 'A', sizeof(payload));

    PERF_WARMUP({
        ws_mask_payload(payload, sizeof(payload), mask_key, 0);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        ws_mask_payload(payload, sizeof(payload), mask_key, 0);
    }
    PERF_END("ws_mask_payload_128", ITERS);
}

static void perf_ws_build_frame_header(void) {
    const int ITERS = 10000;
    uint8_t header[14];

    PERF_WARMUP({
        ws_build_frame_header(header, WS_OPCODE_TEXT, 100, false);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        ws_build_frame_header(header, WS_OPCODE_TEXT, 100, false);
        ws_build_frame_header(header, WS_OPCODE_TEXT, 1000, false);
        ws_build_frame_header(header, WS_OPCODE_BINARY, 50000, false);
    }
    PERF_END("ws_build_frame_header", ITERS * 3);
}

static void perf_ws_process_frame(void) {
    const int ITERS = 5000;

    // Build a masked text frame with 64 bytes of payload
    uint8_t frame[128];
    frame[0] = 0x81;  // FIN + TEXT opcode
    frame[1] = 0xC0 | 64;  // Masked + 64 byte payload
    frame[2] = 0x12;  // Mask key byte 0
    frame[3] = 0x34;  // Mask key byte 1
    frame[4] = 0x56;  // Mask key byte 2
    frame[5] = 0x78;  // Mask key byte 3

    // Fill payload (masked)
    for (int i = 0; i < 64; i++) {
        frame[6 + i] = 'A' ^ frame[2 + (i % 4)];
    }

    connection_t conn;
    ws_frame_context_t ctx;
    size_t consumed;

    memset(&ctx, 0, sizeof(ctx));
    ws_frame_ctx_init(&ctx);

    PERF_WARMUP({
        memset(&conn, 0, sizeof(conn));
        ctx.state = WS_STATE_OPCODE;
        ctx.payload_received = 0;
        ws_process_frame(&conn, frame, 70, &ctx, &consumed);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        memset(&conn, 0, sizeof(conn));
        ctx.state = WS_STATE_OPCODE;
        ctx.payload_received = 0;
        ws_process_frame(&conn, frame, 70, &ctx, &consumed);
    }
    PERF_END("ws_process_frame_64", ITERS);

    // Cleanup
    if (ctx.payload_buffer) {
        free(ctx.payload_buffer);
    }
}

// ============================================================================
// Template Engine Benchmarks
// ============================================================================

static void perf_template_plain(void) {
    const int ITERS = 5000;
    const char* input = "Hello, this is plain text without any variables!";
    uint8_t output[128];

    template_context_t ctx;
    template_config_t config = {
        .start_delim = "{{",
        .end_delim = "}}",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = false
    };

    PERF_WARMUP({
        template_init(&ctx, &config, dummy_var_callback, NULL);
        template_process(&ctx, (const uint8_t*)input, strlen(input), output, sizeof(output));
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        template_init(&ctx, &config, dummy_var_callback, NULL);
        template_process(&ctx, (const uint8_t*)input, strlen(input), output, sizeof(output));
    }
    PERF_END("template_process_plain", ITERS);
}

static void perf_template_vars(void) {
    const int ITERS = 5000;
    const char* input = "Title: {{title}}, User: {{user}}, Count: {{count}}";
    uint8_t output[256];

    template_context_t ctx;
    template_config_t config = {
        .start_delim = "{{",
        .end_delim = "}}",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = false
    };

    PERF_WARMUP({
        template_init(&ctx, &config, dummy_var_callback, NULL);
        template_process(&ctx, (const uint8_t*)input, strlen(input), output, sizeof(output));
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        template_init(&ctx, &config, dummy_var_callback, NULL);
        template_process(&ctx, (const uint8_t*)input, strlen(input), output, sizeof(output));
    }
    PERF_END("template_process_3vars", ITERS);
}

// ============================================================================
// Connection Pool Benchmarks
// ============================================================================

static void perf_connection_find(void) {
    const int ITERS = 10000;

    connection_pool_t pool;
    connection_pool_init(&pool);

    // Fill pool with connections
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        pool.connections[i].fd = 100 + i;
        connection_mark_active(&pool, i);
    }

    PERF_WARMUP({
        connection_find(&pool, 115);  // Middle of pool
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile connection_t* c;
        c = connection_find(&pool, 100);  // First
        c = connection_find(&pool, 115);  // Middle
        c = connection_find(&pool, 100 + MAX_CONNECTIONS - 1);  // Last
        (void)c;
    }
    PERF_END("connection_find", ITERS * 3);
}

static void perf_connection_get_index(void) {
    const int ITERS = 10000;

    connection_pool_t pool;
    connection_pool_init(&pool);

    connection_t* conn = &pool.connections[15];

    PERF_WARMUP({
        connection_get_index(&pool, conn);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile int idx;
        idx = connection_get_index(&pool, &pool.connections[0]);
        idx = connection_get_index(&pool, &pool.connections[15]);
        idx = connection_get_index(&pool, &pool.connections[MAX_CONNECTIONS - 1]);
        (void)idx;
    }
    PERF_END("connection_get_index", ITERS * 3);
}

static void perf_connection_count_active(void) {
    const int ITERS = 10000;

    connection_pool_t pool;
    connection_pool_init(&pool);

    // Mark half as active
    for (int i = 0; i < MAX_CONNECTIONS / 2; i++) {
        connection_mark_active(&pool, i);
    }

    PERF_WARMUP({
        connection_count_active(&pool);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile int count = connection_count_active(&pool);
        (void)count;
    }
    PERF_END("connection_count_active", ITERS);
}

// ============================================================================
// Utility Function Benchmarks
// ============================================================================

static void perf_httpd_get_mime_type(void) {
    const int ITERS = 10000;

    PERF_WARMUP({
        httpd_get_mime_type(".html");
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile const char* m;
        m = httpd_get_mime_type(".html");
        m = httpd_get_mime_type(".js");
        m = httpd_get_mime_type(".css");
        m = httpd_get_mime_type(".json");
        (void)m;
    }
    PERF_END("httpd_get_mime_type", ITERS * 4);
}

static void perf_httpd_status_text(void) {
    const int ITERS = 10000;

    PERF_WARMUP({
        httpd_status_text(200);
    });

    PERF_START();
    for (int i = 0; i < ITERS; i++) {
        volatile const char* s;
        s = httpd_status_text(200);
        s = httpd_status_text(404);
        s = httpd_status_text(500);
        s = httpd_status_text(301);
        (void)s;
    }
    PERF_END("httpd_status_text", ITERS * 4);
}

// ============================================================================
// Test Runner
// ============================================================================

void test_performance_run(void) {
    ESP_LOGI(TAG, "=== Performance Benchmarks ===");
    ESP_LOGI(TAG, "Note: Running under QEMU - timing may differ from real hardware");

    // HTTP Parser benchmarks
    ESP_LOGI(TAG, "--- HTTP Parser ---");
    RUN_TEST(perf_http_parse_method);
    RUN_TEST(perf_http_identify_header);
    RUN_TEST(perf_http_parse_content_length);
    RUN_TEST(perf_http_parse_request);
    RUN_TEST(perf_http_parse_url_params);

    // Radix Tree benchmarks
    ESP_LOGI(TAG, "--- Radix Tree Router ---");
    RUN_TEST(perf_radix_lookup_static);
    RUN_TEST(perf_radix_lookup_param);
    RUN_TEST(perf_radix_lookup_deep);
    RUN_TEST(perf_radix_lookup_miss);
    RUN_TEST(perf_radix_insert);

    // WebSocket benchmarks
    ESP_LOGI(TAG, "--- WebSocket Frame ---");
    RUN_TEST(perf_ws_mask_payload);
    RUN_TEST(perf_ws_build_frame_header);
    RUN_TEST(perf_ws_process_frame);

    // Template benchmarks
    ESP_LOGI(TAG, "--- Template Engine ---");
    RUN_TEST(perf_template_plain);
    RUN_TEST(perf_template_vars);

    // Connection Pool benchmarks
    ESP_LOGI(TAG, "--- Connection Pool ---");
    RUN_TEST(perf_connection_find);
    RUN_TEST(perf_connection_get_index);
    RUN_TEST(perf_connection_count_active);

    // Utility benchmarks
    ESP_LOGI(TAG, "--- Utility Functions ---");
    RUN_TEST(perf_httpd_get_mime_type);
    RUN_TEST(perf_httpd_status_text);

    ESP_LOGI(TAG, "=== Performance Benchmarks Complete ===");
}
