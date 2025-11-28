#include "esphttpd.h"
#include "private/radix_tree.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char* TAG = "ROUTER_API";

// Note: httpd_router_t is defined in esphttpd.h as "struct httpd_router*"
// The struct httpd_router itself is defined in radix_tree.h

// ============================================================================
// Router Lifecycle
// ============================================================================

httpd_router_t httpd_router_create(void) {
    // Allocate the actual struct, not just a pointer
    httpd_router_t router = (httpd_router_t)calloc(1, sizeof(struct httpd_router));
    if (!router) {
        ESP_LOGE(TAG, "Failed to allocate router");
        return NULL;
    }

    router->tree = radix_tree_create();
    if (!router->tree) {
        ESP_LOGE(TAG, "Failed to create radix tree");
        free(router);
        return NULL;
    }

    router->middleware_count = 0;
    router->error_handler = NULL;

    ESP_LOGI(TAG, "Created router");
    return router;
}

void httpd_router_destroy(httpd_router_t router) {
    if (!router) return;

    ESP_LOGI(TAG, "Destroying router");

    // Destroy radix tree
    if (router->tree) {
        radix_tree_destroy(router->tree);
    }

    free(router);
}

// ============================================================================
// Route Registration
// ============================================================================

httpd_err_t httpd_router_get(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, HTTP_GET, handler, NULL, NULL, 0);
}

httpd_err_t httpd_router_post(httpd_router_t router, const char* pattern,
                              httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, HTTP_POST, handler, NULL, NULL, 0);
}

httpd_err_t httpd_router_put(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, HTTP_PUT, handler, NULL, NULL, 0);
}

httpd_err_t httpd_router_delete(httpd_router_t router, const char* pattern,
                                httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, HTTP_DELETE, handler, NULL, NULL, 0);
}

httpd_err_t httpd_router_patch(httpd_router_t router, const char* pattern,
                               httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, HTTP_PATCH, handler, NULL, NULL, 0);
}

httpd_err_t httpd_router_all(httpd_router_t router, const char* pattern,
                             httpd_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;

    // Register for all HTTP methods
    httpd_err_t err;
    for (int method = HTTP_GET; method <= HTTP_PATCH; method++) {
        err = radix_insert(router->tree, pattern, (http_method_t)method,
                          handler, NULL, NULL, 0);
        if (err != HTTPD_OK) return err;
    }

    return HTTPD_OK;
}

httpd_err_t httpd_router_route(httpd_router_t router, const char* pattern,
                               http_method_t method, httpd_handler_t handler,
                               void* user_ctx) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert(router->tree, pattern, method, handler, user_ctx, NULL, 0);
}

httpd_err_t httpd_router_websocket(httpd_router_t router, const char* pattern,
                                   httpd_ws_handler_t handler) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert_ws(router->tree, pattern, handler, NULL, 0, NULL, 0);
}

httpd_err_t httpd_router_ws_route(httpd_router_t router, const char* pattern,
                                  httpd_ws_handler_t handler, void* user_ctx,
                                  uint32_t ping_interval_ms) {
    if (!router || !pattern || !handler) return HTTPD_ERR_INVALID_ARG;
    return radix_insert_ws(router->tree, pattern, handler, user_ctx,
                          ping_interval_ms, NULL, 0);
}

// ============================================================================
// Middleware
// ============================================================================

httpd_err_t httpd_router_use(httpd_router_t router, httpd_middleware_t middleware) {
    if (!router || !middleware) return HTTPD_ERR_INVALID_ARG;

    if (router->middleware_count >= CONFIG_HTTPD_MAX_MIDDLEWARE_PER_ROUTER) {
        ESP_LOGE(TAG, "Router middleware limit reached");
        return HTTPD_ERR_NO_MEM;
    }

    router->middlewares[router->middleware_count++] = middleware;
    ESP_LOGI(TAG, "Added router middleware (count=%d)", router->middleware_count);
    return HTTPD_OK;
}

// ============================================================================
// Error Handling
// ============================================================================

httpd_err_t httpd_router_on_error(httpd_router_t router, httpd_err_handler_t handler) {
    if (!router || !handler) return HTTPD_ERR_INVALID_ARG;

    router->error_handler = handler;
    ESP_LOGI(TAG, "Set router error handler");
    return HTTPD_OK;
}
