#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ESPHTTPD_TEST";

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