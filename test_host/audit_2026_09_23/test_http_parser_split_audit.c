// Audit 2026-09-23: differential split test for the REAL src/http_parser.c.
// on_http_request (esphttpd.c) appends each recv slice to one accumulating
// recv_buf and calls http_parse_request(recv_buf + parse_offset, len). The
// parse outcome (result, header_bytes, content_length, headers stored) must
// not depend on where TCP happened to split the request. This harness feeds
// each request whole, then split at every 1- and 2-way boundary, and compares.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "private/http_parser.h"

static int failures;

typedef struct { char k[80]; char v[2100]; } hdr_t;
static hdr_t hdrs[64];
static int nhdrs;

void esphttpd_store_header(connection_t* conn, const uint8_t* key, uint16_t key_len,
                           const uint8_t* value, uint16_t value_len) {
    (void)conn;
    if (nhdrs >= 64) return;
    snprintf(hdrs[nhdrs].k, sizeof hdrs[nhdrs].k, "%.*s", (int)key_len, (const char*)key);
    snprintf(hdrs[nhdrs].v, sizeof hdrs[nhdrs].v, "%.*s", (int)value_len, (const char*)value);
    nhdrs++;
}

typedef struct {
    parse_result_t result;
    unsigned header_bytes;
    unsigned content_length;
    int keep_alive, upgrade_ws, is_websocket;
    char ws_key[32];
    int nhdrs;
    hdr_t hdrs[8];
} outcome_t;

// Feed `req` split at the given cut points (ascending, exclusive of 0/len).
static outcome_t run(const char* req, size_t len, const size_t* cuts, int ncuts) {
    static uint8_t buf[8192];
    memcpy(buf, req, len);
    connection_t conn; memset(&conn, 0, sizeof conn);
    http_parser_context_t ctx; memset(&ctx, 0, sizeof ctx);
    nhdrs = 0;
    outcome_t o; memset(&o, 0, sizeof o);
    size_t pos = 0;
    parse_result_t r = PARSE_NEED_MORE;
    for (int c = 0; c <= ncuts; c++) {
        size_t end = (c < ncuts) ? cuts[c] : len;
        size_t off = pos;
        r = http_parse_request(&conn, buf + off, end - off, &ctx);
        pos = end;
        if (r != PARSE_NEED_MORE) { conn.header_bytes += off; break; }
    }
    o.result = r;
    o.header_bytes = conn.header_bytes;
    o.content_length = conn.content_length;
    o.keep_alive = conn.keep_alive; o.upgrade_ws = conn.upgrade_ws; o.is_websocket = conn.is_websocket;
    memcpy(o.ws_key, ctx.ws_client_key, sizeof o.ws_key);
    o.nhdrs = nhdrs;
    for (int i = 0; i < nhdrs && i < 8; i++) o.hdrs[i] = hdrs[i];
    return o;
}

static int same(const outcome_t* a, const outcome_t* b) {
    if (a->result != b->result) return 0;
    if (a->result == PARSE_ERROR) return 1;
    if (a->result == PARSE_NEED_MORE) return 1;
    if (a->header_bytes != b->header_bytes || a->content_length != b->content_length ||
        a->keep_alive != b->keep_alive || a->upgrade_ws != b->upgrade_ws ||
        a->is_websocket != b->is_websocket || strcmp(a->ws_key, b->ws_key) || a->nhdrs != b->nhdrs)
        return 0;
    for (int i = 0; i < a->nhdrs && i < 8; i++)
        if (strcmp(a->hdrs[i].k, b->hdrs[i].k) || strcmp(a->hdrs[i].v, b->hdrs[i].v)) return 0;
    return 1;
}

static void describe(const char* tag, const outcome_t* o) {
    printf("    %s: result=%d header_bytes=%u cl=%u ka=%d nh=%d", tag, o->result, o->header_bytes,
           o->content_length, o->keep_alive, o->nhdrs);
    for (int i = 0; i < o->nhdrs && i < 8; i++) printf(" [%s=%s]", o->hdrs[i].k, o->hdrs[i].v);
    printf("\n");
}

static int check_splits(const char* name, const char* req) {
    size_t len = strlen(req);
    outcome_t whole = run(req, len, NULL, 0);
    int bad = 0;
    for (size_t a = 1; a < len && !bad; a++) {
        size_t cuts1[1] = { a };
        outcome_t o = run(req, len, cuts1, 1);
        if (!same(&whole, &o)) {
            printf("FAIL %s: split at %zu (byte 0x%02x | 0x%02x) differs from unsplit parse\n",
                   name, a, (unsigned char)req[a - 1], (unsigned char)req[a]);
            describe("whole", &whole); describe("split", &o);
            bad = 1;
        }
        for (size_t b = a + 1; b < len && !bad; b++) {
            size_t cuts2[2] = { a, b };
            outcome_t o2 = run(req, len, cuts2, 2);
            if (!same(&whole, &o2)) {
                printf("FAIL %s: split at %zu,%zu differs from unsplit parse\n", name, a, b);
                describe("whole", &whole); describe("split", &o2);
                bad = 1;
            }
        }
    }
    if (bad) failures++;
    return bad;
}

int main(void) {
    // Well-formed controls (must pass).
    check_splits("control_get", "GET /a?x=1 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    check_splits("control_post", "POST /p HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello");
    check_splits("control_ws", "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    check_splits("control_bare_lf", "GET / HTTP/1.1\nHost: h\nContent-Length: 0\n\n");
    check_splits("control_empty_value", "GET / HTTP/1.1\r\nX-Empty:\r\nHost: h\r\n\r\n");
    check_splits("control_ows_value", "GET / HTTP/1.1\r\nX-A:   v  \r\nHost: h\r\n\r\n");
    printf("%s: %d failing request(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
