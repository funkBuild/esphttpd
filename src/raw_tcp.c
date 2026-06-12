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
#include "esp_timer.h"
#include <string.h>

#if !defined(CONFIG_LWIP_TCPIP_CORE_LOCKING) && !defined(CONFIG_ESPHTTPD_TEST_MODE)
#error "CONFIG_HTTPD_USE_RAW_API requires CONFIG_LWIP_TCPIP_CORE_LOCKING: without it LOCK_TCPIP_CORE() is a no-op and cross-task WebSocket sends race tcpip_thread"
#endif

static const char TAG[] = "RAW_TCP";

// Store handlers and loop pointers for use in callbacks
// Volatile: written in raw_tcp_listen/raw_tcp_stop, read from lwIP callbacks
static event_loop_t* volatile s_loop = NULL;
static const event_handlers_t* volatile s_handlers = NULL;

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

// Idempotent connection teardown, callable from every close path: remote FIN
// (recv NULL pbuf), error callback, poll timeout, and handler-initiated
// CONN_STATE_CLOSED (which previously had NO finalizer in raw mode - the pcb
// and pool slot leaked permanently). The pool's active bit doubles as the
// "already finalized" flag.
// Returns ERR_ABRT if the pcb had to be aborted (callers inside lwIP
// callbacks must propagate this so lwIP doesn't touch the freed pcb).
static err_t raw_finalize(connection_t* conn) {
    if (!s_loop || !s_loop->pool) return ERR_OK;
    if (!connection_is_active(s_loop->pool, conn->pool_index)) {
        return ERR_OK;  // already finalized
    }

    if ((conn->state == CONN_STATE_WEBSOCKET || conn->state == CONN_STATE_WS_CLOSING)
        && s_handlers && s_handlers->on_ws_disconnect) {
        s_handlers->on_ws_disconnect(conn);
    }
    conn->state = CONN_STATE_CLOSED;
    if (s_handlers && s_handlers->on_disconnect) {
        s_handlers->on_disconnect(conn);
    }

    bool aborted = raw_tcp_close(conn);
    connection_mark_inactive(s_loop->pool, conn->pool_index);
    connection_mark_write_pending(s_loop->pool, conn->pool_index, false);
    connection_mark_ws_inactive(s_loop->pool, conn->pool_index);
    return aborted ? ERR_ABRT : ERR_OK;
}

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

    // NULL pbuf means connection closed by remote. raw_finalize is idempotent
    // and also covers the case where a handler set CONN_STATE_CLOSED without
    // any teardown having run yet (the old early-return guard skipped cleanup
    // for those, leaking the pcb in CLOSE_WAIT and the pool slot forever).
    if (!p) {
        return raw_finalize(conn);
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return raw_finalize(conn);
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
            ESP_LOGE(TAG, "Failed to allocate linearize buffer (%u bytes)", (unsigned)p->tot_len);
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
            // Only count new requests, not continuation segments
            if (conn->state != CONN_STATE_HTTP_HEADERS) {
                s_loop->total_requests++;
            }
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

    // Handler closed the connection during dispatch: finalize now instead of
    // leaving the pcb open with a dead pool slot
    if (conn->state == CONN_STATE_CLOSING || conn->state == CONN_STATE_CLOSED) {
        pbuf_free(p);
        return raw_finalize(conn);
    }

    // Acknowledge received data to lwIP
    // Use conn->raw.pcb instead of local tpcb to avoid stale pointer if handler closed the PCB
    if (conn->raw.pcb && conn->state != CONN_STATE_FREE) {
        tcp_recved(conn->raw.pcb, (u16_t)((total_len <= UINT16_MAX) ? total_len : UINT16_MAX));
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

    // Only clear write_pending when all data has been acknowledged
    if (conn->raw.unacked_bytes == 0) {
        conn->raw.write_pending = false;
    }

    // Notify upper layer that send buffer space is available — but only when
    // it has queued work (O(1) bit test). Short responses with nothing
    // pending otherwise ran the whole on_write_ready prologue on every ACK.
    if (s_loop && s_loop->pool &&
        connection_has_write_pending(s_loop->pool, conn->pool_index) &&
        s_handlers->on_write_ready) {
        s_handlers->on_write_ready(conn);
    }

    return ERR_OK;
}

static void raw_err_cb(void* arg, err_t err) {
    connection_t* conn = (connection_t*)arg;
    if (!conn) return;

    ESP_LOGD(TAG, "Connection [%d] error: %d", conn->pool_index, err);

    // lwIP has already freed the pcb when err_cb is called; NULL it so
    // raw_finalize's raw_tcp_close doesn't touch it
    conn->raw.pcb = NULL;

    raw_finalize(conn);
}

static err_t raw_poll_cb(void* arg, struct tcp_pcb* tpcb) {
    connection_t* conn = (connection_t*)arg;
    if (!conn || !s_loop) return ERR_OK;

    // Advance the shared timeout clock from real elapsed time. Poll fires
    // per-connection, but the time-based advance is idempotent so it ticks
    // once per interval regardless of connection count. (Previously NOTHING
    // incremented tick_count in raw mode, so every timeout below was dead and
    // idle/slow-loris connections held pool slots forever.)
    {
        int64_t now_us = esp_timer_get_time();
        int64_t tick_us = (int64_t)CONFIG_HTTPD_RAW_POLL_INTERVAL * 500 * 1000;
        if (tick_us <= 0) tick_us = 1000000;
        if (s_loop->last_tick_us == 0) s_loop->last_tick_us = now_us;
        while (now_us - s_loop->last_tick_us >= tick_us) {
            s_loop->last_tick_us += tick_us;
            s_loop->tick_count++;
        }
    }

    // Finalize handler-initiated closes: upper-layer error paths set
    // CONN_STATE_CLOSED and nothing else in raw mode recycles them
    if (conn->state == CONN_STATE_CLOSED || conn->state == CONN_STATE_CLOSING) {
        return raw_finalize(conn);
    }

    // Kick the writer for write-pending connections: recovers transfers that
    // stalled because a file-IO submit was dropped (queue full) with no
    // outstanding ACKs left to re-trigger raw_sent_cb
    if (s_loop->pool &&
        connection_has_write_pending(s_loop->pool, conn->pool_index) &&
        s_handlers && s_handlers->on_write_ready) {
        s_handlers->on_write_ready(conn);
        if (conn->state == CONN_STATE_CLOSED) {
            return raw_finalize(conn);
        }
    }

    // Check for connection timeout
    uint32_t timeout_ticks = s_loop->timeout_ticks;

    // Use shorter timeout for WebSocket close handshake (configurable)
    if (conn->state == CONN_STATE_WS_CLOSING) {
        timeout_ticks = s_loop->ws_close_timeout_ticks;
    }

    // Skip timeout for active WebSocket connections
    if (conn->state == CONN_STATE_WEBSOCKET) {
        return ERR_OK;
    }

    if (s_loop->tick_count - conn->last_activity > timeout_ticks) {
        ESP_LOGD(TAG, "Connection [%d] timed out%s", conn->pool_index,
                 conn->state == CONN_STATE_WS_CLOSING ? " (ws close handshake)" : "");
        // Must return ERR_ABRT if the PCB was aborted so lwIP doesn't access
        // the freed PCB (raw_finalize reports this)
        return raw_finalize(conn);
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

    // Limit to available space and u16_t max (tcp_write takes u16_t len)
    // Caller must check return value and retry for remaining data
    size_t to_write = (len <= sndbuf) ? len : sndbuf;
    if (to_write > UINT16_MAX) to_write = UINT16_MAX;

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

bool raw_tcp_close(connection_t* conn) {
    if (!conn) return false;

    struct tcp_pcb* pcb = conn->raw.pcb;
    if (!pcb) return false;

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
    bool aborted = false;
    err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        ESP_LOGW(TAG, "tcp_close failed (%d), aborting", err);
        tcp_abort(pcb);
        aborted = true;
    }

    conn->raw.pcb = NULL;
    conn->raw.recv_offset = 0;
    conn->raw.unacked_bytes = 0;
    conn->raw.write_pending = false;
    return aborted;
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
