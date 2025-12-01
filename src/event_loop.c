#include "private/event_loop.h"
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_log.h"

static const char* TAG = "EVENT_LOOP";

// Default configuration
static const event_loop_config_t default_config = {
    .port = 80,
    .backlog = 5,
    .timeout_ms = 30000,        // 30 second timeout
    .select_timeout_ms = 1000,   // 1 second select timeout
    .io_buffer_size = 1024,      // 1KB I/O buffer
    .nodelay = true,             // Disable Nagle's algorithm
    .reuseaddr = true            // Allow address reuse
};

void event_loop_init_default(event_loop_t* loop, connection_pool_t* pool) {
    event_loop_init(loop, pool, &default_config);
}

void event_loop_init(event_loop_t* loop, connection_pool_t* pool, const event_loop_config_t* config) {
    memset(loop, 0, sizeof(event_loop_t));
    loop->pool = pool;
    loop->config = *config;
    loop->listen_fd = -1;
    loop->running = false;

    // Precompute timeout in ticks (avoid division in hot path)
    loop->timeout_ticks = config->timeout_ms / config->select_timeout_ms;

    // Precompute select timeout struct (avoid repeated struct construction)
    loop->select_timeout.tv_sec = config->select_timeout_ms / 1000;
    loop->select_timeout.tv_usec = (config->select_timeout_ms % 1000) * 1000;

    // Initialize connection pool
    connection_pool_init(pool);
}

int event_loop_create_listener(event_loop_t* loop) {
    struct sockaddr_in server_addr;
    int fd;

    // Create socket
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    // Set socket options
    if (loop->config.reuseaddr) {
        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            ESP_LOGW(TAG, "Failed to set SO_REUSEADDR: %s", strerror(errno));
        }
    }

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Failed to set non-blocking: %s", strerror(errno));
        close(fd);
        return -1;
    }

    // Bind to address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(loop->config.port);

    if (bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind to port %d: %s", loop->config.port, strerror(errno));
        close(fd);
        return -1;
    }

    // Start listening
    if (listen(fd, loop->config.backlog) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    loop->listen_fd = fd;
    ESP_LOGI(TAG, "Server listening on port %d", loop->config.port);
    return fd;
}

static void handle_new_connection(event_loop_t* loop, const event_handlers_t* handlers) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(loop->listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "Accept failed: %s", strerror(errno));
        }
        return;
    }

    // Set non-blocking (single syscall - we only need O_NONBLOCK)
    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "Failed to set client non-blocking: %s", strerror(errno));
        close(client_fd);
        return;
    }

    // Set TCP_NODELAY if configured
    if (loop->config.nodelay) {
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    // Find free connection slot using O(1) bit manipulation
    uint32_t free_mask = ~loop->pool->active_mask;
    if (free_mask == 0) {
        ESP_LOGW(TAG, "No free connection slots, rejecting connection");
        close(client_fd);
        return;
    }

    int slot = __builtin_ctz(free_mask);
    if (slot >= MAX_CONNECTIONS) {
        ESP_LOGW(TAG, "No free connection slots, rejecting connection");
        close(client_fd);
        return;
    }

    // Initialize connection using memset (faster than 18+ individual assignments)
    connection_t* conn = &loop->pool->connections[slot];
    memset(conn, 0, sizeof(connection_t));
    conn->fd = client_fd;               // Client socket fd
    conn->state = CONN_STATE_NEW;       // CONN_STATE_NEW = 1
    conn->pool_index = slot;            // Store index for O(1) context lookup
    conn->last_activity = loop->tick_count;

    connection_mark_active(loop->pool, slot);
    loop->total_connections++;

    ESP_LOGD(TAG, "New connection [%d] from %s:%d",
             slot, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    if (handlers->on_connect) {
        handlers->on_connect(conn);
    }
}

static void handle_connection_data(event_loop_t* loop, connection_t* conn,
                                  uint8_t* buffer, size_t buffer_size,
                                  const event_handlers_t* handlers) {
    ssize_t bytes = recv(conn->fd, buffer, buffer_size, 0);

    if (bytes <= 0) {
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; // No data available
        }
        // Connection closed or error
        // Call WebSocket disconnect handler if this was a WebSocket connection
        if (conn->state == CONN_STATE_WEBSOCKET && handlers->on_ws_disconnect) {
            handlers->on_ws_disconnect(conn);
        }
        conn->state = CONN_STATE_CLOSED;
        return;
    }

    conn->last_activity = loop->tick_count;

    // Process based on connection state
    switch (conn->state) {
        case CONN_STATE_NEW:
        case CONN_STATE_HTTP_HEADERS:
            if (handlers->on_http_request) {
                handlers->on_http_request(conn, buffer, bytes);
            }
            loop->total_requests++;
            break;

        case CONN_STATE_HTTP_BODY:
            if (handlers->on_http_body) {
                handlers->on_http_body(conn, buffer, bytes);
            }
            break;

        case CONN_STATE_WEBSOCKET:
            if (handlers->on_ws_frame) {
                handlers->on_ws_frame(conn, buffer, bytes);
            }
            loop->total_ws_frames++;
            break;

        default:
            break;
    }
}

void event_loop_check_timeouts(event_loop_t* loop) {
    // Use precomputed timeout_ticks (avoid division in hot path)
    uint32_t timeout_ticks = loop->timeout_ticks;

    // Only check non-WebSocket connections (bitmask arithmetic to skip WS)
    // This eliminates per-iteration branch for WebSocket check
    uint32_t mask = loop->pool->active_mask & ~loop->pool->ws_active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = &loop->pool->connections[i];

        // Check for timeout (WebSockets already excluded by bitmask)
        if (loop->tick_count - conn->last_activity > timeout_ticks) {
            ESP_LOGD(TAG, "Connection [%d] timed out", i);
            conn->state = CONN_STATE_CLOSED;
        }
    }
}

int event_loop_iteration(event_loop_t* loop, const event_handlers_t* handlers, uint8_t* io_buffer) {
    fd_set read_fds, write_fds;
    int max_fd = loop->listen_fd;
    int activity;
    const size_t buffer_size = loop->config.io_buffer_size;  // Cache config value
    connection_t* const base = loop->pool->connections;  // Cache base pointer for efficient indexing
    bool has_write_pending = (loop->pool->write_pending_mask != 0);

    // Initialize fd_sets
    FD_ZERO(&read_fds);
    if (has_write_pending) {
        FD_ZERO(&write_fds);
    }

    // Add listening socket
    FD_SET(loop->listen_fd, &read_fds);

    // Add active connections using bitmask iteration
    uint32_t mask = loop->pool->active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = base + i;  // Use cached base pointer

        // Handle closed connections
        if (conn->state == CONN_STATE_CLOSED) {
            if (handlers->on_disconnect) {
                handlers->on_disconnect(conn);
            }
            close(conn->fd);
            connection_mark_inactive(loop->pool, i);
            connection_mark_write_pending(loop->pool, i, false);  // Clear write pending
            ESP_LOGD(TAG, "Connection [%d] closed", i);
            continue;
        }

        FD_SET(conn->fd, &read_fds);

        // Monitor for write readiness if data pending
        if (has_write_pending && connection_has_write_pending(loop->pool, i)) {
            FD_SET(conn->fd, &write_fds);
        }

        if (conn->fd > max_fd) {
            max_fd = conn->fd;
        }
    }

    // Wait for activity (copy precomputed timeout - select may modify it)
    struct timeval timeout = loop->select_timeout;

    activity = select(max_fd + 1, &read_fds,
                     has_write_pending ? &write_fds : NULL,
                     NULL, &timeout);

    if (activity < 0) {
        if (errno != EINTR) {
            ESP_LOGE(TAG, "Select error: %s", strerror(errno));
        }
        return -1;
    }

    if (activity == 0) {
        // Timeout - check connection timeouts
        loop->tick_count++;
        event_loop_check_timeouts(loop);
        return 0;
    }

    // Handle new connections
    if (FD_ISSET(loop->listen_fd, &read_fds)) {
        handle_new_connection(loop, handlers);
    }

    // Handle writable connections first (drain pending data before reading more)
    if (has_write_pending && handlers->on_write_ready) {
        mask = loop->pool->write_pending_mask;
        while (mask) {
            int i = __builtin_ctz(mask);
            mask &= mask - 1;  // Clear lowest set bit

            connection_t* conn = base + i;
            if (FD_ISSET(conn->fd, &write_fds)) {
                handlers->on_write_ready(conn);
            }
        }
    }

    // Handle readable connections using bitmask iteration
    mask = loop->pool->active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = base + i;  // Reuse cached base pointer

        // Handle readable connections
        if (FD_ISSET(conn->fd, &read_fds)) {
            handle_connection_data(loop, conn, io_buffer, buffer_size, handlers);
        }
    }

    return activity;
}

void event_loop_run(event_loop_t* loop, const event_handlers_t* handlers) {
    // Allocate I/O buffer on heap (saves 1KB stack space for 4KB task compatibility)
    if (!loop->io_buffer) {
        loop->io_buffer = (uint8_t*)malloc(loop->config.io_buffer_size);
        if (!loop->io_buffer) {
            ESP_LOGE(TAG, "Failed to allocate I/O buffer");
            return;
        }
    }

    if (loop->listen_fd < 0) {
        if (event_loop_create_listener(loop) < 0) {
            ESP_LOGE(TAG, "Failed to create listener");
            return;
        }
    }

    loop->running = true;
    ESP_LOGI(TAG, "Event loop started");

    while (loop->running) {
        event_loop_iteration(loop, handlers, loop->io_buffer);
    }

    // Free I/O buffer when loop stops
    if (loop->io_buffer) {
        free(loop->io_buffer);
        loop->io_buffer = NULL;
    }

    ESP_LOGI(TAG, "Event loop stopped");
}

void event_loop_stop(event_loop_t* loop) {
    loop->running = false;
}

// Connection pool functions moved to connection.c to avoid duplication