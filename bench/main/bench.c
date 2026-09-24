/*
 * esphttpd hot-path benchmark, ESP32-S3 under QEMU with -icount (bench/run.sh).
 *
 * With -icount shift=2 every guest instruction advances virtual time by 4 ns,
 * and the 240 MHz cycle counter by ~0.96, so every figure is a deterministic
 * (near-)instruction count of the
 * real component built at -O2 - independent of host load, and on the target
 * ISA. (It does not model cache/PSRAM stalls: it measures work, not latency.)
 *
 * Connections use "sink" fds: lwip_send / lwip_writev are wrapped at link time
 * (main/CMakeLists.txt) and bytes to sink fds never reach lwIP, so the figures
 * are esphttpd's own cost. Every benchmark also prints an FNV-1a fingerprint of
 * the bytes ONE operation put on the wire: fingerprints must be identical
 * before and after an optimisation (wire bytes unchanged).
 *
 * Output: "BENCH <name> <cycles/op> fp=<fingerprint> wire=<bytes>B/<send calls>
 * reads=<file read() calls>" - cycles/op is the minimum over BENCH_REPS reps.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "esphttpd.h"
#include "connection.h"
#include "event_loop.h"
#include "radix_tree.h"
#include "send_buffer.h"
#include "http_parser.h"
#include "test_exports.h"
#include "esp_cpu.h"
#include "esp_netif.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BENCH_REPS 5
#define SINK_FD_BASE 900
#define SINK_FD_COUNT 64

// ============================================================================
// Socket sink (wrapped lwip_send / lwip_writev)
// ============================================================================

static size_t s_sink_per_call = SIZE_MAX;  // max bytes one call accepts
static size_t s_sink_budget = SIZE_MAX;    // bytes accepted before EAGAIN
static uint64_t s_sink_bytes, s_sink_calls;
static uint32_t s_fp = 2166136261u;         // FNV-1a over everything sunk
static bool s_hash_on;                      // fingerprint pass only, not timed reps
static uint64_t s_read_calls;

static bool is_sink(int fd) { return fd >= SINK_FD_BASE && fd < SINK_FD_BASE + SINK_FD_COUNT; }

static size_t sink_take(const void* buf, size_t len) {
    size_t n = len;
    if (n > s_sink_per_call) n = s_sink_per_call;
    if (n > s_sink_budget) n = s_sink_budget;
    if (s_hash_on) {
        const uint8_t* p = buf;
        uint32_t h = s_fp;
        for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
        s_fp = h;
    }
    s_sink_budget -= n;
    s_sink_bytes += n;
    s_sink_calls++;
    return n;
}

ssize_t __real_lwip_send(int s, const void* data, size_t size, int flags);
ssize_t __wrap_lwip_send(int s, const void* data, size_t size, int flags) {
    if (is_sink(s)) {
        size_t n = sink_take(data, size);
        if (n == 0 && size > 0) { errno = EAGAIN; return -1; }
        return (ssize_t)n;
    }
    return __real_lwip_send(s, data, size, flags);
}

ssize_t __real_lwip_writev(int s, const struct iovec* iov, int iovcnt);
ssize_t __wrap_lwip_writev(int s, const struct iovec* iov, int iovcnt) {
    if (is_sink(s)) {
        size_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            size_t n = sink_take(iov[i].iov_base, iov[i].iov_len);
            s_sink_calls--;  // one call, however many iovecs
            total += n;
            if (n < iov[i].iov_len) break;
        }
        s_sink_calls++;
        if (total == 0) { errno = EAGAIN; return -1; }
        return (ssize_t)total;
    }
    return __real_lwip_writev(s, iov, iovcnt);
}

static void sink_reset(void) {
    s_sink_per_call = SIZE_MAX;
    s_sink_budget = SIZE_MAX;
    s_sink_bytes = s_sink_calls = 0;
    s_fp = 2166136261u;
}

// ============================================================================
// Read-only RAM VFS at /ram with one 128 KB file (httpd_resp_sendfile bench)
// ============================================================================

static uint8_t* s_big;
#define BIG_LEN (128 * 1024)
static size_t s_ram_pos;
static bool s_ram_open;
static int ram_open(const char* path, int flags, int mode) {
    (void)flags; (void)mode;
    if (strcmp(path, "/big.bin") != 0 || s_ram_open) { errno = ENOENT; return -1; }
    s_ram_open = true;
    s_ram_pos = 0;
    return 0;
}
static ssize_t ram_read(int fd, void* dst, size_t size) {
    (void)fd;
    s_read_calls++;
    size_t left = BIG_LEN - s_ram_pos;
    if (size > left) size = left;
    memcpy(dst, s_big + s_ram_pos, size);
    s_ram_pos += size;
    return (ssize_t)size;
}
static int ram_close(int fd) { (void)fd; s_ram_open = false; return 0; }
static int ram_fill(struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_size = BIG_LEN;
    return 0;
}
static int ram_fstat(int fd, struct stat* st) { (void)fd; return ram_fill(st); }
static int ram_stat(const char* path, struct stat* st) {
    if (strcmp(path, "/big.bin") != 0) { errno = ENOENT; return -1; }
    return ram_fill(st);
}
static void ram_vfs_register_once(void) {
    static bool done;
    if (done) return;
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = ram_open, .read = ram_read, .close = ram_close,
        .fstat = ram_fstat, .stat = ram_stat,
    };
    ESP_ERROR_CHECK(esp_vfs_register("/ram", &vfs, NULL));
    done = true;
}

// ============================================================================
// Harness
// ============================================================================

typedef void (*op_fn)(void);

// Runs op once with a fresh sink to fingerprint its wire bytes, then times
// `iters` back-to-back ops BENCH_REPS times and reports the fastest rep.
static void bench(const char* name, int iters, op_fn op, void (*setup)(void)) {
    if (setup) setup();
    sink_reset();
    s_read_calls = 0;
    s_hash_on = true;
    op();
    s_hash_on = false;
    uint32_t fp = s_fp;
    uint64_t bytes = s_sink_bytes, calls = s_sink_calls, reads = s_read_calls;
    uint64_t best = UINT64_MAX;
    for (int r = 0; r < BENCH_REPS; r++) {
        uint32_t t0 = esp_cpu_get_cycle_count();
        for (int i = 0; i < iters; i++) op();
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        uint64_t per = ((uint64_t)dt + (uint64_t)iters / 2) / (uint64_t)iters;
        if (per < best) best = per;
    }
    printf("BENCH %-26s %9" PRIu64 " cyc/op  fp=%08" PRIx32 "  wire=%" PRIu64 "B/%" PRIu64
           "calls  reads=%" PRIu64 "\n", name, best, fp, bytes, calls, reads);
    fflush(stdout);
}

static httpd_handle_t s_h;

static connection_t* conn_at(int i) {
    connection_t* c = connection_get(&g_server->connection_pool, i);
    memset(c, 0, sizeof(*c));
    c->fd = SINK_FD_BASE + i;
    c->pool_index = (uint8_t)i;
    c->state = CONN_STATE_NEW;
    connection_mark_active(&g_server->connection_pool, i);
    return c;
}

// Drain whatever the last request left queued, in sink "windows" of `window`
// bytes each (the event loop's write-ready rounds)
static void drain_rounds(connection_t* c, size_t window) {
    int guard = 0;
    while (connection_has_write_pending(&g_server->connection_pool, c->pool_index) &&
           guard++ < 100000) {
        s_sink_budget = window;
        g_server->handlers.on_write_ready(c);
    }
    s_sink_budget = SIZE_MAX;
}

static connection_t* s_c;
static const char* s_req;
static size_t s_req_len;

static void do_request(void) {
    g_server->handlers.on_http_request(s_c, (uint8_t*)s_req, s_req_len);
}

// The TCP window accepts `window` bytes per write-ready round, including the
// round the handler itself sends in: the rest queues (ring / memory stream /
// file stream) and drains over further rounds.
static void do_request_window8k(void) {
    s_sink_budget = 8192;
    g_server->handlers.on_http_request(s_c, (uint8_t*)s_req, s_req_len);
    drain_rounds(s_c, 8192);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static httpd_err_t h_ok(httpd_req_t* req) { return httpd_resp_send(req, "ok", 2); }

static int s_sink_int;
static httpd_err_t h_query(httpd_req_t* req) {
    char v[64];
    int acc = 0;
    acc += httpd_req_get_query(req, "limit", v, sizeof(v));
    acc += httpd_req_get_query(req, "offset", v, sizeof(v));
    acc += httpd_req_get_query(req, "name", v, sizeof(v));
    acc += httpd_req_get_query(req, "missing", v, sizeof(v));
    const char* a = httpd_req_get_header(req, "Authorization");
    const char* b = httpd_req_get_header(req, "Accept");
    const char* c = httpd_req_get_header(req, "X-Not-There");
    acc += (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
    s_sink_int += acc;
    return httpd_resp_send(req, v, (ssize_t)strlen(v));
}

static httpd_err_t h_headers(httpd_req_t* req) {
    httpd_resp_set_status(req, 201);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_header(req, "Cache-Control", "no-store");
    httpd_resp_set_header(req, "X-Request-Id", "abcdef0123456789");
    httpd_resp_set_header(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_header(req, "X-Frame-Options", "DENY");
    static const char body[] = "{\"ok\":true,\"id\":12345,\"name\":\"device-name-here\",\"v\":[1,2,3]}";
    return httpd_resp_send(req, body, sizeof(body) - 1);
}

static httpd_err_t h_json(httpd_req_t* req) {
    return httpd_resp_send_json(req,
        "{\"uptime\":123456,\"heap\":204800,\"rssi\":-61,\"ip\":\"192.168.1.50\","
        "\"fw\":\"1.1.0\",\"lua\":{\"running\":true,\"mem\":51234},\"wan\":\"wifi\","
        "\"time\":\"2026-09-24T12:00:00Z\",\"errors\":0,\"name\":\"node-17\"}");
}

static httpd_err_t h_chunked(httpd_req_t* req) {
    static const char chunk[64] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde";
    httpd_resp_set_type(req, "text/plain");
    for (int i = 0; i < 16; i++) httpd_resp_send_chunk(req, chunk, 64);
    return httpd_resp_send_chunk(req, NULL, 0);
}

typedef struct { size_t off, len; } prov_t;
static prov_t s_prov;
static ssize_t prov_cb(httpd_req_t* req, uint8_t* buf, size_t max) {
    (void)req;
    size_t n = s_prov.len - s_prov.off;
    if (n > max) n = max;
    memcpy(buf, s_big + s_prov.off, n);
    s_prov.off += n;
    return (ssize_t)n;
}
static httpd_err_t h_provider_len(httpd_req_t* req) {
    s_prov.off = 0; s_prov.len = BIG_LEN;
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_set_header(req, "Content-Encoding", "gzip");
    return httpd_resp_send_provider(req, BIG_LEN, prov_cb, NULL);
}
static httpd_err_t h_provider_chunked(httpd_req_t* req) {
    s_prov.off = 0; s_prov.len = 64 * 1024;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send_provider(req, -1, prov_cb, NULL);
}
static httpd_err_t h_send32k(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, (const char*)s_big, 32 * 1024);
}

static char s_file_path[64];
static httpd_err_t h_sendfile(httpd_req_t* req) {
    return httpd_resp_sendfile(req, s_file_path);
}

static httpd_err_t mw_pass(httpd_req_t* req, httpd_next_t next) { return next(req); }

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

static const char REQ_MIN[] = "GET /x HTTP/1.1\r\nHost: a\r\n\r\n";
static const char REQ_BROWSER[] =
    "GET /api/v1/status HTTP/1.1\r\n"
    "Host: 192.168.1.50\r\n"
    "Connection: keep-alive\r\n"
    "sec-ch-ua: \"Chromium\";v=\"128\", \"Not;A=Brand\";v=\"24\"\r\n"
    "Accept: application/json, text/plain, */*\r\n"
    "sec-ch-ua-mobile: ?0\r\n"
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36\r\n"
    "sec-ch-ua-platform: \"Linux\"\r\n"
    "Sec-Fetch-Site: same-origin\r\n"
    "Sec-Fetch-Mode: cors\r\n"
    "Sec-Fetch-Dest: empty\r\n"
    "Referer: http://192.168.1.50/status\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n"
    "Cookie: session=0123456789abcdef0123456789abcdef\r\n"
    "\r\n";
static const char REQ_QUERY[] =
    "GET /api/v1/q?limit=10&offset=20&name=hello%20world&flag&x=1 HTTP/1.1\r\n"
    "Host: 192.168.1.50\r\n"
    "Authorization: Bearer abcdefghijklmnopqrstuvwxyz0123456789\r\n"
    "Accept: application/json\r\n"
    "\r\n";

// ============================================================================
// Setup per benchmark
// ============================================================================

static void register_get(const char* pattern, httpd_handler_t h) {
    httpd_route_t r = { HTTP_GET, pattern, h, NULL };
    httpd_register_route(s_h, &r);
}

static char s_route_names[64][48];
static void register_64(void) {
    for (int i = 0; i < 64; i++) {
        switch (i % 4) {
            case 0: snprintf(s_route_names[i], 48, "/api/v1/res%d", i); break;
            case 1: snprintf(s_route_names[i], 48, "/api/v1/res%d/:id", i); break;
            case 2: snprintf(s_route_names[i], 48, "/api/v1/res%d/:id/sub/:sid", i); break;
            default: snprintf(s_route_names[i], 48, "/static%d/*", i); break;
        }
        register_get(s_route_names[i], h_ok);
    }
}

static void server_fresh(void) {
    if (s_h) httpd_stop(s_h);
    s_h = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.port = 0;  // ephemeral: the select benchmark opens a real listener
    if (httpd_start(&s_h, &cfg) != HTTPD_OK) { printf("httpd_start failed\n"); abort(); }
    s_c = conn_at(0);
}

static void set_req(const char* r) { s_req = r; s_req_len = strlen(r); }

static void setup_min(void) { server_fresh(); register_get("/x", h_ok); set_req(REQ_MIN); }
static void setup_browser(void) {
    server_fresh(); register_get("/api/v1/status", h_ok); set_req(REQ_BROWSER);
}
static void setup_query(void) { server_fresh(); register_get("/api/v1/q", h_query); set_req(REQ_QUERY); }
static void setup_route64(void) {
    server_fresh(); register_64(); set_req("GET /api/v1/res42/7/sub/9 HTTP/1.1\r\nHost: a\r\n\r\n");
}
static void setup_mw4(void) {
    server_fresh();
    for (int i = 0; i < 4; i++) httpd_use(s_h, mw_pass);
    register_get("/x", h_ok);
    set_req(REQ_MIN);
}
static void setup_headers(void) { server_fresh(); register_get("/x", h_headers); set_req(REQ_MIN); }
static void setup_json(void) { server_fresh(); register_get("/x", h_json); set_req(REQ_MIN); }
static void setup_chunked(void) { server_fresh(); register_get("/x", h_chunked); set_req(REQ_MIN); }
static void setup_prov_len(void) {
    server_fresh(); register_get("/x", h_provider_len); set_req(REQ_MIN);
}
static void setup_prov_chunked(void) {
    server_fresh(); register_get("/x", h_provider_chunked); set_req(REQ_MIN);
}
static void setup_send32k(void) { server_fresh(); register_get("/x", h_send32k); set_req(REQ_MIN); }
static void setup_sendfile(void) {
    server_fresh(); register_get("/x", h_sendfile); set_req(REQ_MIN);
    ram_vfs_register_once();
    snprintf(s_file_path, sizeof(s_file_path), "/ram/big.bin");
}

// Provider responses: drain in lwIP-sized windows (TCP_SND_BUF 5744 on target)
static void do_request_drain5744(void) {
    s_sink_budget = 5744;
    g_server->handlers.on_http_request(s_c, (uint8_t*)s_req, s_req_len);
    drain_rounds(s_c, 5744);
}

// ---------------------------------------------------------------------------
// Parser alone (http_parse_request over the browser request; the header
// index store is skipped because the slot has no recv_buf)
// ---------------------------------------------------------------------------

static connection_t s_parse_conn;
static void setup_parse(void) {
    server_fresh();
    set_req(REQ_BROWSER);
}
static void do_parse(void) {
    http_parser_context_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    memset(&s_parse_conn, 0, sizeof(s_parse_conn));
    s_parse_conn.pool_index = 1;  // slot 1: never used by a request here
    s_sink_int += (int)http_parse_request(&s_parse_conn, (const uint8_t*)s_req, s_req_len, &pctx);
}

// ---------------------------------------------------------------------------
// Radix lookup
// ---------------------------------------------------------------------------

static radix_tree_t* s_tree;
static const char* s_lookup_paths[] = {
    "/api/v1/res40", "/api/v1/res41/abc", "/api/v1/res42/7/sub/9", "/static43/js/app.js",
    "/api/v1/nope",
};
static void setup_radix(void) {
    if (s_tree) radix_tree_destroy(s_tree);
    s_tree = radix_tree_create();
    for (int i = 0; i < 64; i++) {
        switch (i % 4) {
            case 0: snprintf(s_route_names[i], 48, "/api/v1/res%d", i); break;
            case 1: snprintf(s_route_names[i], 48, "/api/v1/res%d/:id", i); break;
            case 2: snprintf(s_route_names[i], 48, "/api/v1/res%d/:id/sub/:sid", i); break;
            default: snprintf(s_route_names[i], 48, "/static%d/*", i); break;
        }
        radix_insert(s_tree, s_route_names[i], HTTP_GET, h_ok, NULL, NULL, 0);
    }
}
static void do_radix(void) {
    radix_match_t m;
    for (size_t i = 0; i < sizeof(s_lookup_paths) / sizeof(s_lookup_paths[0]); i++) {
        radix_lookup(s_tree, s_lookup_paths[i], HTTP_GET, false, &m, NULL, NULL);
        s_sink_int += m.matched;
    }
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------

static const char WS_UPGRADE[] =
    "GET /ws HTTP/1.1\r\nHost: a\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

static httpd_ws_t* s_ws[16];
static httpd_ws_t* ws_last;
static httpd_err_t ws_capture(httpd_ws_t* ws, httpd_ws_event_t* ev) {
    if (ev->type == WS_EVENT_CONNECT) ws_last = ws;
    return HTTPD_OK;
}

static void setup_ws16(void) {
    server_fresh();
    httpd_ws_route_t r = { .pattern = "/ws", .handler = ws_capture };
    httpd_register_ws_route(s_h, &r);
    for (int i = 0; i < 16; i++) {
        connection_t* c = conn_at(i);
        ws_last = NULL;
        g_server->handlers.on_http_request(c, (uint8_t*)WS_UPGRADE, sizeof(WS_UPGRADE) - 1);
        s_ws[i] = ws_last;
        httpd_ws_join(s_ws[i], "tele");
    }
    s_c = connection_get(&g_server->connection_pool, 0);
}

static uint8_t s_frame_small[256], s_frame_big[8192 + 16];
static size_t s_frame_small_len, s_frame_big_len;
static uint8_t s_frame_work[8192 + 16];

static size_t build_masked(uint8_t* out, uint8_t opcode, const uint8_t* payload, size_t len) {
    static const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
    size_t p = 0;
    out[p++] = 0x80 | opcode;
    if (len < 126) {
        out[p++] = 0x80 | (uint8_t)len;
    } else {
        out[p++] = 0x80 | 126;
        out[p++] = (uint8_t)(len >> 8);
        out[p++] = (uint8_t)len;
    }
    memcpy(out + p, mask, 4);
    p += 4;
    for (size_t i = 0; i < len; i++) out[p + i] = payload[i] ^ mask[i & 3];
    return p + len;
}

static void setup_ws_recv(void) {
    setup_ws16();
    s_frame_small_len = build_masked(s_frame_small, 0x1, s_big, 125);
    s_frame_big_len = build_masked(s_frame_big, 0x2, s_big, 8192);
}
// on_ws_frame unmasks in place: work on a copy
static void do_ws_recv_small(void) {
    memcpy(s_frame_work, s_frame_small, s_frame_small_len);
    g_server->handlers.on_ws_frame(s_c, s_frame_work, s_frame_small_len);
}
static void do_ws_recv_big(void) {
    memcpy(s_frame_work, s_frame_big, s_frame_big_len);
    g_server->handlers.on_ws_frame(s_c, s_frame_work, s_frame_big_len);
}
static void do_ws_send_small(void) { httpd_ws_send(s_ws[0], s_big, 125, WS_TYPE_TEXT); }
static void do_ws_send_4k(void) { httpd_ws_send(s_ws[0], s_big, 4096, WS_TYPE_BINARY); }
static void do_ws_broadcast(void) { httpd_ws_broadcast(s_h, "/ws", s_big, 256, WS_TYPE_TEXT); }
static void do_ws_publish(void) { httpd_ws_publish(s_h, "tele", s_big, 256, WS_TYPE_TEXT); }

// ---------------------------------------------------------------------------
// Event loop: select set build with 16 idle connections, and timeout scan
// ---------------------------------------------------------------------------

// 16 idle UDP sockets stand in for idle keep-alive connections: select()
// only needs readable-or-not fds, and nothing ever arrives on them.
static int s_idle_fds[16];
static uint8_t s_iobuf[4096];
static void setup_select16(void) {
    server_fresh();
    event_loop_t* loop = &g_server->event_loop;
    if (loop->listen_fd < 0 && event_loop_create_listener(loop) < 0) { printf("listener\n"); abort(); }
    loop->select_timeout.tv_sec = 0;
    loop->select_timeout.tv_usec = 0;
    for (int i = 0; i < 16; i++) {
        if (!s_idle_fds[i]) s_idle_fds[i] = socket(AF_INET, SOCK_DGRAM, 0);
        connection_t* c = conn_at(i);
        c->fd = s_idle_fds[i];
        c->last_activity = loop->tick_count;
    }
}
static void do_select16(void) {
    event_loop_iteration(&g_server->event_loop, &g_server->handlers, s_iobuf);
}
static void do_timeouts16(void) {
    event_loop_check_timeouts(&g_server->event_loop);
}

// Fixed reference: a 1000-iteration nop loop. Its figure never changes with
// esphttpd; it anchors the scale of every other line.
static void do_calib(void) {
    for (int i = 1000; i > 0; i--) __asm__ volatile("nop");
}

// ============================================================================

static void bench_task(void* arg) {
    (void)arg;
    s_big = malloc(BIG_LEN);
    if (!s_big) { printf("no memory\n"); abort(); }
    for (size_t i = 0; i < BIG_LEN; i++) s_big[i] = (uint8_t)("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789{}\":,"[i % 41]);
    ESP_ERROR_CHECK(esp_netif_init());

    bench("calib_nop_loop_x1000", 100, do_calib, NULL);
    bench("req_min", 200, do_request, setup_min);
    bench("req_browser_15hdr", 100, do_request, setup_browser);
    bench("parse_browser_15hdr", 100, do_parse, setup_parse);
    bench("req_query_4q_3hdr", 100, do_request, setup_query);
    bench("req_route64_params", 100, do_request, setup_route64);
    bench("req_mw4", 200, do_request, setup_mw4);
    bench("radix_lookup64_x5", 200, do_radix, setup_radix);
    bench("resp_headers_4", 200, do_request, setup_headers);
    bench("resp_json_fast", 200, do_request, setup_json);
    bench("resp_chunked_16x64", 50, do_request, setup_chunked);
    bench("resp_send_32k_q", 50, do_request_window8k, setup_send32k);
    bench("resp_provider_128k", 5, do_request_drain5744, setup_prov_len);
    bench("resp_provider_chunk_64k", 5, do_request_drain5744, setup_prov_chunked);
    bench("resp_sendfile_128k", 5, do_request_window8k, setup_sendfile);
    bench("ws_recv_125", 500, do_ws_recv_small, setup_ws_recv);
    bench("ws_recv_8k", 50, do_ws_recv_big, setup_ws_recv);
    bench("ws_send_125", 500, do_ws_send_small, setup_ws16);
    bench("ws_send_4k", 100, do_ws_send_4k, setup_ws16);
    bench("ws_broadcast_16x256", 50, do_ws_broadcast, setup_ws16);
    bench("ws_publish_16x256", 50, do_ws_publish, setup_ws16);
    bench("loop_select_16idle", 100, do_select16, setup_select16);
    bench("loop_timeouts_16", 1000, do_timeouts16, setup_select16);

    printf("BENCH_DONE %d\n", s_sink_int & 1);
    fflush(stdout);
    for (;;) vTaskDelay(portMAX_DELAY);
}

void app_main(void) {
    xTaskCreatePinnedToCore(bench_task, "bench", 12288, NULL, 5, NULL, 0);
}
