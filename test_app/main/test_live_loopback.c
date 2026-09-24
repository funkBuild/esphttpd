/**
 * @file test_live_loopback.c
 * @brief End-to-end golden tests over lwIP loopback (127.0.0.1)
 *
 * Every other suite in this app drives the handlers directly with mock fds.
 * These tests run the REAL select() event loop (event_loop_run on its own
 * task, exactly as server_task does in production) against real TCP client
 * sockets, and pin the exact bytes on the wire: HTTP responses (plain,
 * staged headers, JSON fast path, errors, chunked, data provider, sendfile
 * and filesystem serving), pipelined ordering, the WebSocket handshake and
 * frames, and cross-task WebSocket send ordering.
 *
 * Files are served from a tiny read-only RAM VFS mounted at /ram, so no flash
 * partition is needed.
 */

#include "unity.h"
#include "esphttpd.h"
#include "connection.h"
#include "event_loop.h"
#include "filesystem.h"
#include "test_exports.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static const char* TAG = "TEST_LIVE";

// ============================================================================
// RAM VFS (read-only) mounted at /ram
// ============================================================================

#define BIG_FILE_SIZE 20000

typedef struct {
    const char* path;       // relative to /ram, leading '/'
    const uint8_t* data;
    size_t len;
    bool dir;
} ram_file_t;

static uint8_t* s_big_data;
static const char s_hello_txt[] = "Hello, file!\n";
static const char s_style_css[] = "body{color:red}";
static const char s_app_js_gz[] = "\x1f\x8b" "GZIPDATA";
static const char s_index_html[] = "<h1>index</h1>";

static ram_file_t s_ram_files[] = {
    { "/hello.txt", (const uint8_t*)s_hello_txt, sizeof(s_hello_txt) - 1, false },
    { "/big.bin", NULL, BIG_FILE_SIZE, false },
    { "/empty.txt", (const uint8_t*)"", 0, false },
    { "/style.css", (const uint8_t*)s_style_css, sizeof(s_style_css) - 1, false },
    { "/app.js.gz", (const uint8_t*)s_app_js_gz, sizeof(s_app_js_gz) - 1, false },
    { "/sub", NULL, 0, true },
    { "/sub/index.html", (const uint8_t*)s_index_html, sizeof(s_index_html) - 1, false },
};
#define RAM_FILE_COUNT (sizeof(s_ram_files) / sizeof(s_ram_files[0]))

#define RAM_MAX_FDS 8
static struct { int file; size_t pos; bool used; } s_ram_fds[RAM_MAX_FDS];
static int s_ram_open_count;  // fds currently open (leak detection)

static int ram_find(const char* path) {
    for (size_t i = 0; i < RAM_FILE_COUNT; i++) {
        if (strcmp(s_ram_files[i].path, path) == 0) return (int)i;
    }
    return -1;
}

static void ram_fill_stat(int idx, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = s_ram_files[idx].dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_size = (off_t)s_ram_files[idx].len;
}

static int ram_open(const char* path, int flags, int mode) {
    (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY) { errno = EROFS; return -1; }
    int idx = ram_find(path);
    if (idx < 0 || s_ram_files[idx].dir) { errno = ENOENT; return -1; }
    for (int fd = 0; fd < RAM_MAX_FDS; fd++) {
        if (!s_ram_fds[fd].used) {
            s_ram_fds[fd].used = true;
            s_ram_fds[fd].file = idx;
            s_ram_fds[fd].pos = 0;
            s_ram_open_count++;
            return fd;
        }
    }
    errno = ENFILE;
    return -1;
}

static ssize_t ram_read(int fd, void* dst, size_t size) {
    if (fd < 0 || fd >= RAM_MAX_FDS || !s_ram_fds[fd].used) { errno = EBADF; return -1; }
    const ram_file_t* f = &s_ram_files[s_ram_fds[fd].file];
    size_t left = f->len - s_ram_fds[fd].pos;
    if (size > left) size = left;
    memcpy(dst, f->data + s_ram_fds[fd].pos, size);
    s_ram_fds[fd].pos += size;
    return (ssize_t)size;
}

static int ram_close(int fd) {
    if (fd < 0 || fd >= RAM_MAX_FDS || !s_ram_fds[fd].used) { errno = EBADF; return -1; }
    s_ram_fds[fd].used = false;
    s_ram_open_count--;
    return 0;
}

static int ram_fstat(int fd, struct stat* st) {
    if (fd < 0 || fd >= RAM_MAX_FDS || !s_ram_fds[fd].used) { errno = EBADF; return -1; }
    ram_fill_stat(s_ram_fds[fd].file, st);
    return 0;
}

static int ram_stat(const char* path, struct stat* st) {
    int idx = ram_find(path);
    if (idx < 0) { errno = ENOENT; return -1; }
    ram_fill_stat(idx, st);
    return 0;
}

static void ram_vfs_register_once(void) {
    static bool done = false;
    if (done) return;
    s_big_data = malloc(BIG_FILE_SIZE);
    TEST_ASSERT_NOT_NULL(s_big_data);
    for (size_t i = 0; i < BIG_FILE_SIZE; i++) {
        s_big_data[i] = (uint8_t)(i * 7 + i / 251);
    }
    s_ram_files[1].data = s_big_data;
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = ram_open,
        .read = ram_read,
        .close = ram_close,
        .fstat = ram_fstat,
        .stat = ram_stat,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_vfs_register("/ram", &vfs, NULL));
    done = true;
}

// ============================================================================
// Live server fixture: real event loop on its own task
// ============================================================================

static httpd_handle_t s_handle;
static TaskHandle_t s_loop_task;
static volatile bool s_loop_exited;
static uint16_t s_port = 18080;

static esphttpd_server_t* srv(void) { return (esphttpd_server_t*)g_server; }

static void live_loop_task(void* arg) {
    (void)arg;
    event_loop_run(&srv()->event_loop, &srv()->handlers);
    s_loop_exited = true;
    vTaskDelete(NULL);
}

static void netif_once(void) {
    static bool done = false;
    if (done) return;
    TEST_ASSERT_EQUAL(ESP_OK, esp_netif_init());
    done = true;
}

typedef void (*register_fn_t)(httpd_handle_t h);
static int64_t live_stop_quiet(void);

static void live_start(register_fn_t reg) {
    live_stop_quiet();  // a previous test that failed mid-way left it running
    netif_once();
    ram_vfs_register_once();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.port = ++s_port;  // fresh port per test: no TIME_WAIT interplay
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_start(&s_handle, &cfg));
    if (reg) reg(s_handle);
    s_loop_exited = false;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(live_loop_task, "live_httpd", 8192, NULL, 5, &s_loop_task));
    for (int i = 0; i < 200 && srv()->event_loop.listen_fd < 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_TRUE(srv()->event_loop.listen_fd >= 0);
}

// Returns how long the loop task took to notice the stop request (ms).
// Assertion-free so it can also clean up after a failed test.
static int64_t live_stop_quiet(void) {
    esphttpd_server_t* s = srv();
    if (!s_handle || !s) return -1;
    int64_t t0 = esp_timer_get_time();
    event_loop_stop(&s->event_loop);
    for (int i = 0; i < 600 && !s_loop_exited; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    int64_t stop_ms = (esp_timer_get_time() - t0) / 1000;
    if (!s_loop_exited) return -2;
    // Test mode httpd_stop does not close connection fds: remember them
    int fds[MAX_CONNECTIONS];
    int nfds = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connection_is_active(&s->connection_pool, i)) {
            fds[nfds++] = s->connection_pool.connections[i].fd;
        }
    }
    s->filesystem_enabled = false;
    s->filesystem = NULL;
    httpd_err_t err = httpd_stop(s_handle);
    for (int i = 0; i < nfds; i++) {
        close(fds[i]);
    }
    s_handle = NULL;
    return err == HTTPD_OK ? stop_ms : -3;
}

static int64_t live_stop(void) {
    int64_t ms = live_stop_quiet();
    TEST_ASSERT_TRUE_MESSAGE(ms >= 0, "live server stop failed");
    return ms;
}

// ============================================================================
// Client helpers
// ============================================================================

static int cli_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(fd >= 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(s_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_MESSAGE(0, connect(fd, (struct sockaddr*)&a, sizeof(a)), "connect");
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void cli_send(int fd, const void* data, size_t len) {
    const uint8_t* p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        TEST_ASSERT_TRUE_MESSAGE(n > 0, "client send");
        p += n;
        len -= (size_t)n;
    }
}

static void cli_send_str(int fd, const char* s) { cli_send(fd, s, strlen(s)); }

// Read exactly len bytes (3 s receive timeout per recv). Returns bytes read.
static size_t cli_recv_exact(int fd, uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got;
}

// Bytes available within wait_ms (used to assert "nothing extra on the wire")
static size_t cli_recv_extra(int fd, uint8_t* buf, size_t cap, int wait_ms) {
    struct timeval tv = { .tv_sec = 0, .tv_usec = wait_ms * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, buf, cap, 0);
    struct timeval tv3 = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv3, sizeof(tv3));
    return n > 0 ? (size_t)n : 0;
}

static void print_escaped(const char* label, const uint8_t* b, size_t n) {
    char out[1024];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 5 < sizeof(out); i++) {
        uint8_t c = b[i];
        if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c >= 32 && c < 127) { out[o++] = (char)c; }
        else { o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c); }
    }
    out[o] = '\0';
    printf("%s (%zu bytes): %s\n", label, n, out);
}

// Read exactly the expected bytes and compare; then assert no extra bytes
static void expect_bytes(int fd, const void* expected, size_t len) {
    uint8_t* got = malloc(len + 1);
    TEST_ASSERT_NOT_NULL(got);
    size_t n = cli_recv_exact(fd, got, len);
    if (n != len || memcmp(got, expected, len) != 0) {
        print_escaped("expected", expected, len);
        print_escaped("actual  ", got, n);
    }
    TEST_ASSERT_EQUAL_size_t(len, n);
    TEST_ASSERT_EQUAL_MEMORY(expected, got, len);
    free(got);
    uint8_t extra[64];
    size_t x = cli_recv_extra(fd, extra, sizeof(extra), 100);
    if (x) print_escaped("unexpected extra", extra, x);
    TEST_ASSERT_EQUAL_size_t(0, x);
}

static void expect_str(int fd, const char* expected) { expect_bytes(fd, expected, strlen(expected)); }

// Masked client WebSocket frame (fixed mask key 01 02 03 04)
static size_t ws_build_client_frame(uint8_t* out, uint8_t opcode, const uint8_t* payload, size_t len) {
    static const uint8_t mask[4] = { 1, 2, 3, 4 };
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

// Read one unmasked server frame; returns payload length, fills opcode
static int ws_read_server_frame(int fd, uint8_t* opcode, uint8_t* payload, size_t cap) {
    uint8_t h[2];
    if (cli_recv_exact(fd, h, 2) != 2) return -1;
    *opcode = h[0] & 0x0F;
    if (!(h[0] & 0x80) || (h[1] & 0x80)) return -2;  // FIN set, unmasked
    size_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t e[2];
        if (cli_recv_exact(fd, e, 2) != 2) return -1;
        len = ((size_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        return -3;
    }
    if (len > cap) return -4;
    if (cli_recv_exact(fd, payload, len) != len) return -1;
    return (int)len;
}

#define WS_UPGRADE_REQ \
    "GET /ws HTTP/1.1\r\n" \
    "Host: x\r\n" \
    "Upgrade: websocket\r\n" \
    "Connection: Upgrade\r\n" \
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" \
    "Sec-WebSocket-Version: 13\r\n\r\n"

#define WS_UPGRADE_RESP \
    "HTTP/1.1 101 Switching Protocols\r\n" \
    "Upgrade: websocket\r\n" \
    "Connection: Upgrade\r\n" \
    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"

// ============================================================================
// Handlers
// ============================================================================

static httpd_err_t h_hello(httpd_req_t* req) {
    return httpd_resp_send(req, "hello", 5);
}

static httpd_err_t h_json_staged(httpd_req_t* req) {
    httpd_resp_set_header(req, "X-Test", "1");
    return httpd_resp_send_json(req, "{\"a\":1}");
}

static httpd_err_t h_json_fast(httpd_req_t* req) {
    return httpd_resp_send_json(req, "{\"a\":1}");
}

static httpd_err_t h_status(httpd_req_t* req) {
    httpd_resp_set_status(req, 201);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "made", -1);
}

static httpd_err_t h_chunk(httpd_req_t* req) {
    httpd_resp_send_chunk(req, "abc", 3);
    httpd_resp_send_chunk(req, "0123456789abcdef", 16);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static httpd_err_t h_fail(httpd_req_t* req) {
    (void)req;
    return HTTPD_ERR_INVALID_ARG;
}

// Data provider: 3 x "0123456789", one 10-byte piece per call
static int s_prov_pieces;  // one provider response at a time in these tests
static ssize_t prov_digits(httpd_req_t* req, uint8_t* buf, size_t max_len) {
    (void)req;
    TEST_ASSERT_TRUE(max_len >= 10);
    if (s_prov_pieces >= 3) {
        s_prov_pieces = 0;
        return 0;
    }
    s_prov_pieces++;
    memcpy(buf, "0123456789", 10);
    return 10;
}

static volatile int s_prov_done_err = -999;
static void prov_done(httpd_req_t* req, httpd_err_t err) { (void)req; s_prov_done_err = err; }

static httpd_err_t h_prov_len(httpd_req_t* req) {
    return httpd_resp_send_provider(req, 30, prov_digits, prov_done);
}

static httpd_err_t h_prov_chunked(httpd_req_t* req) {
    return httpd_resp_send_provider(req, -1, prov_digits, prov_done);
}

static httpd_err_t h_sendfile(httpd_req_t* req) {
    // /file/<name> -> /ram/<name>
    char path[64];
    snprintf(path, sizeof(path), "/ram/%s", httpd_req_get_param(req, "name"));
    return httpd_resp_sendfile(req, path);
}

static httpd_err_t h_sendfile_typed(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/x-custom");
    httpd_resp_set_header(req, "X-File", "yes");
    return httpd_resp_sendfile(req, "/ram/hello.txt");
}

static httpd_err_t h_sendfile_traversal(httpd_req_t* req) {
    return httpd_resp_sendfile(req, "/ram/../hello.txt");
}

static volatile int s_async_done_err = -999;
static void async_file_done(httpd_req_t* req, httpd_err_t err) { (void)req; s_async_done_err = err; }

static httpd_err_t h_fs_serve(httpd_req_t* req) {
    // /fs/* -> filesystem-relative path after "/fs"
    const char* rel = req->path + 3;
    httpd_err_t e = httpd_resp_sendfile_async(req, rel, async_file_done);
    if (e != HTTPD_OK) {
        return httpd_resp_send_error(req, 503, "fs refused");
    }
    return HTTPD_OK;
}

// WebSocket: greet on connect, echo messages
static httpd_ws_t* volatile s_ws;
static httpd_err_t ws_echo(httpd_ws_t* ws, httpd_ws_event_t* ev) {
    if (ev->type == WS_EVENT_CONNECT) {
        s_ws = ws;
        return httpd_ws_send(ws, "hi", 2, WS_TYPE_TEXT);
    }
    if (ev->type == WS_EVENT_MESSAGE) {
        return httpd_ws_send(ws, ev->data, ev->len, ev->frame_type);
    }
    if (ev->type == WS_EVENT_DISCONNECT) {
        s_ws = NULL;
    }
    return HTTPD_OK;
}

// The server instance is static and httpd_stop does not clear a registered
// error handler, so one installed by an earlier suite would still intercept
// errors here. Install a pass-through so the DEFAULT error responses are what
// these tests pin.
static httpd_err_t passthrough_error_handler(httpd_err_t err, httpd_req_t* req) {
    (void)req;
    return err;
}

static void register_all(httpd_handle_t h) {
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_on_error(h, passthrough_error_handler));
    static const httpd_route_t routes[] = {
        { HTTP_GET, "/hello", h_hello, NULL },
        { HTTP_GET, "/json", h_json_staged, NULL },
        { HTTP_GET, "/json2", h_json_fast, NULL },
        { HTTP_POST, "/status", h_status, NULL },
        { HTTP_GET, "/chunk", h_chunk, NULL },
        { HTTP_GET, "/fail", h_fail, NULL },
        { HTTP_GET, "/prov", h_prov_len, NULL },
        { HTTP_GET, "/provc", h_prov_chunked, NULL },
        { HTTP_GET, "/file/:name", h_sendfile, NULL },
        { HTTP_GET, "/typed", h_sendfile_typed, NULL },
        { HTTP_GET, "/trav", h_sendfile_traversal, NULL },
        { HTTP_GET, "/fs/*", h_fs_serve, NULL },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_route(h, &routes[i]));
    }
    httpd_ws_route_t ws = { .pattern = "/ws", .handler = ws_echo };
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_register_ws_route(h, &ws));
}

// ============================================================================
// Golden HTTP tests
// ============================================================================

static void test_live_golden_basic_responses(void) {
    live_start(register_all);
    int fd = cli_connect();

    cli_send_str(fd, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");

    cli_send_str(fd, "GET /json HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nX-Test: 1\r\nContent-Type: application/json\r\n"
                   "Content-Length: 7\r\n\r\n{\"a\":1}");

    cli_send_str(fd, "GET /json2 HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                   "Content-Length: 7\r\n\r\n{\"a\":1}");

    cli_send_str(fd, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 9\r\n\r\nNot Found");

    cli_send_str(fd, "GET /fail HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 11\r\n\r\nBad Request");

    cli_send_str(fd, "GET /chunk HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "3\r\nabc\r\n10\r\n0123456789abcdef\r\n0\r\n\r\n");

    cli_send_str(fd, "POST /status HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    expect_str(fd, "HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 4\r\n\r\nmade");

    cli_send_str(fd, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");

    close(fd);
    live_stop();
}

static void test_live_golden_provider(void) {
    live_start(register_all);
    int fd = cli_connect();

    s_prov_done_err = -999;
    cli_send_str(fd, "GET /prov HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 30\r\n\r\n"
                   "012345678901234567890123456789");
    TEST_ASSERT_EQUAL(HTTPD_OK, s_prov_done_err);

    s_prov_done_err = -999;
    cli_send_str(fd, "GET /provc HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "a\r\n0123456789\r\na\r\n0123456789\r\na\r\n0123456789\r\n0\r\n\r\n");
    TEST_ASSERT_EQUAL(HTTPD_OK, s_prov_done_err);

    close(fd);
    live_stop();
}

// Pipelined requests are answered strictly in order, including behind an
// asynchronous provider response and a file stream
static void test_live_golden_pipelined_order(void) {
    live_start(register_all);
    int fd = cli_connect();
    cli_send_str(fd,
        "GET /prov HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /file/hello.txt HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /json2 HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd,
        "HTTP/1.1 200 OK\r\nContent-Length: 30\r\n\r\n"
        "012345678901234567890123456789"
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, file!\n"
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}");
    close(fd);
    live_stop();
}

// ============================================================================
// Golden file-serving tests (httpd_resp_sendfile and the filesystem path)
// ============================================================================

static void test_live_golden_sendfile(void) {
    live_start(register_all);
    int fd = cli_connect();

    cli_send_str(fd, "GET /file/hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n"
                   "Hello, file!\n");

    cli_send_str(fd, "GET /file/empty.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n");

    cli_send_str(fd, "GET /file/missing.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 14\r\n\r\nFile not found");

    cli_send_str(fd, "GET /typed HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/x-custom\r\nX-File: yes\r\n"
                   "Content-Length: 13\r\n\r\nHello, file!\n");

    cli_send_str(fd, "GET /trav HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 9\r\n\r\nForbidden");

    // Large file: streamed through the ring buffer across many writes
    static const char big_hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 20000\r\n\r\n";
    size_t total = sizeof(big_hdr) - 1 + BIG_FILE_SIZE;
    uint8_t* expected = malloc(total);
    TEST_ASSERT_NOT_NULL(expected);
    memcpy(expected, big_hdr, sizeof(big_hdr) - 1);
    memcpy(expected + sizeof(big_hdr) - 1, s_big_data, BIG_FILE_SIZE);
    cli_send_str(fd, "GET /file/big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_bytes(fd, expected, total);
    free(expected);

    close(fd);
    live_stop();
    TEST_ASSERT_EQUAL(0, s_ram_open_count);
}

static filesystem_t s_fs;

static void attach_ram_filesystem(uint8_t max_open) {
    memset(&s_fs, 0, sizeof(s_fs));
    s_fs.mounted = true;
    strcpy(s_fs.base_path, "/ram");
    s_fs.base_path_len = 4;
    s_fs.max_open_files = max_open;
    srv()->filesystem = &s_fs;
    srv()->filesystem_enabled = true;
}

static void test_live_golden_filesystem_serving(void) {
    live_start(register_all);
    attach_ram_filesystem(5);
    int fd = cli_connect();

    s_async_done_err = -999;
    cli_send_str(fd, "GET /fs/hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n"
                   "Hello, file!\n");
    TEST_ASSERT_EQUAL(HTTPD_OK, s_async_done_err);

    cli_send_str(fd, "GET /fs/style.css HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\nContent-Length: 15\r\n"
                   "Cache-Control: public, max-age=86400\r\n\r\nbody{color:red}");

    cli_send_str(fd, "GET /fs/app.js HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_bytes(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nContent-Length: 10\r\n"
                     "Content-Encoding: gzip\r\nCache-Control: public, max-age=86400\r\n\r\n"
                     "\x1f\x8bGZIPDATA",
                 sizeof("HTTP/1.1 200 OK\r\nContent-Type: application/javascript\r\nContent-Length: 10\r\n"
                        "Content-Encoding: gzip\r\nCache-Control: public, max-age=86400\r\n\r\n"
                        "\x1f\x8bGZIPDATA") - 1);

    cli_send_str(fd, "GET /fs/sub/ HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 14\r\n\r\n"
                   "<h1>index</h1>");

    cli_send_str(fd, "GET /fs/missing.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 10\r\n\r\nfs refused");
    TEST_ASSERT_EQUAL(HTTPD_ERR_NOT_FOUND, s_async_done_err);

    // Zero-length file: the header block alone (Content-Length: 0). Known
    // pre-existing wedge, pinned as-is: httpd_resp_sendfile_async arms its
    // completion although nothing is queued, so on_done never fires and the
    // connection never re-arms - hence this is the LAST request on it.
    s_async_done_err = -999;
    cli_send_str(fd, "GET /fs/empty.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n");

    TEST_ASSERT_EQUAL(0, s_fs.open_files);
    close(fd);
    live_stop();
    TEST_ASSERT_EQUAL(0, s_ram_open_count);
}

// max_open_files counts in-flight streams: a stalled big-file stream holds
// the only slot, so a second request is refused until it drains
static void test_live_golden_filesystem_max_open(void) {
    live_start(register_all);
    attach_ram_filesystem(1);

    int slow = cli_connect();
    int rcv = 1024;  // small window keeps the stream in flight while unread
    setsockopt(slow, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    cli_send_str(slow, "GET /fs/big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL(1, s_fs.open_files);

    int fd = cli_connect();
    cli_send_str(fd, "GET /fs/hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
                   "Content-Length: 10\r\n\r\nfs refused");

    // Drain the big response: slot released
    static const char big_hdr[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 20000\r\n\r\n";
    size_t total = sizeof(big_hdr) - 1 + BIG_FILE_SIZE;
    uint8_t* got = malloc(total);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_size_t(total, cli_recv_exact(slow, got, total));
    TEST_ASSERT_EQUAL_MEMORY(big_hdr, got, sizeof(big_hdr) - 1);
    TEST_ASSERT_EQUAL_MEMORY(s_big_data, got + sizeof(big_hdr) - 1, BIG_FILE_SIZE);
    free(got);
    vTaskDelay(pdMS_TO_TICKS(50));
    TEST_ASSERT_EQUAL(0, s_fs.open_files);

    cli_send_str(fd, "GET /fs/hello.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expect_str(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n"
                   "Hello, file!\n");

    close(fd);
    close(slow);
    live_stop();
    TEST_ASSERT_EQUAL(0, s_ram_open_count);
}

// ============================================================================
// Golden WebSocket tests
// ============================================================================

static int ws_open(void) {
    int fd = cli_connect();
    s_ws = NULL;
    cli_send_str(fd, WS_UPGRADE_REQ);
    // Handshake immediately followed by the handler's greeting frame
    expect_bytes(fd, WS_UPGRADE_RESP "\x81\x02hi", sizeof(WS_UPGRADE_RESP) - 1 + 4);
    TEST_ASSERT_NOT_NULL(s_ws);
    return fd;
}

static void test_live_golden_websocket(void) {
    live_start(register_all);
    int fd = ws_open();
    uint8_t frame[512];

    size_t n = ws_build_client_frame(frame, 0x1, (const uint8_t*)"ping", 4);
    cli_send(fd, frame, n);
    expect_bytes(fd, "\x81\x04ping", 6);

    uint8_t bin[200];
    for (int i = 0; i < 200; i++) bin[i] = (uint8_t)i;
    n = ws_build_client_frame(frame, 0x2, bin, sizeof(bin));
    cli_send(fd, frame, n);
    uint8_t expected[4 + 200] = { 0x82, 0x7e, 0x00, 0xc8 };
    memcpy(expected + 4, bin, 200);
    expect_bytes(fd, expected, sizeof(expected));

    // App-initiated close: close frame 1000 + reason, then the client's echo
    TEST_ASSERT_EQUAL(HTTPD_OK, httpd_ws_close(s_ws, 1000, "bye"));
    expect_bytes(fd, "\x88\x05\x03\xe8" "bye", 7);

    close(fd);
    live_stop();
}

// ============================================================================
// Cross-task WebSocket send ordering
// ============================================================================

#define ORDER_FRAMES 40

typedef struct {
    char tag;
    SemaphoreHandle_t done;
} order_sender_t;

static void order_sender_task(void* arg) {
    order_sender_t* s = arg;
    char* buf = malloc(2000);
    for (int i = 0; i < ORDER_FRAMES; i++) {
        // Every 7th frame is large enough to overflow the socket and queue.
        // Total volume per sender stays well under the 32 KB per-connection
        // backlog cap so no frame is refused.
        size_t len = (i % 7 == 3) ? 2000 : 16;
        memset(buf, s->tag, len);
        snprintf(buf, 16, "%c:%04d", s->tag, i);
        buf[6] = '|';
        httpd_ws_t* ws = s_ws;
        if (!ws || httpd_ws_send(ws, buf, len, WS_TYPE_BINARY) != HTTPD_OK) break;
        if ((i & 3) == 0) vTaskDelay(1);
    }
    free(buf);
    xSemaphoreGive(s->done);
    vTaskDelete(NULL);
}

static void test_live_ws_cross_task_send_ordering(void) {
    live_start(register_all);
    int fd = ws_open();

    order_sender_t a = { 'A', xSemaphoreCreateBinary() };
    order_sender_t b = { 'B', xSemaphoreCreateBinary() };
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(order_sender_task, "ordA", 4096, &a, 4, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(order_sender_task, "ordB", 4096, &b, 4, NULL));

    int next[2] = { 0, 0 };
    uint8_t* payload = malloc(4096);
    for (int f = 0; f < 2 * ORDER_FRAMES; f++) {
        uint8_t op;
        int len = ws_read_server_frame(fd, &op, payload, 4096);
        TEST_ASSERT_TRUE_MESSAGE(len >= 7, "frame read");
        TEST_ASSERT_EQUAL_UINT8(0x2, op);
        char tag = (char)payload[0];
        TEST_ASSERT_TRUE(tag == 'A' || tag == 'B');
        int idx = atoi((const char*)payload + 2);
        int k = (tag == 'A') ? 0 : 1;
        TEST_ASSERT_EQUAL_INT(next[k], idx);   // per-sender order preserved
        size_t want = (idx % 7 == 3) ? 2000 : 16;
        TEST_ASSERT_EQUAL_size_t(want, (size_t)len);
        for (int j = 7; j < len; j++) {        // no interleaving inside a frame
            TEST_ASSERT_EQUAL_UINT8((uint8_t)tag, payload[j]);
        }
        next[k]++;
    }
    free(payload);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(a.done, pdMS_TO_TICKS(3000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(b.done, pdMS_TO_TICKS(3000)));
    vSemaphoreDelete(a.done);
    vSemaphoreDelete(b.done);
    TEST_ASSERT_EQUAL_INT(ORDER_FRAMES, next[0]);
    TEST_ASSERT_EQUAL_INT(ORDER_FRAMES, next[1]);

    close(fd);
    live_stop();
}

void test_live_loopback_run(void) {
    ESP_LOGI(TAG, "Running live loopback golden tests");
    RUN_TEST(test_live_golden_basic_responses);
    RUN_TEST(test_live_golden_provider);
    RUN_TEST(test_live_golden_pipelined_order);
    RUN_TEST(test_live_golden_sendfile);
    RUN_TEST(test_live_golden_filesystem_serving);
    RUN_TEST(test_live_golden_filesystem_max_open);
    RUN_TEST(test_live_golden_websocket);
    RUN_TEST(test_live_ws_cross_task_send_ordering);
}
