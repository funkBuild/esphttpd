#include <string.h>
#include "unity.h"
#include "esp_log.h"
#include "esphttpd.h"
#include "radix_tree.h"

static const char* TAG = "TEST_RADIX_TREE";

// ============================================================================
// Test Handlers
// ============================================================================

static int handler_call_count = 0;
static void* handler_user_ctx = NULL;

static httpd_err_t test_handler_1(httpd_req_t* req) {
    handler_call_count++;
    handler_user_ctx = req->user_data;
    return HTTPD_OK;
}

static httpd_err_t test_handler_2(httpd_req_t* req) {
    handler_call_count += 10;
    handler_user_ctx = req->user_data;
    return HTTPD_OK;
}

static httpd_err_t test_handler_3(httpd_req_t* req) {
    handler_call_count += 100;
    handler_user_ctx = req->user_data;
    return HTTPD_OK;
}

static void reset_handler_state(void) {
    handler_call_count = 0;
    handler_user_ctx = NULL;
}

// ============================================================================
// Phase 1: Basic Tree Operations
// ============================================================================

static void test_radix_tree_create_destroy(void) {
    ESP_LOGI(TAG, "Test: Create and destroy radix tree");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);
    TEST_ASSERT_NOT_NULL(tree->root);
    TEST_ASSERT_EQUAL(1, tree->node_count);
    TEST_ASSERT_EQUAL(0, tree->route_count);

    radix_tree_destroy(tree);
}

static void test_radix_insert_single_static_route(void) {
    ESP_LOGI(TAG, "Test: Insert single static route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert a simple route
    httpd_err_t err = radix_insert(tree, "/api/users", HTTP_GET,
                                   test_handler_1, (void*)0x1234, NULL, 0);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);
    TEST_ASSERT_EQUAL(1, tree->route_count);
    TEST_ASSERT(tree->node_count > 1); // Root + at least one child

    radix_tree_destroy(tree);
}

static void test_radix_lookup_static_route(void) {
    ESP_LOGI(TAG, "Test: Lookup static route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert route
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)0x1234, NULL, 0);

    // Lookup exact match
    radix_match_t match = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(match.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, match.handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x1234, match.user_ctx);
    TEST_ASSERT_FALSE(match.is_websocket);
    TEST_ASSERT_EQUAL(0, match.param_count);

    // Cleanup middleware if allocated
    if (match.middlewares) {
        free(match.middlewares);
    }

    // Lookup non-existent path
    radix_match_t no_match = radix_lookup(tree, "/api/posts", HTTP_GET, false);
    TEST_ASSERT_FALSE(no_match.matched);

    // Lookup wrong method
    radix_match_t wrong_method = radix_lookup(tree, "/api/users", HTTP_POST, false);
    TEST_ASSERT_FALSE(wrong_method.matched);

    radix_tree_destroy(tree);
}

static void test_radix_multiple_routes(void) {
    ESP_LOGI(TAG, "Test: Multiple routes with shared prefix");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert multiple routes with shared prefix
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    radix_insert(tree, "/api/users", HTTP_POST, test_handler_2, (void*)2, NULL, 0);
    radix_insert(tree, "/api/posts", HTTP_GET, test_handler_3, (void*)3, NULL, 0);

    TEST_ASSERT_EQUAL(3, tree->route_count);

    // Lookup each route
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m1.handler);
    TEST_ASSERT_EQUAL_PTR((void*)1, m1.user_ctx);
    if (m1.middlewares) free(m1.middlewares);

    radix_match_t m2 = radix_lookup(tree, "/api/users", HTTP_POST, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m2.handler);
    TEST_ASSERT_EQUAL_PTR((void*)2, m2.user_ctx);
    if (m2.middlewares) free(m2.middlewares);

    radix_match_t m3 = radix_lookup(tree, "/api/posts", HTTP_GET, false);
    TEST_ASSERT_TRUE(m3.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_3, m3.handler);
    TEST_ASSERT_EQUAL_PTR((void*)3, m3.user_ctx);
    if (m3.middlewares) free(m3.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_nested_routes(void) {
    ESP_LOGI(TAG, "Test: Nested routes");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert nested routes
    radix_insert(tree, "/api", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_2, (void*)2, NULL, 0);
    radix_insert(tree, "/api/users/active", HTTP_GET, test_handler_3, (void*)3, NULL, 0);

    // Lookup each level
    radix_match_t m1 = radix_lookup(tree, "/api", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m1.handler);
    if (m1.middlewares) free(m1.middlewares);

    radix_match_t m2 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m2.handler);
    if (m2.middlewares) free(m2.middlewares);

    radix_match_t m3 = radix_lookup(tree, "/api/users/active", HTTP_GET, false);
    TEST_ASSERT_TRUE(m3.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_3, m3.handler);
    if (m3.middlewares) free(m3.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_root_route(void) {
    ESP_LOGI(TAG, "Test: Root route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert root route
    radix_insert(tree, "/", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Lookup root
    radix_match_t m = radix_lookup(tree, "/", HTTP_GET, false);
    TEST_ASSERT_TRUE(m.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m.handler);
    if (m.middlewares) free(m.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_websocket_route(void) {
    ESP_LOGI(TAG, "Test: WebSocket route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert WebSocket route
    httpd_err_t err = radix_insert_ws(tree, "/ws", (httpd_ws_handler_t)test_handler_1,
                                      (void*)0x5678, 30000, NULL, 0);
    TEST_ASSERT_EQUAL(HTTPD_OK, err);

    // Lookup as WebSocket
    radix_match_t ws_match = radix_lookup(tree, "/ws", HTTP_GET, true);
    TEST_ASSERT_TRUE(ws_match.matched);
    TEST_ASSERT_TRUE(ws_match.is_websocket);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, ws_match.ws_handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x5678, ws_match.ws_user_ctx);
    if (ws_match.middlewares) free(ws_match.middlewares);

    // Lookup as HTTP (should not match)
    radix_match_t http_match = radix_lookup(tree, "/ws", HTTP_GET, false);
    TEST_ASSERT_FALSE(http_match.matched);

    radix_tree_destroy(tree);
}

static void test_radix_all_http_methods(void) {
    ESP_LOGI(TAG, "Test: All HTTP methods");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert routes for different methods on same path
    radix_insert(tree, "/api/resource", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_POST, test_handler_1, (void*)2, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_PUT, test_handler_1, (void*)3, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_DELETE, test_handler_1, (void*)4, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_PATCH, test_handler_1, (void*)5, NULL, 0);

    // Test each method
    radix_match_t m_get = radix_lookup(tree, "/api/resource", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_get.matched);
    TEST_ASSERT_EQUAL_PTR((void*)1, m_get.user_ctx);
    if (m_get.middlewares) free(m_get.middlewares);

    radix_match_t m_post = radix_lookup(tree, "/api/resource", HTTP_POST, false);
    TEST_ASSERT_TRUE(m_post.matched);
    TEST_ASSERT_EQUAL_PTR((void*)2, m_post.user_ctx);
    if (m_post.middlewares) free(m_post.middlewares);

    radix_match_t m_put = radix_lookup(tree, "/api/resource", HTTP_PUT, false);
    TEST_ASSERT_TRUE(m_put.matched);
    TEST_ASSERT_EQUAL_PTR((void*)3, m_put.user_ctx);
    if (m_put.middlewares) free(m_put.middlewares);

    radix_match_t m_delete = radix_lookup(tree, "/api/resource", HTTP_DELETE, false);
    TEST_ASSERT_TRUE(m_delete.matched);
    TEST_ASSERT_EQUAL_PTR((void*)4, m_delete.user_ctx);
    if (m_delete.middlewares) free(m_delete.middlewares);

    radix_match_t m_patch = radix_lookup(tree, "/api/resource", HTTP_PATCH, false);
    TEST_ASSERT_TRUE(m_patch.matched);
    TEST_ASSERT_EQUAL_PTR((void*)5, m_patch.user_ctx);
    if (m_patch.middlewares) free(m_patch.middlewares);

    // Test non-registered method
    radix_match_t m_head = radix_lookup(tree, "/api/resource", HTTP_HEAD, false);
    TEST_ASSERT_FALSE(m_head.matched);

    radix_tree_destroy(tree);
}

// ============================================================================
// Phase 2: Parameters and Wildcards
// ============================================================================

static void test_radix_param_route(void) {
    ESP_LOGI(TAG, "Test: Parameterized route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert route with parameter
    radix_insert(tree, "/users/:id", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Lookup with parameter value
    radix_match_t m = radix_lookup(tree, "/users/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(m.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m.handler);
    TEST_ASSERT_EQUAL(1, m.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", m.params[0].key, m.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", m.params[0].value, m.params[0].value_len);
    if (m.middlewares) free(m.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_multiple_params(void) {
    ESP_LOGI(TAG, "Test: Multiple parameters");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert route with multiple parameters
    radix_insert(tree, "/users/:userId/posts/:postId", HTTP_GET,
                test_handler_1, (void*)1, NULL, 0);

    // Lookup with parameter values
    radix_match_t m = radix_lookup(tree, "/users/42/posts/99", HTTP_GET, false);
    TEST_ASSERT_TRUE(m.matched);
    TEST_ASSERT_EQUAL(2, m.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("userId", m.params[0].key, m.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", m.params[0].value, m.params[0].value_len);
    TEST_ASSERT_EQUAL_STRING_LEN("postId", m.params[1].key, m.params[1].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("99", m.params[1].value, m.params[1].value_len);
    if (m.middlewares) free(m.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_param_priority(void) {
    ESP_LOGI(TAG, "Test: Static routes have priority over params");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert both static and param routes
    radix_insert(tree, "/users/:id", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    radix_insert(tree, "/users/me", HTTP_GET, test_handler_2, (void*)2, NULL, 0);

    // Static should match first
    radix_match_t m_static = radix_lookup(tree, "/users/me", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_static.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m_static.handler);
    TEST_ASSERT_EQUAL(0, m_static.param_count); // No parameters extracted
    if (m_static.middlewares) free(m_static.middlewares);

    // Param should match anything else
    radix_match_t m_param = radix_lookup(tree, "/users/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_param.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m_param.handler);
    TEST_ASSERT_EQUAL(1, m_param.param_count);
    if (m_param.middlewares) free(m_param.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_wildcard_route(void) {
    ESP_LOGI(TAG, "Test: Wildcard route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert wildcard route
    radix_insert(tree, "/static/*", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Lookup paths that should match wildcard
    radix_match_t m1 = radix_lookup(tree, "/static/css/style.css", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL(1, m1.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("*", m1.params[0].key, m1.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("css/style.css", m1.params[0].value,
                                  m1.params[0].value_len);
    if (m1.middlewares) free(m1.middlewares);

    radix_match_t m2 = radix_lookup(tree, "/static/index.html", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    if (m2.middlewares) free(m2.middlewares);

    radix_tree_destroy(tree);
}

static void test_radix_wildcard_priority(void) {
    ESP_LOGI(TAG, "Test: Static and param have priority over wildcard");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert routes with different specificity
    radix_insert(tree, "/files/*", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    radix_insert(tree, "/files/:id", HTTP_GET, test_handler_2, (void*)2, NULL, 0);
    radix_insert(tree, "/files/readme", HTTP_GET, test_handler_3, (void*)3, NULL, 0);

    // Static should match first
    radix_match_t m_static = radix_lookup(tree, "/files/readme", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_static.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_3, m_static.handler);
    if (m_static.middlewares) free(m_static.middlewares);

    // Param should match next
    radix_match_t m_param = radix_lookup(tree, "/files/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_param.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m_param.handler);
    TEST_ASSERT_EQUAL(1, m_param.param_count);
    if (m_param.middlewares) free(m_param.middlewares);

    // Wildcard should match everything else
    radix_match_t m_wild = radix_lookup(tree, "/files/path/to/file", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_wild.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m_wild.handler);
    if (m_wild.middlewares) free(m_wild.middlewares);

    radix_tree_destroy(tree);
}

// ============================================================================
// Phase 3: Router API
// ============================================================================

static void test_router_create_destroy(void) {
    ESP_LOGI(TAG, "Test: Create and destroy router");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    httpd_router_destroy(router);
}

static void test_router_convenience_functions(void) {
    ESP_LOGI(TAG, "Test: Router convenience functions");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Register routes using convenience functions
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_get(router, "/get", test_handler_1));
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_post(router, "/post", test_handler_1));
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_put(router, "/put", test_handler_1));
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_delete(router, "/delete", test_handler_1));
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_patch(router, "/patch", test_handler_1));

    // Test that routes are registered
    radix_match_t m_get = radix_lookup(router->tree, "/get", HTTP_GET, false);
    TEST_ASSERT_TRUE(m_get.matched);
    if (m_get.middlewares) free(m_get.middlewares);

    radix_match_t m_post = radix_lookup(router->tree, "/post", HTTP_POST, false);
    TEST_ASSERT_TRUE(m_post.matched);
    if (m_post.middlewares) free(m_post.middlewares);

    httpd_router_destroy(router);
}

static void test_router_all_method(void) {
    ESP_LOGI(TAG, "Test: Router ALL method");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Register route for all methods
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_all(router, "/all", test_handler_1));

    // Test that all methods match
    for (int method = HTTP_GET; method <= HTTP_PATCH; method++) {
        radix_match_t m = radix_lookup(router->tree, "/all", (http_method_t)method, false);
        TEST_ASSERT_TRUE(m.matched);
        if (m.middlewares) free(m.middlewares);
    }

    httpd_router_destroy(router);
}

static void test_router_websocket(void) {
    ESP_LOGI(TAG, "Test: Router WebSocket route");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Register WebSocket route
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_websocket(router, "/ws",
                                                        (httpd_ws_handler_t)test_handler_1));

    // Test lookup
    radix_match_t m = radix_lookup(router->tree, "/ws", HTTP_GET, true);
    TEST_ASSERT_TRUE(m.matched);
    TEST_ASSERT_TRUE(m.is_websocket);
    if (m.middlewares) free(m.middlewares);

    httpd_router_destroy(router);
}

static void test_router_middleware(void) {
    ESP_LOGI(TAG, "Test: Router middleware");

    httpd_router_t router = httpd_router_create();
    TEST_ASSERT_NOT_NULL(router);

    // Add middleware
    httpd_middleware_t mw1 = (httpd_middleware_t)test_handler_1;
    httpd_middleware_t mw2 = (httpd_middleware_t)test_handler_2;

    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_use(router, mw1));
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_router_use(router, mw2));

    // Verify middleware was added (internal check)
    // Note: This accesses internal fields for testing purposes
    TEST_ASSERT_EQUAL(2, router->middleware_count);
    TEST_ASSERT_EQUAL_PTR(mw1, router->middlewares[0]);
    TEST_ASSERT_EQUAL_PTR(mw2, router->middlewares[1]);

    httpd_router_destroy(router);
}

// Nested router mounting has been removed - routers now mount only on servers
// Tests for httpd_mount() should be in integration tests

// ============================================================================
// Phase 4: Handler Chains (Express-style multiple handlers)
// ============================================================================

static int chain_handler_order[10];
static int chain_handler_index = 0;

static httpd_err_t chain_handler_a(httpd_req_t* req) {
    chain_handler_order[chain_handler_index++] = 1;
    return HTTPD_OK;
}

static httpd_err_t chain_handler_b(httpd_req_t* req) {
    chain_handler_order[chain_handler_index++] = 2;
    return HTTPD_OK;
}

static httpd_err_t chain_handler_c(httpd_req_t* req) {
    chain_handler_order[chain_handler_index++] = 3;
    return HTTPD_OK;
}

static void reset_chain_state(void) {
    chain_handler_index = 0;
    memset(chain_handler_order, 0, sizeof(chain_handler_order));
}

static void test_handler_chain_single(void) {
    ESP_LOGI(TAG, "Test: Single handler chain (backwards compat)");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert single handler
    radix_insert(tree, "/api/single", HTTP_GET, chain_handler_a, (void*)0x1, NULL, 0);

    // Lookup - should get single handler
    radix_match_t match = radix_lookup(tree, "/api/single", HTTP_GET, false);
    TEST_ASSERT_TRUE(match.matched);
    TEST_ASSERT_NOT_NULL(match.handler);
    TEST_ASSERT_EQUAL_PTR(chain_handler_a, match.handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x1, match.user_ctx);

    // Handler chain should have one element
    TEST_ASSERT_NOT_NULL(match.handler_chain);
    TEST_ASSERT_EQUAL_PTR(chain_handler_a, match.handler_chain->handler);
    TEST_ASSERT_NULL(match.handler_chain->next);

    if (match.middlewares) free(match.middlewares);
    radix_tree_destroy(tree);
}

static void test_handler_chain_multiple(void) {
    ESP_LOGI(TAG, "Test: Multiple handlers in chain");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert three handlers for same route/method
    radix_insert(tree, "/api/chain", HTTP_GET, chain_handler_a, (void*)0x1, NULL, 0);
    radix_insert(tree, "/api/chain", HTTP_GET, chain_handler_b, (void*)0x2, NULL, 0);
    radix_insert(tree, "/api/chain", HTTP_GET, chain_handler_c, (void*)0x3, NULL, 0);

    // Lookup - should get chain
    radix_match_t match = radix_lookup(tree, "/api/chain", HTTP_GET, false);
    TEST_ASSERT_TRUE(match.matched);

    // First handler for backwards compat
    TEST_ASSERT_EQUAL_PTR(chain_handler_a, match.handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x1, match.user_ctx);

    // Verify full chain
    TEST_ASSERT_NOT_NULL(match.handler_chain);

    handler_node_t* node = match.handler_chain;
    TEST_ASSERT_EQUAL_PTR(chain_handler_a, node->handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x1, node->user_ctx);

    node = node->next;
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_PTR(chain_handler_b, node->handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x2, node->user_ctx);

    node = node->next;
    TEST_ASSERT_NOT_NULL(node);
    TEST_ASSERT_EQUAL_PTR(chain_handler_c, node->handler);
    TEST_ASSERT_EQUAL_PTR((void*)0x3, node->user_ctx);

    // Chain should end
    TEST_ASSERT_NULL(node->next);

    if (match.middlewares) free(match.middlewares);
    radix_tree_destroy(tree);
}

static void test_handler_chain_different_methods(void) {
    ESP_LOGI(TAG, "Test: Handler chains independent per method");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert handlers for different methods
    radix_insert(tree, "/api/resource", HTTP_GET, chain_handler_a, (void*)1, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_GET, chain_handler_b, (void*)2, NULL, 0);
    radix_insert(tree, "/api/resource", HTTP_POST, chain_handler_c, (void*)3, NULL, 0);

    // GET should have chain of 2
    radix_match_t get_match = radix_lookup(tree, "/api/resource", HTTP_GET, false);
    TEST_ASSERT_TRUE(get_match.matched);
    TEST_ASSERT_NOT_NULL(get_match.handler_chain);
    TEST_ASSERT_NOT_NULL(get_match.handler_chain->next);
    TEST_ASSERT_NULL(get_match.handler_chain->next->next);
    if (get_match.middlewares) free(get_match.middlewares);

    // POST should have chain of 1
    radix_match_t post_match = radix_lookup(tree, "/api/resource", HTTP_POST, false);
    TEST_ASSERT_TRUE(post_match.matched);
    TEST_ASSERT_NOT_NULL(post_match.handler_chain);
    TEST_ASSERT_EQUAL_PTR(chain_handler_c, post_match.handler_chain->handler);
    TEST_ASSERT_NULL(post_match.handler_chain->next);
    if (post_match.middlewares) free(post_match.middlewares);

    radix_tree_destroy(tree);
}

static void test_handler_chain_with_params(void) {
    ESP_LOGI(TAG, "Test: Handler chain with route parameters");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Insert chain on parameterized route
    radix_insert(tree, "/users/:id", HTTP_GET, chain_handler_a, NULL, NULL, 0);
    radix_insert(tree, "/users/:id", HTTP_GET, chain_handler_b, NULL, NULL, 0);

    // Lookup with param value
    radix_match_t match = radix_lookup(tree, "/users/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(match.matched);

    // Chain should have 2 handlers
    TEST_ASSERT_NOT_NULL(match.handler_chain);
    TEST_ASSERT_NOT_NULL(match.handler_chain->next);
    TEST_ASSERT_NULL(match.handler_chain->next->next);

    // Parameter should still be extracted
    TEST_ASSERT_EQUAL(1, match.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", match.params[0].key, match.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", match.params[0].value, match.params[0].value_len);

    if (match.middlewares) free(match.middlewares);
    radix_tree_destroy(tree);
}

// ============================================================================
// Phase 5: Optional Parameters
// ============================================================================

static void test_optional_param_basic(void) {
    ESP_LOGI(TAG, "Test: Optional parameter basic matching");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Route: /users/:id?
    radix_insert(tree, "/users/:id?", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should match with param provided
    radix_match_t m1 = radix_lookup(tree, "/users/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m1.handler);
    TEST_ASSERT_EQUAL(1, m1.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", m1.params[0].key, m1.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", m1.params[0].value, m1.params[0].value_len);
    if (m1.middlewares) free(m1.middlewares);

    // Should also match without param
    radix_match_t m2 = radix_lookup(tree, "/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m2.handler);
    TEST_ASSERT_EQUAL(0, m2.param_count);  // No param extracted
    if (m2.middlewares) free(m2.middlewares);

    radix_tree_destroy(tree);
}

static void test_optional_param_with_trailing_slash(void) {
    ESP_LOGI(TAG, "Test: Optional parameter with trailing slash");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_insert(tree, "/api/:version?", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // With param
    radix_match_t m1 = radix_lookup(tree, "/api/v2", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL(1, m1.param_count);
    if (m1.middlewares) free(m1.middlewares);

    // Without param, trailing slash
    radix_match_t m2 = radix_lookup(tree, "/api/", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL(0, m2.param_count);
    if (m2.middlewares) free(m2.middlewares);

    // Without param, no trailing slash
    radix_match_t m3 = radix_lookup(tree, "/api", HTTP_GET, false);
    TEST_ASSERT_TRUE(m3.matched);
    TEST_ASSERT_EQUAL(0, m3.param_count);
    if (m3.middlewares) free(m3.middlewares);

    radix_tree_destroy(tree);
}

static void test_optional_param_mixed_with_required(void) {
    ESP_LOGI(TAG, "Test: Optional param after required param");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Route: /users/:userId/posts/:postId?
    radix_insert(tree, "/users/:userId/posts/:postId?", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Both params provided
    radix_match_t m1 = radix_lookup(tree, "/users/42/posts/99", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL(2, m1.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("userId", m1.params[0].key, m1.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", m1.params[0].value, m1.params[0].value_len);
    TEST_ASSERT_EQUAL_STRING_LEN("postId", m1.params[1].key, m1.params[1].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("99", m1.params[1].value, m1.params[1].value_len);
    if (m1.middlewares) free(m1.middlewares);

    // Only required param provided
    radix_match_t m2 = radix_lookup(tree, "/users/42/posts", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL(1, m2.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("userId", m2.params[0].key, m2.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("42", m2.params[0].value, m2.params[0].value_len);
    if (m2.middlewares) free(m2.middlewares);

    // Missing required param - should not match
    radix_match_t m3 = radix_lookup(tree, "/users", HTTP_GET, false);
    TEST_ASSERT_FALSE(m3.matched);

    radix_tree_destroy(tree);
}

static void test_optional_param_not_confused_with_required(void) {
    ESP_LOGI(TAG, "Test: Optional vs required param distinction");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Required param
    radix_insert(tree, "/required/:id", HTTP_GET, test_handler_1, (void*)1, NULL, 0);
    // Optional param
    radix_insert(tree, "/optional/:id?", HTTP_GET, test_handler_2, (void*)2, NULL, 0);

    // Required should not match without param
    radix_match_t m1 = radix_lookup(tree, "/required", HTTP_GET, false);
    TEST_ASSERT_FALSE(m1.matched);

    // Required should match with param
    radix_match_t m2 = radix_lookup(tree, "/required/123", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m2.handler);
    if (m2.middlewares) free(m2.middlewares);

    // Optional should match without param
    radix_match_t m3 = radix_lookup(tree, "/optional", HTTP_GET, false);
    TEST_ASSERT_TRUE(m3.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m3.handler);
    if (m3.middlewares) free(m3.middlewares);

    // Optional should match with param
    radix_match_t m4 = radix_lookup(tree, "/optional/456", HTTP_GET, false);
    TEST_ASSERT_TRUE(m4.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_2, m4.handler);
    if (m4.middlewares) free(m4.middlewares);

    radix_tree_destroy(tree);
}

// ============================================================================
// Phase 6: Case-Insensitive Routing
// ============================================================================

static void test_case_insensitive_basic(void) {
    ESP_LOGI(TAG, "Test: Case-insensitive basic matching");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Enable case-insensitive routing
    radix_tree_set_case_sensitive(tree, false);

    // Register with lowercase
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should match exact case
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m1.handler);
    if (m1.middlewares) free(m1.middlewares);

    // Should match uppercase
    radix_match_t m2 = radix_lookup(tree, "/API/USERS", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m2.handler);
    if (m2.middlewares) free(m2.middlewares);

    // Should match mixed case
    radix_match_t m3 = radix_lookup(tree, "/Api/Users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m3.matched);
    TEST_ASSERT_EQUAL_PTR(test_handler_1, m3.handler);
    if (m3.middlewares) free(m3.middlewares);

    radix_tree_destroy(tree);
}

static void test_case_sensitive_default(void) {
    ESP_LOGI(TAG, "Test: Case-sensitive is default");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Don't change case_sensitive - should be true by default
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should match exact case
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    if (m1.middlewares) free(m1.middlewares);

    // Should NOT match different case
    radix_match_t m2 = radix_lookup(tree, "/API/USERS", HTTP_GET, false);
    TEST_ASSERT_FALSE(m2.matched);

    radix_tree_destroy(tree);
}

static void test_case_insensitive_with_params(void) {
    ESP_LOGI(TAG, "Test: Case-insensitive with parameters");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_tree_set_case_sensitive(tree, false);
    radix_insert(tree, "/users/:id/profile", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Mixed case should match
    radix_match_t m1 = radix_lookup(tree, "/Users/123/Profile", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    TEST_ASSERT_EQUAL(1, m1.param_count);
    TEST_ASSERT_EQUAL_STRING_LEN("id", m1.params[0].key, m1.params[0].key_len);
    TEST_ASSERT_EQUAL_STRING_LEN("123", m1.params[0].value, m1.params[0].value_len);
    if (m1.middlewares) free(m1.middlewares);

    radix_tree_destroy(tree);
}

// ============================================================================
// Phase 7: Strict Routing
// ============================================================================

static void test_strict_mode_trailing_slash(void) {
    ESP_LOGI(TAG, "Test: Strict mode trailing slash");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Enable strict mode
    radix_tree_set_strict(tree, true);

    // Register route WITHOUT trailing slash
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should match exact path
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    if (m1.middlewares) free(m1.middlewares);

    // Should NOT match with trailing slash in strict mode
    radix_match_t m2 = radix_lookup(tree, "/api/users/", HTTP_GET, false);
    TEST_ASSERT_FALSE(m2.matched);

    radix_tree_destroy(tree);
}

static void test_strict_mode_with_trailing_slash_route(void) {
    ESP_LOGI(TAG, "Test: Strict mode with trailing slash route");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    radix_tree_set_strict(tree, true);

    // Register route WITH trailing slash
    radix_insert(tree, "/api/users/", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should NOT match without trailing slash
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_FALSE(m1.matched);

    // Should match with trailing slash
    radix_match_t m2 = radix_lookup(tree, "/api/users/", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    if (m2.middlewares) free(m2.middlewares);

    radix_tree_destroy(tree);
}

static void test_non_strict_mode_ignores_trailing_slash(void) {
    ESP_LOGI(TAG, "Test: Non-strict mode ignores trailing slash");

    radix_tree_t* tree = radix_tree_create();
    TEST_ASSERT_NOT_NULL(tree);

    // Default is non-strict (don't change)
    radix_insert(tree, "/api/users", HTTP_GET, test_handler_1, (void*)1, NULL, 0);

    // Should match exact path
    radix_match_t m1 = radix_lookup(tree, "/api/users", HTTP_GET, false);
    TEST_ASSERT_TRUE(m1.matched);
    if (m1.middlewares) free(m1.middlewares);

    // Should also match with trailing slash in non-strict mode
    radix_match_t m2 = radix_lookup(tree, "/api/users/", HTTP_GET, false);
    TEST_ASSERT_TRUE(m2.matched);
    if (m2.middlewares) free(m2.middlewares);

    radix_tree_destroy(tree);
}

// ============================================================================
// Test Runner
// ============================================================================

void test_radix_tree_run(void) {
    // Phase 1: Basic operations
    RUN_TEST(test_radix_tree_create_destroy);
    RUN_TEST(test_radix_insert_single_static_route);
    RUN_TEST(test_radix_lookup_static_route);
    RUN_TEST(test_radix_multiple_routes);
    RUN_TEST(test_radix_nested_routes);
    RUN_TEST(test_radix_root_route);
    RUN_TEST(test_radix_websocket_route);
    RUN_TEST(test_radix_all_http_methods);

    // Phase 2: Parameters and wildcards
    RUN_TEST(test_radix_param_route);
    RUN_TEST(test_radix_multiple_params);
    RUN_TEST(test_radix_param_priority);
    RUN_TEST(test_radix_wildcard_route);
    RUN_TEST(test_radix_wildcard_priority);

    // Phase 3: Router API
    RUN_TEST(test_router_create_destroy);
    RUN_TEST(test_router_convenience_functions);
    RUN_TEST(test_router_all_method);
    RUN_TEST(test_router_websocket);
    RUN_TEST(test_router_middleware);

    // Phase 4: Handler chains
    RUN_TEST(test_handler_chain_single);
    RUN_TEST(test_handler_chain_multiple);
    RUN_TEST(test_handler_chain_different_methods);
    RUN_TEST(test_handler_chain_with_params);

    // Phase 5: Optional parameters
    RUN_TEST(test_optional_param_basic);
    RUN_TEST(test_optional_param_with_trailing_slash);
    RUN_TEST(test_optional_param_mixed_with_required);
    RUN_TEST(test_optional_param_not_confused_with_required);

    // Phase 6: Case-insensitive routing
    RUN_TEST(test_case_insensitive_basic);
    RUN_TEST(test_case_sensitive_default);
    RUN_TEST(test_case_insensitive_with_params);

    // Phase 7: Strict routing
    RUN_TEST(test_strict_mode_trailing_slash);
    RUN_TEST(test_strict_mode_with_trailing_slash_route);
    RUN_TEST(test_non_strict_mode_ignores_trailing_slash);
}
