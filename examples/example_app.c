/**
 * Example application demonstrating the esphttpd server
 *
 * Features demonstrated:
 * - HTTP routing with GET, POST handlers
 * - WebSocket support with echo
 * - Response building using httpd_resp_* API
 * - Request body reading
 */

#include "esphttpd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char* TAG = "EXAMPLE_APP";
static httpd_handle_t server = NULL;

// ==================== HTTP ROUTE HANDLERS ====================

// Handle GET /
static httpd_err_t handle_index(httpd_req_t* req) {
    const char* html =
        "<!DOCTYPE html>"
        "<html>"
        "<head><title>ESP HTTP Server</title></head>"
        "<body>"
        "<h1>Welcome to ESP HTTP Server</h1>"
        "<p>High-performance HTTP/WebSocket server for ESP32</p>"
        "<ul>"
        "<li>Event-driven with select()</li>"
        "<li>Connection pooling</li>"
        "<li>WebSocket support</li>"
        "<li>Pub/Sub channels</li>"
        "</ul>"
        "</body>"
        "</html>";

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return HTTPD_OK;
}

// Handle GET /api/status
static httpd_err_t handle_api_status(httpd_req_t* req) {
    char json[128];
    int len = snprintf(json, sizeof(json),
        "{\"status\":\"ok\",\"ws_connections\":%u}",
        httpd_ws_get_connection_count(server));

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return HTTPD_OK;
}

// Handle POST /api/data
static httpd_err_t handle_api_data(httpd_req_t* req) {
    // Read request body
    char body[256];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received > 0) {
        body[received] = '\0';
        ESP_LOGI(TAG, "Received POST data: %s", body);
    }

    const char* response = "{\"result\":\"data received\"}";
    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return HTTPD_OK;
}

// ==================== WEBSOCKET HANDLER ====================

static httpd_err_t handle_websocket(httpd_ws_t* ws, httpd_ws_event_t* event) {
    switch (event->type) {
        case WS_EVENT_CONNECT:
            ESP_LOGI(TAG, "WebSocket client connected");
            // Send welcome message
            const char* welcome = "{\"type\":\"welcome\",\"message\":\"Connected to ESP32\"}";
            httpd_ws_send(ws, welcome, strlen(welcome), WS_TYPE_TEXT);
            break;

        case WS_EVENT_MESSAGE:
            ESP_LOGI(TAG, "WebSocket message received: %.*s",
                     (int)event->len, (char*)event->data);
            // Echo message back
            httpd_ws_send(ws, event->data, event->len, event->frame_type);
            break;

        case WS_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "WebSocket client disconnected");
            break;

        case WS_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;
    }
    return HTTPD_OK;
}

// ==================== MAIN APPLICATION ====================

void app_main(void) {
    ESP_LOGI(TAG, "Starting example application with esphttpd");

    // Configure and start the web server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;

    httpd_err_t err = httpd_start(&server, &config);
    if (err != HTTPD_OK) {
        ESP_LOGE(TAG, "Failed to start server: %d", err);
        return;
    }

    // Add HTTP routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/",
        .handler = handle_index
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/api/status",
        .handler = handle_api_status
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/api/data",
        .handler = handle_api_data
    });

    // Add WebSocket route
    httpd_register_ws_route(server, &(httpd_ws_route_t){
        .pattern = "/ws",
        .handler = handle_websocket
    });

    ESP_LOGI(TAG, "Server running on port 80");
    ESP_LOGI(TAG, "Endpoints: /, /api/status, /api/data, /ws");

    // The server runs in its own task, main task can do other work
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Example: log WebSocket connection count periodically
        unsigned int ws_count = httpd_ws_get_connection_count(server);
        if (ws_count > 0) {
            ESP_LOGI(TAG, "Active WebSocket connections: %u", ws_count);
        }
    }
}
