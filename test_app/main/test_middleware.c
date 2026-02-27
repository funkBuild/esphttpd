/**
 * @file test_middleware.c
 * @brief Unit tests for middleware chain execution
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "test_exports.h"
#include "radix_tree.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "TEST_MIDDLEWARE";

// ==================== Execution Tracking ====================

#define MAX_EXEC_TRACK 16

// Track middleware/handler execution order
static int exec_order[MAX_EXEC_TRACK];
static int exec_count = 0;

static void reset_exec_tracking(void) {
    memset(exec_order, 0, sizeof(exec_order));
    exec_count = 0;
}

static void record_execution(int id) {
    if (exec_count < MAX_EXEC_TRACK) {
        exec_order[exec_count++] = id;
    }
}

// ==================== Test Middleware Functions ====================

// Middleware that calls next and records execution
static httpd_err_t middleware_1(httpd_req_t* req, httpd_next_t next) {
    record_execution(1);
    return next(req);
}

static httpd_err_t middleware_2(httpd_req_t* req, httpd_next_t next) {
    record_execution(2);
    return next(req);
}

static httpd_err_t middleware_3(httpd_req_t* req, httpd_next_t next) {
    record_execution(3);
    return next(req);
}

// Middleware that does NOT call next (stops the chain)
static httpd_err_t middleware_stop(httpd_req_t* req, httpd_next_t next) {
    (void)next;
    record_execution(100);
    return HTTPD_OK;  // Returns OK but doesn't call next
}

// Middleware that returns an error
static httpd_err_t middleware_error(httpd_req_t* req, httpd_next_t next) {
    (void)next;
    record_execution(200);
    return HTTPD_ERR_MIDDLEWARE;
}

// Middleware that modifies request user_data
static httpd_err_t middleware_modify(httpd_req_t* req, httpd_next_t next) {
    record_execution(10);
    // Store a value in user_data to show modification
    req->user_data = (void*)0x12345678;
    return next(req);
}

// Handler that records execution
static httpd_err_t test_handler_record(httpd_req_t* req) {
    (void)req;
    record_execution(999);  // Final handler marker
    return HTTPD_OK;
}

// Handler that just records execution (for modify test)
static httpd_err_t test_handler_simple(httpd_req_t* req) {
    (void)req;
    record_execution(999);
    return HTTPD_OK;
}

// ==================== Helper Functions ====================

static void setup_mock_req(httpd_req_t* req, connection_t* conn) {
    memset(req, 0, sizeof(*req));
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->state = CONN_STATE_HTTP_BODY;
    req->fd = conn->fd;
    req->_internal = conn;
    req->method = HTTP_GET;
}

// ==================== Single Middleware Tests ====================

static void test_middleware_single_calls_next(void) {
    ESP_LOGI(TAG, "Test: Single middleware calls next");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Set up middleware chain with single middleware
    httpd_middleware_t chain[1] = { middleware_1 };
    req._mw.chain = chain;
    req._mw.chain_len = 1;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    // Execute the chain by calling _middleware_next_test (which starts the chain)
    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(2, exec_count);  // middleware_1 + handler
    TEST_ASSERT_EQUAL(1, exec_order[0]);  // middleware_1
    TEST_ASSERT_EQUAL(999, exec_order[1]);  // handler
}

static void test_middleware_single_stops_chain(void) {
    ESP_LOGI(TAG, "Test: Single middleware stops chain");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Set up middleware chain with stopping middleware
    httpd_middleware_t chain[1] = { middleware_stop };
    req._mw.chain = chain;
    req._mw.chain_len = 1;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(1, exec_count);  // Only middleware_stop
    TEST_ASSERT_EQUAL(100, exec_order[0]);  // middleware_stop marker
}

static void test_middleware_single_returns_error(void) {
    ESP_LOGI(TAG, "Test: Single middleware returns error");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    httpd_middleware_t chain[1] = { middleware_error };
    req._mw.chain = chain;
    req._mw.chain_len = 1;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_ERR_MIDDLEWARE, err);
    TEST_ASSERT_EQUAL(1, exec_count);  // Only middleware_error
    TEST_ASSERT_EQUAL(200, exec_order[0]);  // middleware_error marker
}

// ==================== Multiple Middleware Tests ====================

static void test_middleware_chain_order(void) {
    ESP_LOGI(TAG, "Test: Middleware chain execution order");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Chain: mw1 -> mw2 -> mw3 -> handler
    httpd_middleware_t chain[3] = { middleware_1, middleware_2, middleware_3 };
    req._mw.chain = chain;
    req._mw.chain_len = 3;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    // Start by calling _middleware_next_test which will run the chain
    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(4, exec_count);  // 3 middleware + handler
    TEST_ASSERT_EQUAL(1, exec_order[0]);  // middleware_1
    TEST_ASSERT_EQUAL(2, exec_order[1]);  // middleware_2
    TEST_ASSERT_EQUAL(3, exec_order[2]);  // middleware_3
    TEST_ASSERT_EQUAL(999, exec_order[3]);  // handler
}

static void test_middleware_chain_stop_middle(void) {
    ESP_LOGI(TAG, "Test: Middleware chain stops in middle");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Chain: mw1 -> stop -> mw3 -> handler
    // mw3 and handler should NOT execute
    httpd_middleware_t chain[3] = { middleware_1, middleware_stop, middleware_3 };
    req._mw.chain = chain;
    req._mw.chain_len = 3;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(2, exec_count);  // mw1 + stop
    TEST_ASSERT_EQUAL(1, exec_order[0]);  // middleware_1
    TEST_ASSERT_EQUAL(100, exec_order[1]);  // middleware_stop
    // mw3 and handler should not have executed
}

static void test_middleware_chain_error_propagates(void) {
    ESP_LOGI(TAG, "Test: Middleware error propagates through chain");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Chain: mw1 -> error -> mw3 -> handler
    httpd_middleware_t chain[3] = { middleware_1, middleware_error, middleware_3 };
    req._mw.chain = chain;
    req._mw.chain_len = 3;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_ERR_MIDDLEWARE, err);
    TEST_ASSERT_EQUAL(2, exec_count);  // mw1 + error
    TEST_ASSERT_EQUAL(1, exec_order[0]);
    TEST_ASSERT_EQUAL(200, exec_order[1]);
}

// ==================== Empty Chain Tests ====================

static void test_middleware_empty_chain(void) {
    ESP_LOGI(TAG, "Test: Empty middleware chain calls handler directly");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    // Empty chain
    req._mw.chain = NULL;
    req._mw.chain_len = 0;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_record;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(1, exec_count);  // Only handler
    TEST_ASSERT_EQUAL(999, exec_order[0]);  // handler
}

static void test_middleware_no_handler(void) {
    ESP_LOGI(TAG, "Test: Empty chain with no handler");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);

    req._mw.chain = NULL;
    req._mw.chain_len = 0;
    req._mw.current = 0;
    req._mw.final_handler = NULL;  // No handler
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    // Should return OK when chain is exhausted and no handler
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(0, exec_count);  // Nothing executed
}

// ==================== Request Modification Tests ====================

static void test_middleware_modifies_request(void) {
    ESP_LOGI(TAG, "Test: Middleware modifies request");
    reset_exec_tracking();

    httpd_req_t req;
    connection_t conn;
    setup_mock_req(&req, &conn);
    req.user_data = NULL;  // Initial value

    httpd_middleware_t chain[1] = { middleware_modify };
    req._mw.chain = chain;
    req._mw.chain_len = 1;
    req._mw.current = 0;
    req._mw.final_handler = test_handler_simple;
    req._mw.final_user_ctx = NULL;
    req._mw.router = NULL;

    httpd_err_t err = _middleware_next_test(&req);

    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(2, exec_count);  // modify + handler
    TEST_ASSERT_EQUAL(10, exec_order[0]);  // middleware_modify
    TEST_ASSERT_EQUAL(999, exec_order[1]);  // handler
    // Note: user_data gets overwritten by final_user_ctx in _middleware_next
    // so we can't verify middleware modifications to user_data survive to handler
}

// ==================== NULL Input Tests ====================

static void test_middleware_next_null_req(void) {
    ESP_LOGI(TAG, "Test: _middleware_next with NULL request");

    httpd_err_t err = _middleware_next_test(NULL);
    TEST_ASSERT_EQUAL(HTTPD_ERR_INVALID_ARG, err);
}

// ==================== Server-Level Middleware Tests ====================

static void test_server_middleware_registration(void) {
    ESP_LOGI(TAG, "Test: Server middleware registration");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_err_t err = httpd_start(&server, &config);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_NOT_NULL(server);

    // Register multiple middleware
    err = httpd_use(server, middleware_1);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_use(server, middleware_2);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_use(server, middleware_3);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_stop(server);
}

static void test_server_middleware_limit(void) {
    ESP_LOGI(TAG, "Test: Server middleware limit");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    httpd_start(&server, &config);
    TEST_ASSERT_NOT_NULL(server);

    // Fill up middleware slots
    httpd_err_t err;
    for (int i = 0; i < MAX_MIDDLEWARES; i++) {
        err = httpd_use(server, middleware_1);
        TEST_ASSERT_EQUAL(HTTPD_OK, err);
    }

    // Next should fail
    err = httpd_use(server, middleware_1);
    TEST_ASSERT_EQUAL(HTTPD_ERR_NO_MEM, err);

    httpd_stop(server);
}

// ==================== Router-Level Middleware Tests ====================

static void test_router_middleware_registration(void) {
    ESP_LOGI(TAG, "Test: Router middleware registration");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_err_t err = httpd_router_use(router, middleware_1);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    err = httpd_router_use(router, middleware_2);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    httpd_router_destroy(router);
}

static void test_router_middleware_limit(void) {
    ESP_LOGI(TAG, "Test: Router middleware limit");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Fill up router middleware slots
    httpd_err_t err;
    for (int i = 0; i < CONFIG_HTTPD_MAX_MIDDLEWARE_PER_ROUTER; i++) {
        err = httpd_router_use(router, middleware_1);
        TEST_ASSERT_EQUAL(HTTPD_OK, err);
    }

    // Next should fail
    err = httpd_router_use(router, middleware_1);
    TEST_ASSERT_EQUAL(HTTPD_ERR_NO_MEM, err);

    httpd_router_destroy(router);
}

// ==================== Route-Level Middleware via Radix Tree ====================

static httpd_err_t route_handler(httpd_req_t* req) {
    (void)req;
    return HTTPD_OK;
}

static void test_radix_route_with_middleware(void) {
    ESP_LOGI(TAG, "Test: Radix route with middleware");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    httpd_middleware_t route_mw[2] = { middleware_1, middleware_2 };

    httpd_err_t err = radix_insert(tree, "/api/test", HTTP_GET, route_handler,
                                    NULL, route_mw, 2);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Look up the route and verify middleware is attached
    radix_match_t match;
    httpd_middleware_t mw_buf[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];
    uint8_t mw_count = 0;
    radix_lookup(tree, "/api/test", HTTP_GET, false, &match, mw_buf, &mw_count);
    TEST_ASSERT_TRUE(match.matched);
    TEST_ASSERT_EQUAL(2, mw_count);
    TEST_ASSERT_EQUAL_PTR(middleware_1, mw_buf[0]);
    TEST_ASSERT_EQUAL_PTR(middleware_2, mw_buf[1]);

    radix_tree_destroy(tree);
}

static void test_radix_route_middleware_collected_during_traversal(void) {
    ESP_LOGI(TAG, "Test: Middleware collected during radix tree traversal");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert routes at different levels
    httpd_middleware_t mw1[1] = { middleware_1 };
    httpd_middleware_t mw2[1] = { middleware_2 };

    // Route with middleware
    radix_insert(tree, "/api/users/:id", HTTP_GET, route_handler, NULL, mw1, 1);

    // Another route at different path
    radix_insert(tree, "/api/posts", HTTP_GET, route_handler, NULL, mw2, 1);

    // Look up first route
    radix_match_t match;
    httpd_middleware_t mw_buf[CONFIG_HTTPD_MAX_TOTAL_MIDDLEWARE];
    uint8_t mw_count = 0;
    radix_lookup(tree, "/api/users/123", HTTP_GET, false, &match, mw_buf, &mw_count);
    TEST_ASSERT_TRUE(match.matched);
    TEST_ASSERT_EQUAL(1, mw_count);

    // Look up second route
    mw_count = 0;
    radix_lookup(tree, "/api/posts", HTTP_GET, false, &match, mw_buf, &mw_count);
    TEST_ASSERT_TRUE(match.matched);
    TEST_ASSERT_EQUAL(1, mw_count);

    radix_tree_destroy(tree);
}

// ==================== Test Runner ====================

void test_middleware_run(void) {
    ESP_LOGI(TAG, "Running Middleware tests");

    // Single middleware tests
    RUN_TEST(test_middleware_single_calls_next);
    RUN_TEST(test_middleware_single_stops_chain);
    RUN_TEST(test_middleware_single_returns_error);

    // Multiple middleware tests
    RUN_TEST(test_middleware_chain_order);
    RUN_TEST(test_middleware_chain_stop_middle);
    RUN_TEST(test_middleware_chain_error_propagates);

    // Empty chain tests
    RUN_TEST(test_middleware_empty_chain);
    RUN_TEST(test_middleware_no_handler);

    // Request modification tests
    RUN_TEST(test_middleware_modifies_request);

    // NULL input tests
    RUN_TEST(test_middleware_next_null_req);

    // Server-level middleware tests
    RUN_TEST(test_server_middleware_registration);
    RUN_TEST(test_server_middleware_limit);

    // Router-level middleware tests
    RUN_TEST(test_router_middleware_registration);
    RUN_TEST(test_router_middleware_limit);

    // Radix tree route-level middleware tests
    RUN_TEST(test_radix_route_with_middleware);
    RUN_TEST(test_radix_route_middleware_collected_during_traversal);

    ESP_LOGI(TAG, "Middleware tests completed");
}
