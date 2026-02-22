/**
 * @file test_router_api.c
 * @brief Unit tests for the Router API (Express-style routing)
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "radix_tree.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_ROUTER_API";

// Test handler that just returns OK
static httpd_err_t test_handler(httpd_req_t* req) {
    (void)req;
    return HTTPD_OK;
}

// Test middleware
static httpd_err_t test_middleware(httpd_req_t* req, httpd_next_t next) {
    (void)req;
    return next(req);
}

// WebSocket test handler
static httpd_err_t test_ws_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
    (void)ws;
    (void)event;
    return HTTPD_OK;
}

// ==================== Router Create/Destroy Tests ====================

static void test_router_create(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);
    httpd_router_destroy(router);
}

static void test_router_destroy_null(void) {
    // Should not crash
    httpd_router_destroy(NULL);
    TEST_PASS();
}

// ==================== Router HTTP Method Tests ====================

static void test_router_get(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_get(router, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_post(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_post(router, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_put(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_put(router, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_delete(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_delete(router, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_patch(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_patch(router, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_all_methods(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Register handler for all HTTP methods
    httpd_err_t err = httpd_router_all(router, "/universal", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

// ==================== Router NULL Argument Tests ====================

static void test_router_get_null_router(void) {
    httpd_err_t err = httpd_router_get(NULL, "/test", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_router_get_null_pattern(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_get(router, NULL, test_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

static void test_router_get_null_handler(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_get(router, "/test", NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

// ==================== Router Pattern Tests ====================

static void test_router_parameterized_route(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_get(router, "/users/:id", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_get(router, "/users/:userId/posts/:postId", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_wildcard_route(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_get(router, "/static/*", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_multiple_routes(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err;
    err = httpd_router_get(router, "/api/users", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_post(router, "/api/users", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_get(router, "/api/posts", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_put(router, "/api/users/:id", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_delete(router, "/api/users/:id", test_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

// ==================== Router Middleware Tests ====================

static void test_router_use_middleware(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_use(router, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_use_multiple_middleware(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err;
    err = httpd_router_use(router, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_use(router, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_use(router, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_use_null_router(void) {
    httpd_err_t err = httpd_router_use(NULL, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_router_use_null_middleware(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_use(router, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

// ==================== Router WebSocket Tests ====================

static void test_router_websocket(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_websocket(router, "/ws", test_ws_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_websocket_null_router(void) {
    httpd_err_t err = httpd_router_websocket(NULL, "/ws", test_ws_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_router_websocket_null_pattern(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_websocket(router, NULL, test_ws_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

static void test_router_websocket_null_handler(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_websocket(router, "/ws", NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

// ==================== Server Mount Tests ====================

static void test_httpd_mount_basic(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_router_get(router, "/status", test_handler);

    httpd_err_t err = httpd_mount(server, "/api/v1", router);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_stop(server);
}

static void test_httpd_mount_null_server(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_mount(NULL, "/api", router);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_router_destroy(router);
}

static void test_httpd_mount_null_router(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_err_t err = httpd_mount(server, "/api", NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_stop(server);
}

static void test_httpd_mount_multiple_routers(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_router_t router_v1 = httpd_router_create();
    httpd_router_t router_v2 = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router_v1);
    TEST_ASSERT_NOT_NULL(router_v2);

    httpd_router_get(router_v1, "/users", test_handler);
    httpd_router_get(router_v2, "/users", test_handler);

    httpd_err_t err;
    err = httpd_mount(server, "/api/v1", router_v1);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_mount(server, "/api/v2", router_v2);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_stop(server);
}

// ==================== Server-level Middleware Tests ====================

static void test_httpd_use_middleware(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_err_t err = httpd_use(server, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_stop(server);
}

static void test_httpd_use_null_server(void) {
    httpd_err_t err = httpd_use(NULL, test_middleware);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_httpd_use_null_middleware(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_err_t err = httpd_use(server, NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);

    httpd_stop(server);
}

// ==================== Error Handler Tests ====================

static httpd_err_t test_error_handler(httpd_err_t err, httpd_req_t* req) {
    (void)err;
    (void)req;
    return HTTPD_OK;
}

static void test_httpd_on_error(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    httpd_err_t err = httpd_on_error(server, test_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_stop(server);
}

static void test_httpd_on_error_null_server(void) {
    httpd_err_t err = httpd_on_error(NULL, test_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

static void test_router_on_error(void) {
    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_on_error(router, test_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_on_error_null_router(void) {
    httpd_err_t err = httpd_router_on_error(NULL, test_error_handler);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Test Runner ====================

void test_router_api_run(void) {
    ESP_LOGI(TAG, "Running Router API tests");

    // Create/Destroy tests
    RUN_TEST(test_router_create);
    RUN_TEST(test_router_destroy_null);

    // HTTP method tests
    RUN_TEST(test_router_get);
    RUN_TEST(test_router_post);
    RUN_TEST(test_router_put);
    RUN_TEST(test_router_delete);
    RUN_TEST(test_router_patch);
    RUN_TEST(test_router_all_methods);

    // NULL argument tests
    RUN_TEST(test_router_get_null_router);
    RUN_TEST(test_router_get_null_pattern);
    RUN_TEST(test_router_get_null_handler);

    // Pattern tests
    RUN_TEST(test_router_parameterized_route);
    RUN_TEST(test_router_wildcard_route);
    RUN_TEST(test_router_multiple_routes);

    // Middleware tests
    RUN_TEST(test_router_use_middleware);
    RUN_TEST(test_router_use_multiple_middleware);
    RUN_TEST(test_router_use_null_router);
    RUN_TEST(test_router_use_null_middleware);

    // WebSocket tests
    RUN_TEST(test_router_websocket);
    RUN_TEST(test_router_websocket_null_router);
    RUN_TEST(test_router_websocket_null_pattern);
    RUN_TEST(test_router_websocket_null_handler);

    // Mount tests
    RUN_TEST(test_httpd_mount_basic);
    RUN_TEST(test_httpd_mount_null_server);
    RUN_TEST(test_httpd_mount_null_router);
    RUN_TEST(test_httpd_mount_multiple_routers);

    // Server middleware tests
    RUN_TEST(test_httpd_use_middleware);
    RUN_TEST(test_httpd_use_null_server);
    RUN_TEST(test_httpd_use_null_middleware);

    // Error handler tests
    RUN_TEST(test_httpd_on_error);
    RUN_TEST(test_httpd_on_error_null_server);
    RUN_TEST(test_router_on_error);
    RUN_TEST(test_router_on_error_null_router);

    ESP_LOGI(TAG, "Router API tests completed");
}
