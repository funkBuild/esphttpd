#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esphttpd.h"
#include "test_exports.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ESPHTTPD_TEST";

// Unity setUp/tearDown - called before/after EVERY test.
//
// tearDown defensively stops the server if it is still running. This prevents
// cascade failures when a test assertion fires mid-execution (after
// start_test_server but before stop_test_server). Without this, the leaked
// server would cause every subsequent test that calls httpd_start to fail
// with HTTPD_ERR_ALREADY_STARTED or similar.
//
// We use the global g_server pointer (set by httpd_start, cleared by
// httpd_stop) as the single source of truth. Each test file has its own
// static handle variable, but those are irrelevant here; httpd_stop via
// g_server is sufficient because httpd_stop sets g_server = NULL internally.
// The per-file static handles will be stale after tearDown, but every test
// that uses start_test_server calls httpd_start which overwrites them anyway.

void setUp(void) {
    // Nothing needed before each test
}

void tearDown(void) {
    // If the server is still running (test failed before stop_test_server),
    // stop it to prevent cascade failures in subsequent tests.
    if (g_server != NULL) {
        httpd_stop((httpd_handle_t)g_server);
        // httpd_stop sets g_server = NULL internally
    }
}

// Declare test functions
void test_http_parser_run(void);
void test_websocket_frame_run(void);
void test_connection_run(void);
void test_event_loop_run(void);
void test_template_run(void);
void test_integration_run(void);
void test_radix_tree_run(void);
void test_defer_run(void);
void test_performance_run(void);
void test_send_buffer_run(void);
void test_filesystem_run(void);
void test_nonblocking_run(void);
void test_http_api_run(void);
void test_router_api_run(void);
void test_websocket_api_run(void);
void test_middleware_run(void);

void app_main(void)
{
    ESP_LOGI(TAG, "Starting esphttpd Unity tests on ESP32S3 QEMU");

    // Wait a bit for QEMU to stabilize
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Initialize Unity
    UNITY_BEGIN();

    // Run test suites
    ESP_LOGI(TAG, "Running HTTP Parser tests...");
    test_http_parser_run();

    ESP_LOGI(TAG, "Running WebSocket Frame tests...");
    test_websocket_frame_run();

    ESP_LOGI(TAG, "Running Connection tests...");
    test_connection_run();

    ESP_LOGI(TAG, "Running Event Loop tests...");
    test_event_loop_run();

    ESP_LOGI(TAG, "Running Template tests...");
    test_template_run();

    ESP_LOGI(TAG, "Running Radix Tree tests...");
    test_radix_tree_run();

    ESP_LOGI(TAG, "Running Integration tests...");
    test_integration_run();

    ESP_LOGI(TAG, "Running Defer (Async) tests...");
    test_defer_run();

    ESP_LOGI(TAG, "Running Send Buffer tests...");
    test_send_buffer_run();

    ESP_LOGI(TAG, "Running Filesystem tests...");
    test_filesystem_run();

    ESP_LOGI(TAG, "Running Non-blocking I/O tests...");
    test_nonblocking_run();

    ESP_LOGI(TAG, "Running HTTP API tests...");
    test_http_api_run();

    ESP_LOGI(TAG, "Running Router API tests...");
    test_router_api_run();

    ESP_LOGI(TAG, "Running WebSocket API tests...");
    test_websocket_api_run();

    ESP_LOGI(TAG, "Running Middleware tests...");
    test_middleware_run();

    ESP_LOGI(TAG, "Running Performance benchmarks...");
    test_performance_run();

    // End Unity tests
    int failures = UNITY_END();

    // Print results
    if (failures == 0) {
        ESP_LOGI(TAG, "All tests passed!");
    } else {
        ESP_LOGE(TAG, "%d test(s) failed", failures);
    }

    // For QEMU: print a marker that tests are complete
    printf("QEMU_TEST_COMPLETE: %s\n", failures == 0 ? "PASS" : "FAIL");

    // Keep alive for QEMU
    while(1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}