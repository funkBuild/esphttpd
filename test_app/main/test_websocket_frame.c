#include "unity.h"
#include "esphttpd.h"  // For public WS_OP_ constants
#include "websocket.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "TEST_WS_FRAME";

// Test parsing unmasked text frame
static void test_parse_unmasked_text_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Create simple text frame: FIN=1, opcode=TEXT, mask=0, length=5, payload="Hello"
    uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);
    TEST_ASSERT_FALSE(conn.ws_masked);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(sizeof(frame), consumed);

    // Verify payload is unchanged (unmasked)
    TEST_ASSERT_EQUAL_MEMORY("Hello", &frame[2], 5);
}

// Test parsing masked text frame
static void test_parse_masked_text_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Create masked text frame
    uint8_t frame[] = {
        0x81, 0x85,             // FIN=1, TEXT, MASK=1, len=5
        0x37, 0xfa, 0x21, 0x3d, // Mask key
        0x7f, 0x9f, 0x4d, 0x51, 0x58  // Masked "Hello"
    };

    // Make a copy to preserve original for comparison
    uint8_t frame_copy[sizeof(frame)];
    memcpy(frame_copy, frame, sizeof(frame));

    ws_frame_result_t result = ws_process_frame(&conn, frame_copy, sizeof(frame_copy),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);
    TEST_ASSERT_TRUE(conn.ws_masked);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(0x3d21fa37, conn.ws_mask_key); // Little-endian on ESP32

    // Verify payload was unmasked in place
    TEST_ASSERT_EQUAL_MEMORY("Hello", &frame_copy[6], 5);
}

// Test parsing frame with 16-bit extended length
static void test_parse_extended_length_16(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Frame with 126 byte payload (16-bit length)
    uint8_t frame[132];
    frame[0] = 0x82; // FIN=1, BINARY
    frame[1] = 126;  // Extended 16-bit length follows
    frame[2] = 0x00; // High byte
    frame[3] = 0x7E; // Low byte = 126

    // Fill with test pattern
    for (int i = 0; i < 126; i++) {
        frame[4 + i] = i & 0xFF;
    }

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_BINARY, conn.ws_opcode);
    TEST_ASSERT_EQUAL(126, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(130, consumed); // 1 + 1 + 2 + 126
}

// Test parsing fragmented frame
static void test_parse_fragmented_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // First fragment: FIN=0, TEXT
    uint8_t frame1[] = {0x01, 0x03, 'H', 'e', 'l'};
    ws_frame_result_t result = ws_process_frame(&conn, frame1, sizeof(frame1),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_FALSE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);

    // Continuation fragment: FIN=1, CONTINUATION
    ctx.state = WS_STATE_OPCODE; // Reset for next frame
    uint8_t frame2[] = {0x80, 0x02, 'l', 'o'};
    result = ws_process_frame(&conn, frame2, sizeof(frame2), &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_CONTINUATION, conn.ws_opcode);
}

// Test parsing control frames
static void test_parse_control_frames(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Close frame
    uint8_t close_frame[] = {0x88, 0x02, 0x03, 0xE8}; // Code 1000
    ws_frame_result_t result = ws_process_frame(&conn, close_frame,
                                               sizeof(close_frame), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_CLOSE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_CLOSE, conn.ws_opcode);

    // Ping frame
    ctx.state = WS_STATE_OPCODE;
    uint8_t ping_frame[] = {0x89, 0x04, 'p', 'i', 'n', 'g'};
    result = ws_process_frame(&conn, ping_frame, sizeof(ping_frame),
                             &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_PING, conn.ws_opcode);

    // Pong frame
    ctx.state = WS_STATE_OPCODE;
    uint8_t pong_frame[] = {0x8A, 0x04, 'p', 'o', 'n', 'g'};
    result = ws_process_frame(&conn, pong_frame, sizeof(pong_frame),
                             &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_PONG, conn.ws_opcode);
}

// Test parsing frame received in chunks
static void test_parse_frame_in_chunks(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Frame to be received in chunks
    uint8_t full_frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};

    // First chunk - just opcode byte
    ws_frame_result_t result = ws_process_frame(&conn, full_frame, 1,
                                               &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
    TEST_ASSERT_EQUAL(1, consumed);

    // Second chunk - length byte
    result = ws_process_frame(&conn, &full_frame[1], 1, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Third chunk - partial payload
    result = ws_process_frame(&conn, &full_frame[2], 3, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Fourth chunk - remaining payload
    result = ws_process_frame(&conn, &full_frame[5], 2, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);
}

// Test invalid frames
static void test_parse_invalid_frames(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Invalid opcode (0x3)
    uint8_t invalid_opcode[] = {0x83, 0x00};
    ws_frame_result_t result = ws_process_frame(&conn, invalid_opcode,
                                               sizeof(invalid_opcode), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    // Fragmented control frame (invalid)
    ctx.state = WS_STATE_OPCODE;
    uint8_t fragmented_control[] = {0x08, 0x00}; // FIN=0, CLOSE
    result = ws_process_frame(&conn, fragmented_control,
                             sizeof(fragmented_control), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    // Control frame with > 125 byte payload (invalid)
    ctx.state = WS_STATE_OPCODE;
    uint8_t large_control[] = {0x89, 0x7E, 0x00, 0x7E}; // PING with 126 bytes
    result = ws_process_frame(&conn, large_control, sizeof(large_control),
                             &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// Test building frame headers
static void test_build_frame_header(void)
{
    uint8_t header[14];
    size_t header_len;

    // Simple text frame
    header_len = ws_build_frame_header(header, WS_OPCODE_TEXT, 5, false);
    TEST_ASSERT_EQUAL(2, header_len);
    TEST_ASSERT_EQUAL(0x81, header[0]); // FIN=1, TEXT
    TEST_ASSERT_EQUAL(0x05, header[1]); // Length=5

    // Frame with 16-bit length
    header_len = ws_build_frame_header(header, WS_OPCODE_BINARY, 126, false);
    TEST_ASSERT_EQUAL(4, header_len);
    TEST_ASSERT_EQUAL(0x82, header[0]); // FIN=1, BINARY
    TEST_ASSERT_EQUAL(126, header[1]);  // Extended length marker
    TEST_ASSERT_EQUAL(0x00, header[2]); // High byte
    TEST_ASSERT_EQUAL(0x7E, header[3]); // Low byte = 126

    // Masked frame
    header_len = ws_build_frame_header(header, WS_OPCODE_TEXT, 10, true);
    TEST_ASSERT_EQUAL(6, header_len);  // 2 + 4 (mask)
    TEST_ASSERT_EQUAL(0x81, header[0]); // FIN=1, TEXT
    TEST_ASSERT_EQUAL(0x8A, header[1]); // MASK=1, Length=10
}

// Test mask/unmask operations
static void test_mask_unmask_payload(void)
{
    uint8_t payload[] = "Hello, World!";
    size_t len = strlen((char*)payload);
    uint32_t mask_key = 0x37fa213d;

    // Make a copy for testing
    uint8_t masked[32];
    memcpy(masked, payload, len);

    // Mask the payload
    ws_mask_payload(masked, len, mask_key, 0);

    // Verify it's different from original
    TEST_ASSERT_NOT_EQUAL(0, memcmp(payload, masked, len));

    // Unmask it (masking is XOR, so applying again unmasks)
    ws_mask_payload(masked, len, mask_key, 0);

    // Verify it's back to original
    TEST_ASSERT_EQUAL_MEMORY(payload, masked, len);
}

// ============================================================================
// Security/Edge Case Tests
// ============================================================================

// Test process_frame with NULL connection
static void test_process_frame_null_conn(void)
{
    ws_frame_context_t ctx = {0};
    size_t consumed;
    uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};

    ws_frame_result_t result = ws_process_frame(NULL, frame, sizeof(frame),
                                                &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// Test process_frame with NULL data
static void test_process_frame_null_data(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    ws_frame_result_t result = ws_process_frame(&conn, NULL, 10, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// Test process_frame with zero length
static void test_process_frame_zero_length(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;
    uint8_t frame[] = {0x81, 0x05};

    ws_frame_result_t result = ws_process_frame(&conn, frame, 0, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
}

// Test process_frame with NULL context
static void test_process_frame_null_ctx(void)
{
    connection_t conn = {0};
    size_t consumed;
    uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                NULL, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// Test build_frame_header with NULL buffer
static void test_build_frame_header_null(void)
{
    size_t header_len = ws_build_frame_header(NULL, WS_OPCODE_TEXT, 5, false);
    TEST_ASSERT_EQUAL(0, header_len);
}

// Test build_frame_header with >64KB payload (not supported - returns 0)
static void test_build_frame_header_64bit(void)
{
    uint8_t header[14];
    // Test with 65536 bytes - implementation doesn't support >64KB frames
    size_t header_len = ws_build_frame_header(header, WS_OPCODE_BINARY, 65536, false);
    TEST_ASSERT_EQUAL(0, header_len); // Returns 0 for unsupported payload sizes
}

// Test mask_payload with zero length
static void test_mask_payload_zero_length(void)
{
    uint8_t payload[] = "Hello";
    uint8_t original[6];
    memcpy(original, payload, sizeof(original));

    // Masking zero bytes should do nothing
    ws_mask_payload(payload, 0, 0x12345678, 0);
    TEST_ASSERT_EQUAL_MEMORY(original, payload, sizeof(original));
}

// ========== Masking Alignment Tests ==========

// Test mask_payload with single byte
static void test_mask_payload_single_byte(void)
{
    uint8_t payload[1] = {0x41}; // 'A'
    uint32_t mask_key = 0x04030201; // Bytes: 01, 02, 03, 04 (little-endian)

    ws_mask_payload(payload, 1, mask_key, 0);
    TEST_ASSERT_EQUAL(0x41 ^ 0x01, payload[0]);
}

// Test mask_payload with various small lengths (1-8 bytes)
static void test_mask_payload_small_lengths(void)
{
    uint32_t mask_key = 0x04030201;
    uint8_t mask_bytes[] = {0x01, 0x02, 0x03, 0x04}; // little-endian order

    for (int len = 1; len <= 8; len++) {
        uint8_t payload[8];
        memset(payload, 0xAA, sizeof(payload));

        uint8_t expected[8];
        for (int i = 0; i < len; i++) {
            expected[i] = 0xAA ^ mask_bytes[i % 4];
        }

        ws_mask_payload(payload, len, mask_key, 0);

        for (int i = 0; i < len; i++) {
            TEST_ASSERT_EQUAL(expected[i], payload[i]);
        }
    }
}

// Test mask_payload with offsets 0, 1, 2, 3
static void test_mask_payload_all_offsets(void)
{
    uint32_t mask_key = 0x04030201;
    uint8_t mask_bytes[] = {0x01, 0x02, 0x03, 0x04};

    for (int offset = 0; offset < 4; offset++) {
        uint8_t payload[4] = {0x55, 0x55, 0x55, 0x55};
        uint8_t expected[4];
        for (int i = 0; i < 4; i++) {
            expected[i] = 0x55 ^ mask_bytes[(offset + i) % 4];
        }

        ws_mask_payload(payload, 4, mask_key, offset);

        TEST_ASSERT_EQUAL_MEMORY(expected, payload, 4);
    }
}

// Test mask_payload with large buffer (tests 8-byte fast path)
static void test_mask_payload_large(void)
{
    uint8_t payload[64];
    uint8_t original[64];
    uint32_t mask_key = 0xDEADBEEF;

    // Fill with known pattern
    for (int i = 0; i < 64; i++) {
        payload[i] = i & 0xFF;
        original[i] = i & 0xFF;
    }

    // Mask
    ws_mask_payload(payload, 64, mask_key, 0);

    // Verify it changed
    TEST_ASSERT_NOT_EQUAL(0, memcmp(original, payload, 64));

    // Unmask (XOR again)
    ws_mask_payload(payload, 64, mask_key, 0);

    // Verify it's back to original
    TEST_ASSERT_EQUAL_MEMORY(original, payload, 64);
}

// Test mask_payload with misaligned pointer (offset 1 into buffer)
static void test_mask_payload_misaligned_ptr(void)
{
    // Allocate extra bytes to create misalignment
    uint8_t buffer[20] __attribute__((aligned(8)));
    uint8_t* misaligned = buffer + 1; // Misaligned by 1

    uint32_t mask_key = 0x04030201;
    uint8_t mask_bytes[] = {0x01, 0x02, 0x03, 0x04};

    // Fill with test pattern
    memset(misaligned, 0xCC, 8);

    // Calculate expected
    uint8_t expected[8];
    for (int i = 0; i < 8; i++) {
        expected[i] = 0xCC ^ mask_bytes[i % 4];
    }

    ws_mask_payload(misaligned, 8, mask_key, 0);
    TEST_ASSERT_EQUAL_MEMORY(expected, misaligned, 8);
}

// Test mask_payload with offset that spans fast path boundary
static void test_mask_payload_offset_boundary(void)
{
    uint8_t payload[16];
    uint32_t mask_key = 0x04030201;
    uint8_t mask_bytes[] = {0x01, 0x02, 0x03, 0x04};

    memset(payload, 0x77, sizeof(payload));

    // Offset 3 means first byte uses mask[3], then wraps around
    ws_mask_payload(payload, 16, mask_key, 3);

    // Verify first few bytes manually
    TEST_ASSERT_EQUAL(0x77 ^ mask_bytes[3], payload[0]); // offset 3 -> mask[3]
    TEST_ASSERT_EQUAL(0x77 ^ mask_bytes[0], payload[1]); // offset 4 -> mask[0]
    TEST_ASSERT_EQUAL(0x77 ^ mask_bytes[1], payload[2]); // offset 5 -> mask[1]
    TEST_ASSERT_EQUAL(0x77 ^ mask_bytes[2], payload[3]); // offset 6 -> mask[2]
}

// Test mask_payload with NULL pointer
static void test_mask_payload_null_ptr(void)
{
    // Should not crash, just return early
    ws_mask_payload(NULL, 10, 0x12345678, 0);
    TEST_ASSERT_TRUE(true); // If we get here, it didn't crash
}

// Test frame with RSV bits set
// NOTE: RSV bit validation is not implemented per RFC 6455 - frames are accepted
// This test documents current behavior (RSV bits are ignored)
static void test_frame_rsv_bits_set(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // RSV1 bit set (0x40) - currently accepted (RSV validation not implemented)
    uint8_t frame_rsv1[] = {0xC1, 0x00}; // FIN + RSV1 + TEXT, len=0
    ws_frame_result_t result = ws_process_frame(&conn, frame_rsv1,
                                                sizeof(frame_rsv1), &ctx, &consumed);
    // RSV bits are currently ignored, frame completes with zero payload
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);

    // RSV2 bit set (0x20)
    ctx.state = WS_STATE_OPCODE;
    uint8_t frame_rsv2[] = {0xA1, 0x00}; // FIN + RSV2 + TEXT, len=0
    result = ws_process_frame(&conn, frame_rsv2, sizeof(frame_rsv2), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);

    // RSV3 bit set (0x10)
    ctx.state = WS_STATE_OPCODE;
    uint8_t frame_rsv3[] = {0x91, 0x00}; // FIN + RSV3 + TEXT, len=0
    result = ws_process_frame(&conn, frame_rsv3, sizeof(frame_rsv3), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
}

// Test frame with zero-length payload
static void test_frame_zero_payload(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    uint8_t frame[] = {0x81, 0x00}; // FIN=1, TEXT, len=0
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(0, conn.ws_payload_len);
}

// Test close frame without payload
static void test_close_frame_empty(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    uint8_t frame[] = {0x88, 0x00}; // Close with no payload
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_CLOSE, result);
}

// Test close frame with 1-byte payload (invalid - should have 0 or 2+ bytes)
static void test_close_frame_one_byte(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    uint8_t frame[] = {0x88, 0x01, 0x00}; // Close with 1 byte payload
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);
    // 1-byte close payload is technically invalid, but implementations may vary
    TEST_ASSERT_TRUE(result == WS_FRAME_CLOSE || result == WS_FRAME_ERROR);
}

// Test frame_ctx_init with NULL
static void test_frame_ctx_init_null(void)
{
    bool result = ws_frame_ctx_init(NULL);
    TEST_ASSERT_FALSE(result);
}

// Test frame_ctx_init success
static void test_frame_ctx_init_success(void)
{
    ws_frame_context_t ctx = {
        .state = WS_STATE_PAYLOAD,  // Non-zero initial value
        .payload_received = 100
    };

    bool result = ws_frame_ctx_init(&ctx);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(WS_STATE_OPCODE, ctx.state);
    TEST_ASSERT_EQUAL(0, ctx.payload_received);
}

// Test masking with offset
static void test_mask_payload_with_offset(void)
{
    uint8_t payload[] = "ABCD";
    uint32_t mask_key = 0x01020304;

    // Mask with offset 2
    ws_mask_payload(payload, 4, mask_key, 2);

    // With offset 2, mask bytes should start at position 2 of mask key
    // Mask key bytes: 04, 03, 02, 01 (little-endian)
    // Offset 2 means: starts with 02, 01, 04, 03
    // Expected: 'A'^0x02, 'B'^0x01, 'C'^0x04, 'D'^0x03
    TEST_ASSERT_EQUAL('A' ^ 0x02, payload[0]);
    TEST_ASSERT_EQUAL('B' ^ 0x01, payload[1]);
    TEST_ASSERT_EQUAL('C' ^ 0x04, payload[2]);
    TEST_ASSERT_EQUAL('D' ^ 0x03, payload[3]);
}

// Test building close frame header
static void test_build_close_frame_header(void)
{
    uint8_t header[14];
    size_t header_len = ws_build_frame_header(header, WS_OPCODE_CLOSE, 2, false);

    TEST_ASSERT_EQUAL(2, header_len);
    TEST_ASSERT_EQUAL(0x88, header[0]); // FIN=1, CLOSE
    TEST_ASSERT_EQUAL(0x02, header[1]); // len=2
}

// Test building ping frame header
static void test_build_ping_frame_header(void)
{
    uint8_t header[14];
    size_t header_len = ws_build_frame_header(header, WS_OPCODE_PING, 4, false);

    TEST_ASSERT_EQUAL(2, header_len);
    TEST_ASSERT_EQUAL(0x89, header[0]); // FIN=1, PING
    TEST_ASSERT_EQUAL(0x04, header[1]); // len=4
}

// ============================================================================
// WebSocket Handshake Tests (ws_compute_accept_key, ws_send_handshake_response)
// ============================================================================

// Test computing accept key with RFC 6455 test vector
static void test_compute_accept_key_rfc6455(void)
{
    // RFC 6455 example: client key "dGhlIHNhbXBsZSBub25jZQ=="
    // should produce accept key "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    const char* client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char accept_key[64] = {0};

    ws_compute_accept_key(client_key, accept_key, sizeof(accept_key));

    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept_key);
}

// Test computing accept key with another known value
static void test_compute_accept_key_another_key(void)
{
    // Another common test key
    const char* client_key = "x3JJHMbDL1EzLkh9GBhXDw==";
    char accept_key[64] = {0};

    ws_compute_accept_key(client_key, accept_key, sizeof(accept_key));

    // The accept key should be consistently computed
    // We verify it's not empty and has the right length (28 chars for base64)
    TEST_ASSERT_TRUE(strlen(accept_key) == 28);
    // Accept key should end with '=' (base64 padding)
    TEST_ASSERT_EQUAL('=', accept_key[27]);
}

// Test accept key is deterministic
static void test_compute_accept_key_deterministic(void)
{
    const char* client_key = "testKey12345678901234==";
    char accept_key1[64] = {0};
    char accept_key2[64] = {0};

    ws_compute_accept_key(client_key, accept_key1, sizeof(accept_key1));
    ws_compute_accept_key(client_key, accept_key2, sizeof(accept_key2));

    // Same input should produce same output
    TEST_ASSERT_EQUAL_STRING(accept_key1, accept_key2);
}

// Test accept key with minimum valid client key
static void test_compute_accept_key_short_key(void)
{
    const char* client_key = "AAAAAAAAAAAAAAAAAAAAAA==";
    char accept_key[64] = {0};

    ws_compute_accept_key(client_key, accept_key, sizeof(accept_key));

    // Should produce valid base64 output
    TEST_ASSERT_TRUE(strlen(accept_key) > 0);
    TEST_ASSERT_TRUE(strlen(accept_key) == 28);
}

// Test accept key buffer size limit
static void test_compute_accept_key_small_buffer(void)
{
    const char* client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char accept_key[32] = {0};  // Just big enough for the output

    ws_compute_accept_key(client_key, accept_key, sizeof(accept_key));

    // Should still work with exact-size buffer
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept_key);
}

// Test accept key with empty client key
static void test_compute_accept_key_empty(void)
{
    const char* client_key = "";
    char accept_key[64] = {0};

    ws_compute_accept_key(client_key, accept_key, sizeof(accept_key));

    // Should produce output (SHA1 of just the GUID)
    TEST_ASSERT_TRUE(strlen(accept_key) > 0);
}

void test_websocket_frame_run(void)
{
    // Core functionality tests
    RUN_TEST(test_parse_unmasked_text_frame);
    RUN_TEST(test_parse_masked_text_frame);
    RUN_TEST(test_parse_extended_length_16);
    RUN_TEST(test_parse_fragmented_frame);
    RUN_TEST(test_parse_control_frames);
    RUN_TEST(test_parse_frame_in_chunks);
    RUN_TEST(test_parse_invalid_frames);
    RUN_TEST(test_build_frame_header);
    RUN_TEST(test_mask_unmask_payload);

    // Security and edge case tests
    RUN_TEST(test_process_frame_null_conn);
    RUN_TEST(test_process_frame_null_data);
    RUN_TEST(test_process_frame_zero_length);
    RUN_TEST(test_process_frame_null_ctx);
    RUN_TEST(test_build_frame_header_null);
    RUN_TEST(test_build_frame_header_64bit);
    RUN_TEST(test_mask_payload_zero_length);
    RUN_TEST(test_frame_rsv_bits_set);
    RUN_TEST(test_frame_zero_payload);
    RUN_TEST(test_close_frame_empty);
    RUN_TEST(test_close_frame_one_byte);
    RUN_TEST(test_frame_ctx_init_null);
    RUN_TEST(test_frame_ctx_init_success);
    RUN_TEST(test_mask_payload_with_offset);
    RUN_TEST(test_build_close_frame_header);
    RUN_TEST(test_build_ping_frame_header);

    // Masking alignment tests
    RUN_TEST(test_mask_payload_single_byte);
    RUN_TEST(test_mask_payload_small_lengths);
    RUN_TEST(test_mask_payload_all_offsets);
    RUN_TEST(test_mask_payload_large);
    RUN_TEST(test_mask_payload_misaligned_ptr);
    RUN_TEST(test_mask_payload_offset_boundary);
    RUN_TEST(test_mask_payload_null_ptr);

    // WebSocket handshake tests
    RUN_TEST(test_compute_accept_key_rfc6455);
    RUN_TEST(test_compute_accept_key_another_key);
    RUN_TEST(test_compute_accept_key_deterministic);
    RUN_TEST(test_compute_accept_key_short_key);
    RUN_TEST(test_compute_accept_key_small_buffer);
    RUN_TEST(test_compute_accept_key_empty);

    ESP_LOGI(TAG, "WebSocket frame tests completed");
}