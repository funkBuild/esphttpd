/**
 * @file raw_tcp.c
 * @brief lwIP raw TCP callback layer for zero-copy, zero-context-switch HTTP serving
 *
 * Replaces the BSD socket + select() event loop when CONFIG_HTTPD_USE_RAW_API is enabled.
 * Callbacks fire directly inside tcpip_thread, eliminating ~20-110us of socket layer overhead.
 */

#include "sdkconfig.h"

#ifdef CONFIG_HTTPD_USE_RAW_API

#include "private/raw_tcp.h"
#include "private/connection.h"
#include "private/event_loop.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "esp_log.h"
#include <string.h>

static const char TAG[] = "RAW_TCP";

// Store handlers and loop pointers for use in callbacks
static event_loop_t* s_loop = NULL;
static const event_handlers_t* s_handlers = NULL;

#ifdef CONFIG_ESPHTTPD_TEST_MODE
static raw_tcp_write_mock_t s_write_mock = NULL;

void raw_tcp_set_write_mock(raw_tcp_write_mock_t mock) {
    s_write_mock = mock;
}
#endif

// Forward declarations for callbacks
static err_t raw_accept_cb(void* arg, struct tcp_pcb* newpcb, err_t err);
static err_t raw_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err);
static err_t raw_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len);
static void raw_err_cb(void* arg, err_t err);
static err_t raw_poll_cb(void* arg, struct tcp_pcb* tpcb);

int raw_tcp_listen(event_loop_t* loop, const event_handlers_t* handlers) {
    struct tcp_pcb* pcb = tcp_new();
    if (!pcb) {
        ESP_LOGE(TAG, "Failed to create TCP PCB");
        return -1;
    }

    // Bind to configured port
    err_t err = tcp_bind(pcb, IP_ADDR_ANY, loop->config.port);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind to port %d: %d", loop->config.port, err);
        tcp_close(pcb);
        return -1;
    }

    // Start listening with backlog
    struct tcp_pcb* listen_pcb = tcp_listen_with_backlog(pcb, loop->config.backlog);
    if (!listen_pcb) {
        ESP_LOGE(TAG, "Failed to listen");
        tcp_close(pcb);
        return -1;
    }

    // Store context for accept callback
    s_loop = loop;
    s_handlers = handlers;

    tcp_arg(listen_pcb, loop);
    tcp_accept(listen_pcb, raw_accept_cb);

    loop->listen_pcb = listen_pcb;
    loop->running = true;

    ESP_LOGI(TAG, "Raw TCP server listening on port %d", loop->config.port);
    return 0;
}

static err_t raw_accept_cb(void* arg, struct tcp_pcb* newpcb, err_t err) {
    event_loop_t* loop = (event_loop_t*)arg;

    if (err != ERR_OK || !newpcb) {
        ESP_LOGE(TAG, "Accept error: %d", err);
        return ERR_VAL;
    }

    // Find free connection slot
    connection_t* conn = connection_alloc_slot(loop->pool);
    if (!conn) {
        ESP_LOGW(TAG, "No free connection slots, rejecting connection");
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    // Initialize raw TCP state
    conn->raw.pcb = newpcb;
    conn->raw.recv_chain = NULL;
    conn->raw.recv_offset = 0;
    conn->raw.unacked_bytes = 0;
    conn->raw.write_pending = false;
    conn->fd = -1;  // Not a socket

    // Disable Nagle's algorithm if configured
    if (loop->config.nodelay) {
        tcp_nagle_disable(newpcb);
    }

    // Register per-connection callbacks with conn as arg
    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, raw_recv_cb);
    tcp_sent(newpcb, raw_sent_cb);
    tcp_err(newpcb, raw_err_cb);
    tcp_poll(newpcb, raw_poll_cb, CONFIG_HTTPD_RAW_POLL_INTERVAL);

    loop->total_connections++;
    conn->last_activity = loop->tick_count;

    ESP_LOGD(TAG, "New connection [%d]", conn->pool_index);

    if (s_handlers->on_connect) {
        s_handlers->on_connect(conn);
    }

    return ERR_OK;
}

static err_t raw_recv_cb(void* arg, struct tcp_pcb* tpcb, struct pbuf* p, err_t err) {
    connection_t* conn = (connection_t*)arg;

    if (!conn || !s_loop) {
        if (p) pbuf_free(p);
        return ERR_ARG;
    }

    // NULL pbuf means connection closed by remote
    if (!p) {
        // Call WebSocket disconnect handler if applicable
        if ((conn->state == CONN_STATE_WEBSOCKET || conn->state == CONN_STATE_WS_CLOSING)
            && s_handlers->on_ws_disconnect) {
            s_handlers->on_ws_disconnect(conn);
        }
        conn->state = CONN_STATE_CLOSED;
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        conn->state = CONN_STATE_CLOSED;
        return ERR_OK;
    }

    conn->last_activity = s_loop->tick_count;

    // Linearize pbuf for the parser (most HTTP requests are a single segment)
    // For single-segment pbufs, use the payload directly (zero-copy)
    uint8_t* buffer;
    bool needs_free = false;

    if (p->next == NULL) {
        // Single segment - zero copy
        buffer = (uint8_t*)p->payload;
    } else {
        // Multi-segment - linearize into contiguous buffer
        buffer = (uint8_t*)malloc(p->tot_len);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate linearize buffer (%d bytes)", p->tot_len);
            pbuf_free(p);
            conn->state = CONN_STATE_CLOSED;
            return ERR_MEM;
        }
        pbuf_copy_partial(p, buffer, p->tot_len, 0);
        needs_free = true;
    }

    size_t total_len = p->tot_len;

    // Dispatch to appropriate handler based on connection state
    switch (conn->state) {
        case CONN_STATE_NEW:
        case CONN_STATE_HTTP_HEADERS:
            if (s_handlers->on_http_request) {
                s_handlers->on_http_request(conn, buffer, total_len);
            }
            s_loop->total_requests++;
            break;

        case CONN_STATE_HTTP_BODY:
            if (s_handlers->on_http_body) {
                s_handlers->on_http_body(conn, buffer, total_len);
            }
            break;

        case CONN_STATE_WEBSOCKET:
            if (s_handlers->on_ws_frame) {
                s_handlers->on_ws_frame(conn, buffer, total_len);
            }
            s_loop->total_ws_frames++;
            break;

        case CONN_STATE_WS_CLOSING:
            if (s_handlers->on_ws_frame) {
                s_handlers->on_ws_frame(conn, buffer, total_len);
            }
            break;

        default:
            break;
    }

    if (needs_free) {
        free(buffer);
    }

    // Acknowledge received data to lwIP (skip if connection is closing/closed)
    if (conn->state != CONN_STATE_CLOSING && conn->state != CONN_STATE_CLOSED) {
        tcp_recved(tpcb, total_len);
    }
    pbuf_free(p);

    return ERR_OK;
}

static err_t raw_sent_cb(void* arg, struct tcp_pcb* tpcb, u16_t len) {
    connection_t* conn = (connection_t*)arg;
    if (!conn) return ERR_ARG;

    // Decrement unacked bytes
    if (conn->raw.unacked_bytes >= len) {
        conn->raw.unacked_bytes -= len;
    } else {
        conn->raw.unacked_bytes = 0;
    }

    conn->raw.write_pending = false;

    // Notify upper layer that send buffer space is available
    if (s_handlers->on_write_ready) {
        s_handlers->on_write_ready(conn);
    }

    return ERR_OK;
}

static void raw_err_cb(void* arg, err_t err) {
    connection_t* conn = (connection_t*)arg;
    if (!conn) return;

    ESP_LOGD(TAG, "Connection [%d] error: %d", conn->pool_index, err);

    // lwIP has already freed the pcb when err_cb is called
    conn->raw.pcb = NULL;

    // Guard against double-disconnect (err_cb + poll_cb race)
    if (conn->state == CONN_STATE_CLOSED || conn->state == CONN_STATE_FREE) {
        return;
    }

    conn->state = CONN_STATE_CLOSED;

    // Call disconnect handler
    if (s_handlers->on_disconnect) {
        s_handlers->on_disconnect(conn);
    }

    // Clean up from pool
    if (s_loop && s_loop->pool) {
        connection_mark_inactive(s_loop->pool, conn->pool_index);
        connection_mark_write_pending(s_loop->pool, conn->pool_index, false);
        connection_mark_ws_inactive(s_loop->pool, conn->pool_index);
    }
}

// WS close handshake timeout: ~10 seconds (5 polls at 2s interval)
#define RAW_WS_CLOSE_TIMEOUT_POLLS 5

static err_t raw_poll_cb(void* arg, struct tcp_pcb* tpcb) {
    connection_t* conn = (connection_t*)arg;
    if (!conn || !s_loop) return ERR_OK;

    s_loop->tick_count++;

    // Check for connection timeout
    uint32_t timeout_ticks = s_loop->timeout_ticks;

    // Use shorter timeout for WebSocket close handshake
    if (conn->state == CONN_STATE_WS_CLOSING) {
        timeout_ticks = RAW_WS_CLOSE_TIMEOUT_POLLS;
    }

    // Skip timeout for active WebSocket connections
    if (conn->state == CONN_STATE_WEBSOCKET) {
        return ERR_OK;
    }

    if (s_loop->tick_count - conn->last_activity > timeout_ticks) {
        // Guard against double-disconnect (err_cb may have already handled this)
        if (conn->state == CONN_STATE_CLOSED || conn->state == CONN_STATE_FREE) {
            return ERR_OK;
        }

        ESP_LOGD(TAG, "Connection [%d] timed out%s", conn->pool_index,
                 conn->state == CONN_STATE_WS_CLOSING ? " (ws close handshake)" : "");
        conn->state = CONN_STATE_CLOSED;

        // Handle disconnect
        if (s_handlers->on_disconnect) {
            s_handlers->on_disconnect(conn);
        }

        raw_tcp_close(conn);
        if (s_loop->pool) {
            connection_mark_inactive(s_loop->pool, conn->pool_index);
            connection_mark_write_pending(s_loop->pool, conn->pool_index, false);
            connection_mark_ws_inactive(s_loop->pool, conn->pool_index);
        }
    }

    return ERR_OK;
}

ssize_t raw_tcp_write(connection_t* conn, const void* data, size_t len, bool more) {
    if (!conn || !data || len == 0) return 0;

#ifdef CONFIG_ESPHTTPD_TEST_MODE
    if (s_write_mock) {
        return s_write_mock(conn, data, len, more);
    }
#endif

    struct tcp_pcb* pcb = conn->raw.pcb;
    if (!pcb) return -1;

    // Check available send buffer
    size_t sndbuf = tcp_sndbuf(pcb);
    if (sndbuf == 0) return 0;  // No space available

    // Limit to available space
    size_t to_write = (len <= sndbuf) ? len : sndbuf;

    uint8_t flags = TCP_WRITE_FLAG_COPY;
    if (more) {
        flags |= TCP_WRITE_FLAG_MORE;
    }

    err_t err = tcp_write(pcb, data, to_write, flags);
    if (err != ERR_OK) {
        if (err == ERR_MEM) {
            return 0;  // No memory, try again later
        }
        ESP_LOGE(TAG, "tcp_write error: %d", err);
        return -1;
    }

    conn->raw.unacked_bytes += to_write;
    conn->raw.write_pending = true;

    // Flush immediately unless MSG_MORE equivalent
    if (!more) {
        tcp_output(pcb);
    }

    return (ssize_t)to_write;
}

size_t raw_tcp_sndbuf(connection_t* conn) {
    if (!conn || !conn->raw.pcb) return 0;
    return tcp_sndbuf(conn->raw.pcb);
}

void raw_tcp_output(connection_t* conn) {
    if (conn && conn->raw.pcb) {
        tcp_output(conn->raw.pcb);
    }
}

void raw_tcp_close(connection_t* conn) {
    if (!conn) return;

    struct tcp_pcb* pcb = conn->raw.pcb;
    if (!pcb) return;

    // Clear all callbacks first
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_poll(pcb, NULL, 0);

    // Free any pending recv chain
    if (conn->raw.recv_chain) {
        pbuf_free(conn->raw.recv_chain);
        conn->raw.recv_chain = NULL;
    }

    // Try graceful close first, abort if it fails
    err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        ESP_LOGW(TAG, "tcp_close failed (%d), aborting", err);
        tcp_abort(pcb);
    }

    conn->raw.pcb = NULL;
    conn->raw.recv_offset = 0;
    conn->raw.unacked_bytes = 0;
    conn->raw.write_pending = false;
}

void raw_tcp_recved(connection_t* conn, uint16_t len) {
    if (conn && conn->raw.pcb) {
        tcp_recved(conn->raw.pcb, len);
    }
}

void raw_tcp_stop(event_loop_t* loop) {
    if (!loop) return;

    // Close listen PCB
    if (loop->listen_pcb) {
        tcp_close(loop->listen_pcb);
        loop->listen_pcb = NULL;
    }

    // Close all active connections
    if (loop->pool) {
        uint32_t mask = loop->pool->active_mask;
        while (mask) {
            int i = __builtin_ctz(mask);
            mask &= mask - 1;

            connection_t* conn = &loop->pool->connections[i];
            raw_tcp_close(conn);
            connection_mark_inactive(loop->pool, i);
            connection_mark_write_pending(loop->pool, i, false);
            connection_mark_ws_inactive(loop->pool, i);
        }
    }

    loop->running = false;
    s_loop = NULL;
    s_handlers = NULL;
}

#endif // CONFIG_HTTPD_USE_RAW_API
