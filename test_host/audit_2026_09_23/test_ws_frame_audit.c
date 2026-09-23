// Audit 2026-09-23 repro tests for esphttpd WebSocket frame parsing.
// Compiles the REAL src/websocket.c (see run_tests.sh). Each test asserts the
// correct behaviour and fails on the current tree while the bug is present.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "private/websocket.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static ssize_t sent_bytes;
static ssize_t fake_send(connection_t* c, const void* d, size_t n, int f) {
    (void)c; (void)d; (void)f; sent_bytes += (ssize_t)n; return (ssize_t)n;
}

static void setup(connection_t* conn, ws_frame_context_t* ctx) {
    memset(conn, 0, sizeof(*conn));
    conn->state = CONN_STATE_WEBSOCKET;
    free(ctx->payload_buffer);
    memset(ctx, 0, sizeof(*ctx));
    ws_frame_ctx_init(ctx);
}

// LIBS-1: a zero-length masked frame whose 6-byte header ends exactly at the
// end of a recv slice (header split across TCP segments / recv chunks) is not
// completed in that call. It is completed on the NEXT call with
// bytes_consumed == 0, which on_ws_frame (esphttpd.c) treats as "no progress,
// need more data" and breaks out, silently discarding the whole next recv
// buffer (and leaving the parser in WS_STATE_COMPLETE, so the buffer after
// that is dropped too). The stream is then desynchronised.
static void test_LIBS_1_zero_len_frame_header_at_slice_end(void) {
    connection_t conn; ws_frame_context_t ctx = {0};
    setup(&conn, &ctx);
    size_t used = 0;

    // Slice 1: first 2 header bytes of an empty masked TEXT frame.
    uint8_t s1[] = { 0x81, 0x80 };
    ws_frame_result_t r = ws_process_frame(&conn, s1, sizeof s1, &ctx, &used);
    CHECK(r == WS_FRAME_NEED_MORE && used == 2, "r=%d used=%zu", r, used);

    // Slice 2: the 4 mask bytes -> header complete, payload length 0.
    // Correct: the frame is complete now (nothing else to wait for).
    uint8_t s2[] = { 0x11, 0x22, 0x33, 0x44 };
    r = ws_process_frame(&conn, s2, sizeof s2, &ctx, &used);
    CHECK(r == WS_FRAME_COMPLETE && used == 4,
          "empty frame with complete header not completed: r=%d used=%zu (expected COMPLETE/4)", r, used);

    // Whatever happened above, the next slice carries a complete masked
    // TEXT frame "hi". Simulate the caller's contract (on_ws_frame): a call
    // that consumes 0 bytes ends processing of this slice. The next frame
    // must not be lost.
    if (r == WS_FRAME_COMPLETE) { ctx.state = WS_STATE_OPCODE; ctx.payload_received = 0; }
    uint8_t s3[] = { 0x81, 0x82, 0, 0, 0, 0, 'h', 'i' };
    r = ws_process_frame(&conn, s3, sizeof s3, &ctx, &used);
    CHECK(used != 0,
          "next slice reported r=%d with bytes_consumed=0: on_ws_frame's `if (bytes_consumed == 0) break;` "
          "drops the entire %zu-byte slice", r, sizeof s3);
    free(ctx.payload_buffer);
}

// LIBS-1 (ping variant): an empty PING split the same way is answered only
// when the next unrelated data arrives, and that data is then discarded.
static void test_LIBS_1_zero_len_ping_split(void) {
    connection_t conn; ws_frame_context_t ctx = {0};
    setup(&conn, &ctx);
    size_t used = 0;
    sent_bytes = 0;
    uint8_t s1[] = { 0x89, 0x80, 1, 2 };
    ws_frame_result_t r = ws_process_frame(&conn, s1, sizeof s1, &ctx, &used);
    uint8_t s2[] = { 3, 4 };
    r = ws_process_frame(&conn, s2, sizeof s2, &ctx, &used);
    CHECK(r == WS_FRAME_COMPLETE && sent_bytes > 0,
          "empty PING with complete header: r=%d, pong bytes sent=%zd (expected COMPLETE + pong)", r, sent_bytes);
    free(ctx.payload_buffer);
}

int main(void) {
    ws_set_send_func(fake_send);
    test_LIBS_1_zero_len_frame_header_at_slice_end();
    test_LIBS_1_zero_len_ping_split();
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
