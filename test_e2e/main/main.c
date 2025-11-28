/**
 * E2E Test Server - Minimal version
 * Using only public esphttpd API
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac.h"
#include "esp_eth_com.h"
#include "esp_eth_netif_glue.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esphttpd.h"

static const char* TAG = "E2E_TEST";
static int request_count = 0;
static esp_eth_handle_t eth_handle = NULL;
static httpd_handle_t server = NULL;

// GET /
static httpd_err_t handle_root(httpd_req_t* req) {
    request_count++;

    char response[512];
    int len = snprintf(response, sizeof(response),
             "<html><body>"
             "<h1>ESP32 E2E Test Server</h1>"
             "<p>Server is running!</p>"
             "<p>Request count: %d</p>"
             "</body></html>",
             request_count);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, len);

    return HTTPD_OK;
}

// Track server start time for uptime calculation
static int64_t server_start_time = 0;

// GET /api/status
static httpd_err_t handle_api_status(httpd_req_t* req) {
    request_count++;

    // Calculate uptime in seconds
    int64_t uptime = (esp_timer_get_time() - server_start_time) / 1000000;

    char json[256];
    int len = snprintf(json, sizeof(json),
             "{\"status\":\"ok\",\"uptime\":%lld,\"requests\":%d,\"ws_connections\":%u}",
             (long long)uptime, request_count, httpd_ws_get_connection_count(server));

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, len);

    return HTTPD_OK;
}

// POST /api/echo
static httpd_err_t handle_api_echo(httpd_req_t* req) {
    request_count++;

    // Get Content-Type from request
    const char* content_type = httpd_req_get_header(req, "Content-Type");
    bool is_json = content_type && strstr(content_type, "application/json") != NULL;

    // Read the request body - smaller buffer for stack safety
    char body[512];
    size_t received = httpd_req_recv(req, body, sizeof(body) - 1);
    body[received] = '\0';  // Null terminate

    httpd_resp_set_status(req, 200);
    // Echo back with same content type, default to application/json for JSON data
    if (is_json) {
        httpd_resp_set_type(req, "application/json");
    } else if (content_type) {
        httpd_resp_set_type(req, content_type);
    } else {
        httpd_resp_set_type(req, "text/plain");
    }
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, body, received);

    return HTTPD_OK;
}

// GET /cors
static httpd_err_t handle_cors(httpd_req_t* req) {
    const char* response = "{\"cors\":\"enabled\"}";
    int len = strlen(response);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_header(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_header(req, "Access-Control-Allow-Headers", "Content-Type, X-Test-Header, X-Custom-Header");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, len);

    return HTTPD_OK;
}

// OPTIONS /cors
static httpd_err_t handle_cors_options(httpd_req_t* req) {
    httpd_resp_set_status(req, 204);
    httpd_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_header(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_header(req, "Access-Control-Allow-Headers", "Content-Type, X-Test-Header, X-Custom-Header");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, "", 0);

    return HTTPD_OK;
}

// Helper to extract ID from URL like /api/data/123
static const char* extract_id_from_url(const char* url, const char* prefix) {
    size_t prefix_len = strlen(prefix);
    if (strncmp(url, prefix, prefix_len) == 0) {
        return url + prefix_len;
    }
    return "";
}

// GET /api/data/*
static httpd_err_t handle_api_data_get(httpd_req_t* req) {
    request_count++;

    const char* url = httpd_req_get_uri(req);
    const char* id = extract_id_from_url(url, "/api/data/");

    char json[256];
    int len = snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"data\":\"Test data for %s\"}",
             id, id);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, len);

    return HTTPD_OK;
}

// DELETE /api/data/*
static httpd_err_t handle_api_data_delete(httpd_req_t* req) {
    request_count++;

    const char* url = httpd_req_get_uri(req);
    const char* id = extract_id_from_url(url, "/api/data/");

    char json[256];
    int len = snprintf(json, sizeof(json),
             "{\"message\":\"Deleted\",\"id\":\"%s\"}",
             id);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, len);

    return HTTPD_OK;
}

// PUT /api/update
static httpd_err_t handle_api_update(httpd_req_t* req) {
    request_count++;

    // Read the request body to get byte count - smaller buffer for stack safety
    char body[256];
    size_t received = httpd_req_recv(req, body, sizeof(body) - 1);

    char json[128];
    int len = snprintf(json, sizeof(json),
             "{\"message\":\"Updated\",\"bytes_received\":%zu}",
             received);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, len);

    return HTTPD_OK;
}

// HEAD / - responds with headers only, no body
static httpd_err_t handle_head_root(httpd_req_t* req) {
    request_count++;

    // Calculate what the body length would be (but don't send body for HEAD)
    char response[512];
    int len = snprintf(response, sizeof(response),
             "<html><body>"
             "<h1>ESP32 E2E Test Server</h1>"
             "<p>Server is running!</p>"
             "<p>Request count: %d</p>"
             "</body></html>",
             request_count);

    char content_len[32];
    snprintf(content_len, sizeof(content_len), "%d", len);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_header(req, "Content-Length", content_len);
    httpd_resp_set_header(req, "Connection", "close");
    // No body for HEAD request
    httpd_resp_send(req, "", 0);

    return HTTPD_OK;
}

// GET /headers - echo back request headers as JSON (small buffer for stack safety)
static httpd_err_t handle_headers(httpd_req_t* req) {
    request_count++;

    // Keep buffer small to avoid stack overflow
    char response[256];
    int pos = 0;

    // Get specific headers - return as JSON
    const char* host = httpd_req_get_header(req, "Host");
    const char* x_test_header = httpd_req_get_header(req, "X-Test-Header");

    pos += snprintf(response + pos, sizeof(response) - pos, "{\"Host\":\"%s\"", host ? host : "");
    pos += snprintf(response + pos, sizeof(response) - pos, ",\"X-Test-Header\":\"%s\"}", x_test_header ? x_test_header : "");

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, pos);

    return HTTPD_OK;
}

// GET /template - server-side template processing
static httpd_err_t handle_template(httpd_req_t* req) {
    request_count++;

    char response[256];
    int len = snprintf(response, sizeof(response),
             "<html><body><h1>E2E Test Page</h1>"
             "<p>Dynamic content here</p>"
             "<p>Request count: %d</p></body></html>",
             request_count);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, len);

    return HTTPD_OK;
}

// Helper to determine content type from file extension
static const char* get_content_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "text/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";

    return "application/octet-stream";
}

// GET /static/* - serve mock static files
static httpd_err_t handle_static(httpd_req_t* req) {
    request_count++;

    // Extract filename from URL (skip /static/)
    const char* url = httpd_req_get_uri(req);
    const char* filename = url + 8;  // Skip "/static/"
    if (*filename == '\0') {
        filename = "index.html";
    }

    // Create mock response with filename
    char response[512];
    int len = snprintf(response, sizeof(response),
             "Mock static file: %s", filename);

    const char* content_type = get_content_type(filename);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, len);

    return HTTPD_OK;
}

// ============================================================================
// New Feature Tests: Basic Auth, File Upload, 100-Continue
// ============================================================================

// Test credentials for basic auth
#define TEST_AUTH_USER "testuser"
#define TEST_AUTH_PASS "testpass"

// GET/POST /auth/protected - protected resource requiring basic auth
static httpd_err_t handle_auth_protected(httpd_req_t* req) {
    request_count++;

    // Check basic auth credentials
    if (!httpd_check_basic_auth(req, TEST_AUTH_USER, TEST_AUTH_PASS)) {
        // Send 401 challenge with realm
        return httpd_resp_send_auth_challenge(req, "Protected Area");
    }

    // Auth successful - return protected content
    const char* json = "{\"status\":\"authenticated\",\"message\":\"Welcome to the protected area\"}";
    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// POST /upload - upload file using pipe_to_file
static httpd_err_t handle_upload(httpd_req_t* req) {
    request_count++;

    // Upload to a fixed path in littlefs (or RAM filesystem for test)
    const char* upload_path = "/littlefs/uploaded_file.bin";

    ssize_t bytes_written = httpd_req_pipe_to_file(req, upload_path);

    char json[128];
    if (bytes_written >= 0) {
        snprintf(json, sizeof(json),
            "{\"status\":\"success\",\"bytes\":%zd,\"path\":\"%s\"}",
            bytes_written, upload_path);
        httpd_resp_set_status(req, 200);
    } else {
        snprintf(json, sizeof(json),
            "{\"status\":\"error\",\"code\":%zd}", bytes_written);
        httpd_resp_set_status(req, 500);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// POST /upload/continue - test 100-continue handling explicitly
static httpd_err_t handle_upload_continue(httpd_req_t* req) {
    request_count++;

    // Manually check and respond to Expect: 100-continue
    const char* expect = httpd_req_get_header(req, "Expect");
    bool had_expect = (expect && strcasecmp(expect, "100-continue") == 0);

    if (had_expect) {
        // Manually send 100 Continue before reading body
        httpd_resp_send_continue(req);
    }

    // Read the body
    char body[512];
    size_t total = 0;
    int received;
    while ((received = httpd_req_recv(req, body + total, sizeof(body) - total - 1)) > 0) {
        total += received;
        if (total >= sizeof(body) - 1) break;
    }
    body[total] = '\0';

    char json[256];
    snprintf(json, sizeof(json),
        "{\"status\":\"success\",\"expect_header\":%s,\"bytes_received\":%zu,\"body\":\"%.*s\"}",
        had_expect ? "true" : "false",
        total,
        (int)(total > 50 ? 50 : total), body);  // Truncate body in response

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// GET /upload/verify - verify uploaded file exists and return its size
static httpd_err_t handle_upload_verify(httpd_req_t* req) {
    request_count++;

    const char* upload_path = "/littlefs/uploaded_file.bin";
    FILE* fp = fopen(upload_path, "rb");

    char json[128];
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fclose(fp);

        snprintf(json, sizeof(json),
            "{\"exists\":true,\"size\":%ld,\"path\":\"%s\"}", size, upload_path);
    } else {
        snprintf(json, sizeof(json),
            "{\"exists\":false,\"path\":\"%s\"}", upload_path);
    }

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// DELETE /upload - delete uploaded file
static httpd_err_t handle_upload_delete(httpd_req_t* req) {
    request_count++;

    const char* upload_path = "/littlefs/uploaded_file.bin";
    int result = remove(upload_path);

    char json[64];
    snprintf(json, sizeof(json), "{\"deleted\":%s}", result == 0 ? "true" : "false");

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// ============================================================================
// Deferred (Async) Upload Tests
// ============================================================================

// Done callback for deferred file upload
static void defer_upload_done(httpd_req_t* req, httpd_err_t err) {
    char json[128];

    if (err == HTTPD_OK) {
        // Verify the file was written
        const char* upload_path = "/littlefs/deferred_upload.bin";
        FILE* fp = fopen(upload_path, "rb");
        long size = 0;
        if (fp) {
            fseek(fp, 0, SEEK_END);
            size = ftell(fp);
            fclose(fp);
        }

        snprintf(json, sizeof(json),
            "{\"status\":\"success\",\"deferred\":true,\"bytes\":%ld,\"path\":\"%s\"}",
            size, upload_path);
        httpd_resp_set_status(req, 200);
    } else {
        snprintf(json, sizeof(json),
            "{\"status\":\"error\",\"deferred\":true,\"code\":%d}", err);
        httpd_resp_set_status(req, 500);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
}

// POST /upload/defer - upload file using httpd_req_defer_to_file (async)
static httpd_err_t handle_upload_defer(httpd_req_t* req) {
    request_count++;

    // Use deferred (async) file upload
    const char* upload_path = "/littlefs/deferred_upload.bin";

    ESP_LOGI(TAG, "Starting deferred upload to %s (content_length=%zu)",
             upload_path, req->content_length);

    httpd_err_t err = httpd_req_defer_to_file(req, upload_path, defer_upload_done);
    if (err != HTTPD_OK) {
        // Error starting defer - send error response immediately
        char json[64];
        snprintf(json, sizeof(json), "{\"status\":\"error\",\"code\":%d}", err);
        httpd_resp_set_status(req, 500);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
    }

    // If defer succeeded, response will be sent by defer_upload_done callback
    return HTTPD_OK;
}

// Custom deferred upload context
typedef struct {
    FILE* fp;
    size_t total_bytes;
    size_t bytes_written;
    int chunk_count;
} custom_defer_ctx_t;

// Custom body callback - tracks chunks and progress
static httpd_err_t custom_defer_body(httpd_req_t* req, const void* data, size_t len) {
    custom_defer_ctx_t* ctx = httpd_req_get_user_data(req);
    if (!ctx || !ctx->fp) {
        return HTTPD_ERR_IO;
    }

    size_t written = fwrite(data, 1, len, ctx->fp);
    if (written != len) {
        ESP_LOGE(TAG, "Custom defer: write failed");
        return HTTPD_ERR_IO;
    }

    ctx->bytes_written += written;
    ctx->chunk_count++;

    ESP_LOGD(TAG, "Custom defer: chunk %d, %zu bytes (total: %zu/%zu)",
             ctx->chunk_count, len, ctx->bytes_written, ctx->total_bytes);

    return HTTPD_OK;
}

// Custom done callback - includes chunk count in response
static void custom_defer_done(httpd_req_t* req, httpd_err_t err) {
    custom_defer_ctx_t* ctx = httpd_req_get_user_data(req);
    char json[256];

    if (ctx) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }

        if (err == HTTPD_OK) {
            snprintf(json, sizeof(json),
                "{\"status\":\"success\",\"deferred\":true,\"custom\":true,"
                "\"bytes\":%zu,\"chunks\":%d}",
                ctx->bytes_written, ctx->chunk_count);
            httpd_resp_set_status(req, 200);
        } else {
            snprintf(json, sizeof(json),
                "{\"status\":\"error\",\"deferred\":true,\"custom\":true,"
                "\"code\":%d,\"bytes_before_error\":%zu}",
                err, ctx->bytes_written);
            httpd_resp_set_status(req, 500);
        }

        free(ctx);
    } else {
        snprintf(json, sizeof(json),
            "{\"status\":\"error\",\"message\":\"context lost\"}");
        httpd_resp_set_status(req, 500);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
}

// POST /upload/defer/custom - upload with custom body callback (demonstrates full API)
static httpd_err_t handle_upload_defer_custom(httpd_req_t* req) {
    request_count++;

    // Allocate custom context
    custom_defer_ctx_t* ctx = calloc(1, sizeof(custom_defer_ctx_t));
    if (!ctx) {
        httpd_resp_send_error(req, 500, "Out of memory");
        return HTTPD_OK;
    }

    // Open file
    ctx->fp = fopen("/littlefs/custom_defer_upload.bin", "wb");
    if (!ctx->fp) {
        free(ctx);
        httpd_resp_send_error(req, 500, "Cannot create file");
        return HTTPD_OK;
    }

    ctx->total_bytes = req->content_length;
    ctx->bytes_written = 0;
    ctx->chunk_count = 0;

    // Store context in request
    httpd_req_set_user_data(req, ctx);

    ESP_LOGI(TAG, "Starting custom deferred upload (content_length=%zu)",
             req->content_length);

    // Start deferred handling with custom callbacks
    httpd_err_t err = httpd_req_defer(req, custom_defer_body, custom_defer_done);
    if (err != HTTPD_OK) {
        fclose(ctx->fp);
        free(ctx);
        char json[64];
        snprintf(json, sizeof(json), "{\"status\":\"error\",\"code\":%d}", err);
        httpd_resp_set_status(req, 500);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
    }

    return HTTPD_OK;
}

// GET /upload/defer/verify - verify deferred uploaded files
static httpd_err_t handle_upload_defer_verify(httpd_req_t* req) {
    request_count++;

    // Check both upload paths
    const char* defer_path = "/littlefs/deferred_upload.bin";
    const char* custom_path = "/littlefs/custom_defer_upload.bin";

    long defer_size = -1;
    long custom_size = -1;

    FILE* fp = fopen(defer_path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        defer_size = ftell(fp);
        fclose(fp);
    }

    fp = fopen(custom_path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        custom_size = ftell(fp);
        fclose(fp);
    }

    char json[256];
    snprintf(json, sizeof(json),
        "{\"deferred\":{\"exists\":%s,\"size\":%ld},"
        "\"custom\":{\"exists\":%s,\"size\":%ld}}",
        defer_size >= 0 ? "true" : "false", defer_size >= 0 ? defer_size : 0,
        custom_size >= 0 ? "true" : "false", custom_size >= 0 ? custom_size : 0);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// DELETE /upload/defer - delete deferred uploaded files
static httpd_err_t handle_upload_defer_delete(httpd_req_t* req) {
    request_count++;

    int defer_result = remove("/littlefs/deferred_upload.bin");
    int custom_result = remove("/littlefs/custom_defer_upload.bin");

    char json[128];
    snprintf(json, sizeof(json),
        "{\"deferred_deleted\":%s,\"custom_deleted\":%s}",
        defer_result == 0 ? "true" : "false",
        custom_result == 0 ? "true" : "false");

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));

    return HTTPD_OK;
}

// ============================================================================
// Concurrent Request Test: Large Upload Sink + Hello World
// ============================================================================

// Context for sink upload (tracks progress without storing data)
typedef struct {
    size_t total_bytes;
    size_t bytes_received;
    int chunk_count;
    int64_t start_time;
} sink_upload_ctx_t;

// Body callback for sink - just discards the data
static httpd_err_t sink_body_callback(httpd_req_t* req, const void* data, size_t len) {
    (void)data;  // Discard data - we're just a sink

    sink_upload_ctx_t* ctx = httpd_req_get_user_data(req);
    if (ctx) {
        ctx->bytes_received += len;
        ctx->chunk_count++;

        // Log progress every 1MB
        if (ctx->bytes_received % (1024 * 1024) < len) {
            ESP_LOGI(TAG, "Sink upload progress: %zu/%zu bytes (%d chunks)",
                     ctx->bytes_received, ctx->total_bytes, ctx->chunk_count);
        }
    }

    return HTTPD_OK;
}

// Done callback for sink upload
static void sink_done_callback(httpd_req_t* req, httpd_err_t err) {
    sink_upload_ctx_t* ctx = httpd_req_get_user_data(req);
    char json[256];

    if (ctx) {
        int64_t elapsed_us = esp_timer_get_time() - ctx->start_time;
        double elapsed_sec = elapsed_us / 1000000.0;
        double rate_mbps = (ctx->bytes_received * 8.0) / (elapsed_sec * 1000000.0);

        if (err == HTTPD_OK) {
            snprintf(json, sizeof(json),
                "{\"status\":\"success\",\"bytes\":%zu,\"chunks\":%d,"
                "\"elapsed_ms\":%lld,\"rate_mbps\":%.2f}",
                ctx->bytes_received, ctx->chunk_count,
                (long long)(elapsed_us / 1000), rate_mbps);
            httpd_resp_set_status(req, 200);
            ESP_LOGI(TAG, "Sink upload complete: %zu bytes in %lld ms (%.2f Mbps)",
                     ctx->bytes_received, (long long)(elapsed_us / 1000), rate_mbps);
        } else {
            snprintf(json, sizeof(json),
                "{\"status\":\"error\",\"code\":%d,\"bytes_before_error\":%zu}",
                err, ctx->bytes_received);
            httpd_resp_set_status(req, 500);
        }

        free(ctx);
    } else {
        snprintf(json, sizeof(json), "{\"status\":\"error\",\"message\":\"context lost\"}");
        httpd_resp_set_status(req, 500);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, strlen(json));
}

// POST /upload/sink - accepts large uploads, discards data (for concurrent test)
static httpd_err_t handle_upload_sink(httpd_req_t* req) {
    request_count++;

    // Allocate context to track progress
    sink_upload_ctx_t* ctx = calloc(1, sizeof(sink_upload_ctx_t));
    if (!ctx) {
        httpd_resp_send_error(req, 500, "Out of memory");
        return HTTPD_OK;
    }

    ctx->total_bytes = req->content_length;
    ctx->bytes_received = 0;
    ctx->chunk_count = 0;
    ctx->start_time = esp_timer_get_time();

    httpd_req_set_user_data(req, ctx);

    ESP_LOGI(TAG, "Starting sink upload (content_length=%zu)", req->content_length);

    // Use deferred mode - this allows other requests to be processed
    httpd_err_t err = httpd_req_defer(req, sink_body_callback, sink_done_callback);
    if (err != HTTPD_OK) {
        free(ctx);
        char json[64];
        snprintf(json, sizeof(json), "{\"status\":\"error\",\"code\":%d}", err);
        httpd_resp_set_status(req, 500);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, strlen(json));
    }

    return HTTPD_OK;
}

// GET /hello - simple hello world response for concurrent request testing
static httpd_err_t handle_hello(httpd_req_t* req) {
    request_count++;

    // Record timestamp to prove we responded quickly during upload
    int64_t now = esp_timer_get_time();

    char json[128];
    int len = snprintf(json, sizeof(json),
        "{\"message\":\"Hello, World!\",\"timestamp\":%lld,\"request_count\":%d}",
        (long long)now, request_count);

    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, json, len);

    return HTTPD_OK;
}

// GET /perf - minimal fixed response for performance testing
static httpd_err_t handle_perf(httpd_req_t* req) {
    // Minimal response - no dynamic content, no formatting
    static const char* response = "OK";
    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, 2);
    return HTTPD_OK;
}

// 404 handler - custom format matching test expectations
static httpd_err_t handle_404(httpd_req_t* req) {
    const char* url = httpd_req_get_uri(req);

    char response[512];
    int len = snprintf(response, sizeof(response),
             "<html><body>"
             "<h1>404 - Not Found</h1>"
             "<p>The requested URL %s was not found on this server.</p>"
             "</body></html>",
             url && *url ? url : "/");

    httpd_resp_set_status(req, 404);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_header(req, "Connection", "close");
    httpd_resp_send(req, response, len);

    return HTTPD_OK;
}

// Event handler for Ethernet events
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    esp_netif_t *eth_netif = (esp_netif_t *)arg;

    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

// Event handler for IP events
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

// Initialize OpenCores Ethernet for QEMU
// Based on ESP-IDF protocol_examples_common/eth_connect.c
static void init_ethernet(void) {
    ESP_LOGI(TAG, "Initializing Ethernet for QEMU...");

    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create new default instance of esp-netif for Ethernet
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Configure OpenCores MAC for QEMU
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    mac_config.rx_task_prio = 15;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create OpenCores MAC");
        return;
    }

    // Configure PHY for QEMU OpenCores
    // Key: Use short timeouts since QEMU PHY emulation responds quickly
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;  // Auto-detect PHY address (QEMU uses 1)
    phy_config.reset_gpio_num = -1;  // No reset GPIO in QEMU
    phy_config.autonego_timeout_ms = 100;  // Short timeout for QEMU

    // QEMU OpenCores emulates DP83848 PHY
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create PHY driver");
        return;
    }

    // Install Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t ret = esp_eth_driver_install(&eth_config, &eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        return;
    }

    // Attach Ethernet driver to network interface
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Start Ethernet driver
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return;
    }

    // Wait for link up and IP assignment
    ESP_LOGI(TAG, "Waiting for network initialization...");
    vTaskDelay(pdMS_TO_TICKS(3000)); // Give time for DHCP

    // Check if we got an IP address
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        // Set a static IP for QEMU
        // QEMU user-mode networking uses 10.0.2.0/24 subnet
        ESP_LOGI(TAG, "No DHCP response, setting static IP...");
        esp_netif_ip_info_t static_ip = {
            .ip.addr = ESP_IP4TOADDR(10, 0, 2, 15),
            .gw.addr = ESP_IP4TOADDR(10, 0, 2, 2),
            .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0)
        };

        esp_netif_dhcpc_stop(eth_netif);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &static_ip));
        ESP_LOGI(TAG, "Static IP configured: 10.0.2.15");
    }
}

// WebSocket echo handler
static httpd_err_t handle_ws_echo(httpd_ws_t* ws, httpd_ws_event_t* event) {
    switch (event->type) {
        case WS_EVENT_CONNECT:
            ESP_LOGI(TAG, "WebSocket connected");
            // Connection is accepted by returning HTTPD_OK
            const char* welcome = "{\"type\":\"welcome\",\"message\":\"Connected to echo server\"}";
            httpd_ws_send(ws, welcome, strlen(welcome), WS_TYPE_TEXT);
            break;

        case WS_EVENT_MESSAGE:
            ESP_LOGI(TAG, "WebSocket message: %zu bytes, frame_type: %d", event->len, event->frame_type);
            // Echo back with same frame type (text or binary)
            httpd_ws_send(ws, event->data, event->len, event->frame_type);
            break;

        case WS_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "WebSocket disconnected");
            break;

        case WS_EVENT_ERROR:
            ESP_LOGI(TAG, "WebSocket error");
            break;
    }

    return HTTPD_OK;
}

// WebSocket broadcast handler - broadcasts messages to all connected clients
static httpd_err_t handle_ws_broadcast(httpd_ws_t* ws, httpd_ws_event_t* event) {
    switch (event->type) {
        case WS_EVENT_CONNECT:
            ESP_LOGI(TAG, "Broadcast WebSocket connected");
            // Connection is accepted by returning HTTPD_OK
            // Notify about client joining (get count after accepting this connection)
            char join_msg[64];
            snprintf(join_msg, sizeof(join_msg), "{\"type\":\"client_joined\",\"total\":%u}",
                    httpd_ws_get_connection_count(server));
            httpd_ws_broadcast(server, "/ws/broadcast", join_msg, strlen(join_msg), WS_TYPE_TEXT);
            break;

        case WS_EVENT_MESSAGE:
            ESP_LOGI(TAG, "Broadcast message: %zu bytes", event->len);
            // Broadcast to all connected clients on this route
            httpd_ws_broadcast(server, "/ws/broadcast", event->data, event->len, WS_TYPE_TEXT);
            break;

        case WS_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Broadcast WebSocket disconnected");
            // Notify about client leaving (count will be reduced after this event)
            char leave_msg[64];
            unsigned int remaining = httpd_ws_get_connection_count(server);
            if (remaining > 0) remaining--;  // This connection hasn't been removed yet
            snprintf(leave_msg, sizeof(leave_msg), "{\"type\":\"client_left\",\"remaining\":%u}",
                    remaining);
            httpd_ws_broadcast(server, "/ws/broadcast", leave_msg, strlen(leave_msg), WS_TYPE_TEXT);
            break;

        case WS_EVENT_ERROR:
            ESP_LOGI(TAG, "Broadcast WebSocket error");
            break;
    }

    return HTTPD_OK;
}

// WebSocket channel handler - supports pub/sub channels
// Messages: {"cmd":"join","channel":"name"}, {"cmd":"leave","channel":"name"},
//           {"cmd":"publish","channel":"name","data":"..."}
static httpd_err_t handle_ws_channel(httpd_ws_t* ws, httpd_ws_event_t* event) {
    switch (event->type) {
        case WS_EVENT_CONNECT:
            ESP_LOGI(TAG, "Channel WebSocket connected");
            // Send welcome message
            const char* welcome = "{\"type\":\"welcome\",\"message\":\"Connected to channel server\"}";
            httpd_ws_send(ws, welcome, strlen(welcome), WS_TYPE_TEXT);
            break;

        case WS_EVENT_MESSAGE: {
            ESP_LOGI(TAG, "Channel message: %zu bytes", event->len);

            // Simple JSON parsing (production would use proper parser)
            char msg[256];
            size_t len = event->len < sizeof(msg) - 1 ? event->len : sizeof(msg) - 1;
            memcpy(msg, event->data, len);
            msg[len] = '\0';

            // Parse cmd field
            char* cmd = strstr(msg, "\"cmd\":\"");
            if (!cmd) {
                const char* err = "{\"type\":\"error\",\"message\":\"missing cmd\"}";
                httpd_ws_send(ws, err, strlen(err), WS_TYPE_TEXT);
                break;
            }
            cmd += 7;

            // Parse channel name
            char* channel = strstr(msg, "\"channel\":\"");
            char channel_name[32] = {0};
            if (channel) {
                channel += 11;
                char* end = strchr(channel, '"');
                if (end && (end - channel) < 32) {
                    memcpy(channel_name, channel, end - channel);
                }
            }

            if (strncmp(cmd, "join", 4) == 0) {
                if (channel_name[0]) {
                    httpd_err_t err = httpd_ws_join(ws, channel_name);
                    char resp[128];
                    if (err == HTTPD_OK) {
                        snprintf(resp, sizeof(resp), "{\"type\":\"joined\",\"channel\":\"%s\",\"size\":%u}",
                                channel_name, httpd_ws_channel_size(server, channel_name));
                    } else {
                        snprintf(resp, sizeof(resp), "{\"type\":\"error\",\"message\":\"join failed\",\"code\":%d}", err);
                    }
                    httpd_ws_send(ws, resp, strlen(resp), WS_TYPE_TEXT);
                }
            }
            else if (strncmp(cmd, "leave", 5) == 0) {
                if (channel_name[0]) {
                    httpd_err_t err = httpd_ws_leave(ws, channel_name);
                    char resp[128];
                    if (err == HTTPD_OK) {
                        snprintf(resp, sizeof(resp), "{\"type\":\"left\",\"channel\":\"%s\"}", channel_name);
                    } else {
                        snprintf(resp, sizeof(resp), "{\"type\":\"error\",\"message\":\"not in channel\"}");
                    }
                    httpd_ws_send(ws, resp, strlen(resp), WS_TYPE_TEXT);
                }
            }
            else if (strncmp(cmd, "publish", 7) == 0) {
                if (channel_name[0]) {
                    // Extract data field
                    char* data = strstr(msg, "\"data\":\"");
                    if (data) {
                        data += 8;
                        char* end = strchr(data, '"');
                        if (end) {
                            size_t data_len = end - data;
                            char pub_msg[256];
                            int pub_len = snprintf(pub_msg, sizeof(pub_msg),
                                "{\"type\":\"channel_message\",\"channel\":\"%s\",\"data\":\"%.*s\"}",
                                channel_name, (int)data_len, data);
                            int sent = httpd_ws_publish(server, channel_name, pub_msg, pub_len, WS_TYPE_TEXT);
                            char resp[64];
                            snprintf(resp, sizeof(resp), "{\"type\":\"published\",\"sent\":%d}", sent);
                            httpd_ws_send(ws, resp, strlen(resp), WS_TYPE_TEXT);
                        }
                    }
                }
            }
            else if (strncmp(cmd, "channels", 8) == 0) {
                // List subscribed channels
                const char* chs[8];
                unsigned int count = httpd_ws_get_channels(ws, chs, 8);
                char resp[256] = "{\"type\":\"channels\",\"channels\":[";
                size_t pos = strlen(resp);
                for (unsigned int i = 0; i < count && pos < sizeof(resp) - 20; i++) {
                    if (i > 0) resp[pos++] = ',';
                    pos += snprintf(resp + pos, sizeof(resp) - pos, "\"%s\"", chs[i]);
                }
                pos += snprintf(resp + pos, sizeof(resp) - pos, "],\"count\":%u}", count);
                httpd_ws_send(ws, resp, strlen(resp), WS_TYPE_TEXT);
            }
            else {
                const char* err = "{\"type\":\"error\",\"message\":\"unknown command\"}";
                httpd_ws_send(ws, err, strlen(err), WS_TYPE_TEXT);
            }
            break;
        }

        case WS_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Channel WebSocket disconnected");
            // Channels are auto-cleared on disconnect
            break;

        case WS_EVENT_ERROR:
            ESP_LOGI(TAG, "Channel WebSocket error");
            break;
    }

    return HTTPD_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting E2E Test Server");

    // Record server start time for uptime calculation
    server_start_time = esp_timer_get_time();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_ETH_USE_OPENETH
    ESP_LOGI(TAG, "OpenCores Ethernet enabled in config, attempting initialization...");
    init_ethernet();
    // If init_ethernet returns, it means it couldn't set up networking
    // but we'll continue anyway for testing
#else
    ESP_LOGI(TAG, "Running without Ethernet initialization (network stack bypass mode)");
    // For basic testing, we can run without network initialization
    // QEMU's port forwarding might work at a lower level
#endif

    // Start server on port 80
    ESP_LOGI(TAG, "Starting web server");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.port = 80;
    httpd_start(&server, &config);

    // Setup routes - order matters, more specific routes first
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/",
        .handler = handle_root
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_HEAD,
        .pattern = "/",
        .handler = handle_head_root
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/api/status",
        .handler = handle_api_status
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/api/data/*",
        .handler = handle_api_data_get
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_DELETE,
        .pattern = "/api/data/*",
        .handler = handle_api_data_delete
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/api/echo",
        .handler = handle_api_echo
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_PUT,
        .pattern = "/api/update",
        .handler = handle_api_update
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/headers",
        .handler = handle_headers
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/cors",
        .handler = handle_cors
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_OPTIONS,
        .pattern = "/cors",
        .handler = handle_cors_options
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/template",
        .handler = handle_template
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/static/*",
        .handler = handle_static
    });

    // Authentication test routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/auth/protected",
        .handler = handle_auth_protected
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/auth/protected",
        .handler = handle_auth_protected
    });

    // File upload test routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/upload",
        .handler = handle_upload
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/upload/continue",
        .handler = handle_upload_continue
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/upload/verify",
        .handler = handle_upload_verify
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_DELETE,
        .pattern = "/upload",
        .handler = handle_upload_delete
    });

    // Deferred (async) upload test routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/upload/defer",
        .handler = handle_upload_defer
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/upload/defer/custom",
        .handler = handle_upload_defer_custom
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/upload/defer/verify",
        .handler = handle_upload_defer_verify
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_DELETE,
        .pattern = "/upload/defer",
        .handler = handle_upload_defer_delete
    });

    // Concurrent request test routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "/upload/sink",
        .handler = handle_upload_sink
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/hello",
        .handler = handle_hello
    });

    // Performance test route - minimal response
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "/perf",
        .handler = handle_perf
    });

    // WebSocket routes
    httpd_register_ws_route(server, &(httpd_ws_route_t){
        .pattern = "/ws/echo",
        .handler = handle_ws_echo
    });

    httpd_register_ws_route(server, &(httpd_ws_route_t){
        .pattern = "/ws/broadcast",
        .handler = handle_ws_broadcast
    });

    httpd_register_ws_route(server, &(httpd_ws_route_t){
        .pattern = "/ws/channel",
        .handler = handle_ws_channel
    });

    // 404 handler - catch all for unmatched routes
    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_GET,
        .pattern = "*",
        .handler = handle_404
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_POST,
        .pattern = "*",
        .handler = handle_404
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_PUT,
        .pattern = "*",
        .handler = handle_404
    });

    httpd_register_route(server, &(httpd_route_t){
        .method = HTTP_DELETE,
        .pattern = "*",
        .handler = handle_404
    });

    ESP_LOGI(TAG, "E2E Server ready on port 80!");
    ESP_LOGI(TAG, "Endpoints: /, /api/status, /api/data/*, /api/echo, /api/update, /headers, /cors, /template, /static/*");
    ESP_LOGI(TAG, "Upload endpoints: /upload, /upload/defer, /upload/defer/custom, /upload/sink");
    ESP_LOGI(TAG, "Test endpoints: /hello, /perf");
    ESP_LOGI(TAG, "WebSocket endpoints: /ws/echo, /ws/broadcast, /ws/channel");

    // Keep running
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}