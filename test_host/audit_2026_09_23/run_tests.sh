#!/usr/bin/env bash
# Audit 2026-09-23 repro tests: real src/websocket.c and src/http_parser.c,
# native gcc with ASan+UBSan. Exit 0 = all pass.
# Requires host mbedtls (libmbedtls-dev) for websocket.c's SHA-1/base64.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMP="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/httpd-audit-XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
CFLAGS=(-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
        -Wno-deprecated-declarations -I"$HERE/stubs" -I"$COMP/src" -I"$COMP/include")
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
rc=0
gcc "${CFLAGS[@]}" "$HERE/test_ws_frame_audit.c" "$COMP/src/websocket.c" -lmbedcrypto \
    -o "$OUT/test_ws_frame_audit" || exit 2
"$OUT/test_ws_frame_audit" || rc=1
gcc "${CFLAGS[@]}" "$HERE/test_http_parser_split_audit.c" "$COMP/src/http_parser.c" \
    -o "$OUT/test_http_parser_split_audit" || exit 2
"$OUT/test_http_parser_split_audit" || rc=1
exit $rc
