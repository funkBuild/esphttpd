#include "private/event_loop.h"
#ifndef CONFIG_HTTPD_USE_RAW_API
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private/websocket.h"

static const char TAG[] __attribute__((unused)) = "EVENT_LOOP";

// RX I/O buffer size from Kconfig (CONFIG_HTTPD_RECV_BUFFER_SIZE) with a
// 1024 fallback for builds without the symbol.
#ifdef CONFIG_HTTPD_RECV_BUFFER_SIZE
#define EVENT_LOOP_IO_BUFFER_SIZE CONFIG_HTTPD_RECV_BUFFER_SIZE
#else
#define EVENT_LOOP_IO_BUFFER_SIZE 1024
#endif

// Default configuration
static const event_loop_config_t default_config = {
    .port = 80,
    .backlog = 5,
    .timeout_ms = 30000,        // 30 second timeout
#ifndef CONFIG_HTTPD_USE_RAW_API
    .select_timeout_ms = 1000,   // 1 second select timeout
    .io_buffer_size = EVENT_LOOP_IO_BUFFER_SIZE,  // RX I/O buffer
#endif
    .ws_close_timeout_ms = 5000, // 5 second WS close handshake timeout
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
    // Validate configuration BEFORE deriving tick budgets from it: computing
    // from the raw input meant timeout_ms==0 yielded timeout_ticks==0 and
    // connections were closed after ~1 tick instead of the 30 s default.
    if (loop->config.timeout_ms == 0) loop->config.timeout_ms = 30000;
    if (loop->config.backlog == 0) loop->config.backlog = 5;
    if (loop->config.ws_close_timeout_ms == 0) loop->config.ws_close_timeout_ms = 5000;
    if (loop->config.header_timeout_ms == 0) loop->config.header_timeout_ms = 10000;
    if (loop->config.body_timeout_ms == 0) loop->config.body_timeout_ms = 10000;
    if (loop->config.body_min_bytes == 0) loop->config.body_min_bytes = 4096;
#ifdef CONFIG_HTTPD_USE_RAW_API
    loop->listen_pcb = NULL;
    // Compute timeout in poll intervals (each poll = CONFIG_HTTPD_RAW_POLL_INTERVAL * 500ms)
    uint32_t poll_interval_ms = CONFIG_HTTPD_RAW_POLL_INTERVAL * 500;
    uint32_t divisor = poll_interval_ms > 0 ? poll_interval_ms : 1000;
    loop->timeout_ticks = loop->config.timeout_ms / divisor;
    loop->ws_close_timeout_ticks = loop->config.ws_close_timeout_ms / divisor;
    loop->header_timeout_ticks = loop->config.header_timeout_ms / divisor;
    if (loop->header_timeout_ticks == 0) loop->header_timeout_ticks = 1;
    loop->body_timeout_ticks = loop->config.body_timeout_ms / divisor;
    if (loop->body_timeout_ticks == 0) loop->body_timeout_ticks = 1;
#else
    loop->listen_fd = -1;
    if (loop->config.select_timeout_ms == 0) loop->config.select_timeout_ms = 1000;
    if (loop->config.io_buffer_size == 0) loop->config.io_buffer_size = EVENT_LOOP_IO_BUFFER_SIZE;
    // Precompute timeout in ticks (avoid division in hot path); floor of 1
    // tick so a coarse select interval cannot zero out a timeout entirely
    loop->timeout_ticks = loop->config.timeout_ms / loop->config.select_timeout_ms;
    if (loop->timeout_ticks == 0) loop->timeout_ticks = 1;
    loop->ws_close_timeout_ticks = loop->config.ws_close_timeout_ms / loop->config.select_timeout_ms;
    if (loop->ws_close_timeout_ticks == 0) loop->ws_close_timeout_ticks = 1;
    loop->header_timeout_ticks = loop->config.header_timeout_ms / loop->config.select_timeout_ms;
    if (loop->header_timeout_ticks == 0) loop->header_timeout_ticks = 1;
    loop->body_timeout_ticks = loop->config.body_timeout_ms / loop->config.select_timeout_ms;
    if (loop->body_timeout_ticks == 0) loop->body_timeout_ticks = 1;
    // Precompute select timeout struct (avoid repeated struct construction)
    loop->select_timeout.tv_sec = loop->config.select_timeout_ms / 1000;
    loop->select_timeout.tv_usec = (loop->config.select_timeout_ms % 1000) * 1000;
#endif
    loop->running = false;

    // Initialize connection pool
    connection_pool_init(pool);
}

void event_loop_stop(event_loop_t* loop) {
    loop->stop_requested = true;
    loop->running = false;
    event_loop_wake(loop);
}

#ifdef CONFIG_HTTPD_USE_RAW_API
void event_loop_wake(event_loop_t* loop) {
    (void)loop;  // raw API: driven by lwIP callbacks, nothing to wake
}
#else
// ============================================================================
// Wake pair: a loopback UDP socket connected to itself. A byte sent to it
// makes the loop's select() return so it rebuilds its fd sets.
//
// Process-lifetime, created once by the loop task and never closed: a sender
// on another task reads s_wake_fd without a lock, so closing it at loop exit
// could let that sender write into an unrelated socket that reused the fd
// number. One socket serves every (sequential) server instance.
// ============================================================================
static atomic_int s_wake_fd = -1;
static atomic_bool s_wake_pending;             // a wake byte is in flight
static TaskHandle_t volatile s_wake_loop_task; // task currently running the loop

static void wake_pair_open(void) {
    if (atomic_load(&s_wake_fd) >= 0) return;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ESP_LOGW(TAG, "Wake socket unavailable (%s): cross-task wakeups fall back "
                 "to the select timeout", strerror(errno));
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    socklen_t addr_len = sizeof(addr);
    if (fd >= FD_SETSIZE ||
        bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        getsockname(fd, (struct sockaddr*)&addr, &addr_len) < 0 ||
        connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "Wake socket setup failed (%s): cross-task wakeups fall back "
                 "to the select timeout", strerror(errno));
        close(fd);
        return;
    }
    atomic_store(&s_wake_fd, fd);
}

void event_loop_wake(event_loop_t* loop) {
    (void)loop;
    int fd = atomic_load(&s_wake_fd);
    if (fd < 0) return;
    // The loop task rebuilds its fd sets before every select: nothing to do
    if (xTaskGetCurrentTaskHandle() == s_wake_loop_task) return;
    // Coalesce: one byte in flight is enough to break the current select
    if (atomic_exchange(&s_wake_pending, true)) return;
    static const uint8_t b = 0;
    if (send(fd, &b, 1, MSG_DONTWAIT) < 0) {
        atomic_store(&s_wake_pending, false);  // allow a later retry
    }
}

// Consume wake bytes. Clear the pending flag FIRST: a producer that changed
// state before waking is covered by the fd-set rebuild that follows this
// drain; one that wakes after the clear sends a fresh byte that breaks the
// next select.
static void wake_pair_drain(int fd) {
    atomic_store(&s_wake_pending, false);
    uint8_t buf[16];
    while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }
}
#endif

// ============================================================================
// Socket-based event loop (select mode)
// ============================================================================
#ifndef CONFIG_HTTPD_USE_RAW_API

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

    // Reject listen fd that would overflow fd_set
    if (fd >= FD_SETSIZE) {
        ESP_LOGE(TAG, "Listen fd %d >= FD_SETSIZE (%d)", fd, FD_SETSIZE);
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

    // Reject fds that would overflow fd_set (FD_SET with fd >= FD_SETSIZE is UB)
    if (client_fd >= FD_SETSIZE) {
        ESP_LOGE(TAG, "Accepted fd %d >= FD_SETSIZE (%d), rejecting", client_fd, FD_SETSIZE);
        close(client_fd);
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
    uint32_t free_mask = ~connection_mask_load(&loop->pool->active_mask);
    free_mask &= (MAX_CONNECTIONS < 32) ? ((1U << (MAX_CONNECTIONS & 31)) - 1U) : 0xFFFFFFFFU;
    if (free_mask == 0) {
        // Pool full: reclaim the longest-idle keep-alive connection rather
        // than refusing the newcomer. Otherwise a handful of clients that
        // hold idle keep-alive sockets (or trickle a request every idle
        // period) lock every other client out of the UI/API.
        int victim = event_loop_evict_idle(loop, handlers);
        if (victim < 0) {
            ESP_LOGW(TAG, "No free connection slots, rejecting connection");
            close(client_fd);
            return;
        }
        free_mask = 1U << victim;
    }

    int slot = __builtin_ctz(free_mask);

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
        if ((conn->state == CONN_STATE_WEBSOCKET || conn->state == CONN_STATE_WS_CLOSING)
            && handlers->on_ws_disconnect) {
            handlers->on_ws_disconnect(conn);
        }
        // Mark closed immediately so next select() doesn't monitor this fd
        conn->state = CONN_STATE_CLOSED;
        if (handlers->on_disconnect) {
            handlers->on_disconnect(conn);
        }
        close(conn->fd);
        connection_mark_inactive(loop->pool, conn->pool_index);
        connection_mark_write_pending(loop->pool, conn->pool_index, false);
        connection_mark_ws_inactive(loop->pool, conn->pool_index);
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
            // Only count new requests, not continuation segments
            if (conn->state != CONN_STATE_HTTP_HEADERS) {
                loop->total_requests++;
            }
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

        case CONN_STATE_WS_CLOSING:
            // Still reading frames while waiting for client's close ack
            if (handlers->on_ws_frame) {
                handlers->on_ws_frame(conn, buffer, bytes);
            }
            break;

        default:
            break;
    }
}

// Pick the connection that has been idle longest between keep-alive requests
// and close it to make room. Only a connection with nothing in progress is a
// candidate: re-armed for its next request (CONN_STATE_NEW) with no partial
// headers, no pending response bytes, and not a WebSocket or deferred upload.
// Returns the freed slot, or -1 if nothing is evictable.
int event_loop_evict_idle(event_loop_t* loop, const event_handlers_t* handlers) {
    int victim = -1;
    uint32_t oldest_age = 0;
    uint32_t mask = connection_mask_load(&loop->pool->active_mask);
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;
        connection_t* conn = &loop->pool->connections[i];
        if (conn->state != CONN_STATE_NEW || conn->header_pending ||
            conn->deferred || conn->continuation ||
            connection_has_write_pending(loop->pool, i) ||
            connection_is_ws_active(loop->pool, i)) {
            continue;
        }
        uint32_t age = loop->tick_count - conn->last_activity;
        if (victim < 0 || age > oldest_age) {
            victim = i;
            oldest_age = age;
        }
    }
    if (victim < 0) return -1;

    connection_t* conn = &loop->pool->connections[victim];
    ESP_LOGD(TAG, "Pool full: evicting idle keep-alive connection [%d]", victim);
    conn->state = CONN_STATE_CLOSED;
    if (handlers && handlers->on_disconnect) {
        handlers->on_disconnect(conn);
    }
    close(conn->fd);
    connection_mark_inactive(loop->pool, victim);
    connection_mark_write_pending(loop->pool, victim, false);
    connection_mark_ws_inactive(loop->pool, victim);
    return victim;
}

// Slow-body deadline. The idle timeout is refreshed by every received byte and
// the header deadline ends at the blank line, so a client that declared a huge
// Content-Length and then sent one body byte per idle period held its slot
// forever (and a mid-request connection is never evicted): sixteen of them
// locked every client out. While body bytes are outstanding, each
// body_timeout_ticks window must deliver body_min_bytes (or the whole
// remainder, if smaller); a window that falls short closes the connection.
// This is a minimum rate, not a total deadline: a large upload that keeps
// progressing is never cut off. Ticks during which the server itself was not
// polling (a handler blocking the loop, e.g. a flash erase in an upload
// handler) are excused so a server-side stall is not blamed on the client.
// No 408 is sent: the handler may already have answered this request.
static bool body_progress_stalled(event_loop_t* loop, connection_t* conn,
                                  uint32_t excused_ticks) {
    if (conn->state != CONN_STATE_HTTP_BODY ||
        conn->bytes_received >= conn->content_length) {
        return false;  // no body outstanding (e.g. response still streaming)
    }
    conn->body_window_start += (uint16_t)excused_ticks;
    uint16_t elapsed = (uint16_t)((uint16_t)loop->tick_count - conn->body_window_start);
    if (elapsed < loop->body_timeout_ticks) {
        return false;
    }
    // Bytes still outstanding when this window opened
    uint32_t owed = conn->content_length - conn->bytes_received + conn->body_window_bytes;
    uint32_t need = loop->config.body_min_bytes;
    if (owed < need) need = owed;
    if (conn->body_window_bytes < need) {
        return true;
    }
    connection_arm_body_window(conn, loop->tick_count);
    return false;
}

void event_loop_check_timeouts(event_loop_t* loop) {
    // Use precomputed timeout_ticks (avoid division in hot path)
    uint32_t timeout_ticks = loop->timeout_ticks;
    // Ticks beyond one since the previous scan elapsed while the loop was not
    // running (blocked in a handler): see body_progress_stalled()
    uint32_t scan_gap = loop->tick_count - loop->last_check_tick;
    uint32_t excused_ticks = (scan_gap > 1) ? scan_gap - 1 : 0;
    loop->last_check_tick = loop->tick_count;

    uint32_t mask = loop->pool->active_mask;
    while (mask) {
        int i = __builtin_ctz(mask);
        mask &= mask - 1;  // Clear lowest set bit

        connection_t* conn = &loop->pool->connections[i];

        if (conn->state == CONN_STATE_WEBSOCKET &&
            conn->ws_ping_interval_ticks > 0 &&
            loop->tick_count - conn->ws_last_ping >= conn->ws_ping_interval_ticks) {
            if (ws_send_ping(conn, NULL, 0) < 0) {
                conn->state = CONN_STATE_CLOSED;
                continue;
            }
            conn->ws_last_ping = loop->tick_count;
        }

        // A paused deferred upload is in an app-controlled state: its read fd
        // is deliberately excluded from the select set (back-pressure), so
        // last_activity — refreshed only on recv — inevitably goes stale.
        // Refresh it here so the pause cannot idle the connection out; the
        // app is responsible for resuming or closing. Tradeoff: a client that
        // dies while paused is only reaped after resume + a full idle timeout.
        if (conn->defer_paused) {
            conn->last_activity = loop->tick_count;
            connection_arm_body_window(conn, loop->tick_count);
            continue;
        }

        if (body_progress_stalled(loop, conn, excused_ticks)) {
            ESP_LOGD(TAG, "Connection [%d] request body stalled at %" PRIu32 "/%" PRIu32
                     " bytes", i, conn->bytes_received, conn->content_length);
            conn->state = CONN_STATE_CLOSED;
            continue;
        }

        // Header-completion deadline (slowloris): the idle timeout alone is
        // refreshed by every received byte, so a client trickling one header
        // byte per idle period held its slot forever. A request whose headers
        // are not complete within header_timeout_ticks of its first byte gets
        // 408 (RFC 9110 15.5.9) and is closed.
        if (conn->header_pending &&
            (conn->state == CONN_STATE_NEW || conn->state == CONN_STATE_HTTP_HEADERS) &&
            (uint16_t)((uint16_t)loop->tick_count - conn->request_start) >
                loop->header_timeout_ticks) {
            ESP_LOGD(TAG, "Connection [%d] request headers timed out", i);
            // Best effort, and only when no earlier response bytes are still
            // queued (the 408 must not overtake them)
            if (!connection_has_write_pending(loop->pool, i)) {
                static const char resp_408[] =
                    "HTTP/1.1 408 Request Timeout\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 0\r\n\r\n";
                (void)send(conn->fd, resp_408, sizeof(resp_408) - 1, MSG_DONTWAIT);
            }
            conn->state = CONN_STATE_CLOSED;
            continue;
        }

        // Use shorter timeout for WebSocket close handshake (RFC 6455)
        uint32_t effective_timeout = (conn->state == CONN_STATE_WS_CLOSING)
            ? loop->ws_close_timeout_ticks : timeout_ticks;

        if (loop->tick_count - conn->last_activity > effective_timeout) {
            ESP_LOGD(TAG, "Connection [%d] timed out%s", i,
                     conn->state == CONN_STATE_WS_CLOSING ? " (ws close handshake)" : "");
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
    bool has_write_pending = (connection_mask_load(&loop->pool->write_pending_mask) != 0);

    // Initialize fd_sets
    FD_ZERO(&read_fds);
    if (has_write_pending) {
        FD_ZERO(&write_fds);
    }

    // Add listening socket
    FD_SET(loop->listen_fd, &read_fds);

    // Add the wake socket (cross-task producers break select() through it)
    const int wake_fd = atomic_load(&s_wake_fd);
    if (wake_fd >= 0) {
        FD_SET(wake_fd, &read_fds);
        if (wake_fd > max_fd) max_fd = wake_fd;
    }

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
            connection_mark_write_pending(loop->pool, i, false);
            connection_mark_ws_inactive(loop->pool, i);
            ESP_LOGD(TAG, "Connection [%d] closed", i);
            continue;
        }

        // Back-pressure a paused deferred upload: omit its read fd from the
        // select set so we stop draining the socket. lwIP's receive window
        // then closes and the client is throttled instead of us pulling body
        // bytes off the socket and discarding them. The connection stays in
        // active_mask, so its writes and timeouts are still serviced below.
        // Note: httpd_req_defer_resume() from another task clears
        // defer_paused and wakes the loop (event_loop_wake), so the fd set
        // rebuilt here re-arms the read side promptly. While paused, the
        // timeout scan refreshes last_activity so the connection cannot
        // idle out.
        if (!conn->defer_paused) {
            FD_SET(conn->fd, &read_fds);
        }

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

    // Advance the timeout clock from real elapsed time, not per select()
    // return: under load select() returns once per packet, which previously
    // advanced the "clock" by thousands of ticks per second and closed idle
    // keep-alive connections long before their configured timeout. Checking
    // on every iteration (not only idle ones) still catches slow-loris.
    {
        int64_t now_us = esp_timer_get_time();
        int64_t tick_us = (int64_t)loop->config.select_timeout_ms * 1000;
        if (loop->last_tick_us == 0) {
            loop->last_tick_us = now_us;
        }
        bool tick_advanced = false;
        while (now_us - loop->last_tick_us >= tick_us) {
            loop->last_tick_us += tick_us;
            loop->tick_count++;
            tick_advanced = true;
        }
        if (tick_advanced) {
            event_loop_check_timeouts(loop);
        }
    }

    if (activity == 0) {
        return 0;
    }

    if (wake_fd >= 0 && FD_ISSET(wake_fd, &read_fds)) {
        wake_pair_drain(wake_fd);
    }

    // Handle new connections
    if (FD_ISSET(loop->listen_fd, &read_fds)) {
        handle_new_connection(loop, handlers);
    }

    // Handle writable connections first (drain pending data before reading more)
    if (has_write_pending && handlers->on_write_ready) {
        mask = connection_mask_load(&loop->pool->write_pending_mask);
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
    // Allocate I/O buffer on heap (saves 1KB stack space for 4KB task compatibility).
    // Deliberate exception to the malloc-goes-to-SPIRAM default: this is the
    // hottest RX buffer (every inbound byte crosses it), so prefer internal
    // DRAM. Fall back to plain malloc if internal DRAM is unavailable — under
    // QEMU there is no SPIRAM, so both paths land in the same heap anyway.
    if (!loop->io_buffer) {
        loop->io_buffer = (uint8_t*)heap_caps_malloc(loop->config.io_buffer_size,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!loop->io_buffer) {
            loop->io_buffer = (uint8_t*)malloc(loop->config.io_buffer_size);
        }
        if (!loop->io_buffer) {
            ESP_LOGE(TAG, "Failed to allocate I/O buffer");
            return;
        }
    }

    if (loop->listen_fd < 0) {
        if (event_loop_create_listener(loop) < 0) {
            ESP_LOGE(TAG, "Failed to create listener");
            goto cleanup;
        }
    }

    wake_pair_open();
    s_wake_loop_task = xTaskGetCurrentTaskHandle();

    loop->running = true;
    ESP_LOGI(TAG, "Event loop started");

    while (loop->running && !loop->stop_requested) {
        event_loop_iteration(loop, handlers, loop->io_buffer);
    }

cleanup:
    s_wake_loop_task = NULL;
    loop->running = false;
    // Close listening socket
    if (loop->listen_fd >= 0) {
        close(loop->listen_fd);
        loop->listen_fd = -1;
    }

    // Free I/O buffer when loop stops
    if (loop->io_buffer) {
        free(loop->io_buffer);
        loop->io_buffer = NULL;
    }

    ESP_LOGI(TAG, "Event loop stopped");
}

#endif // !CONFIG_HTTPD_USE_RAW_API

// Connection pool functions moved to connection.c to avoid duplication
