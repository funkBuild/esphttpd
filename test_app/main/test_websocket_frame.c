#include "unity.h"
#include "esphttpd.h"  // For public WS_OP_ constants
#include "websocket.h"
#include <string.h>
#include "esp_log.h"

static const char* TAG = "TEST_WS_FRAME";

// Test that an unmasked client data frame is rejected (RFC 6455 5.1).
// A server MUST fail the connection on any unmasked frame from a client.
static void test_parse_unmasked_frame_rejected(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Simple text frame: FIN=1, opcode=TEXT, mask=0, length=5, payload="Hello"
    // Unmasked (mask bit clear) => must be rejected.
    uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test parsing a normal masked data frame (mask key 0 leaves payload intact)
static void test_parse_masked_data_frame_ok(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Masked text frame with an all-zero mask key: masked bytes == plaintext.
    uint8_t frame[] = {
        0x81, 0x85,             // FIN=1, TEXT, MASK=1, len=5
        0x00, 0x00, 0x00, 0x00, // Mask key (0 => payload unchanged)
        'H', 'e', 'l', 'l', 'o'
    };

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);
    TEST_ASSERT_TRUE(conn.ws_masked);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(sizeof(frame), consumed);
    TEST_ASSERT_EQUAL_MEMORY("Hello", &frame[6], 5);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
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

    // Masked frame with 126 byte payload (16-bit length).
    // Mask key is all-zero, so masked payload == plaintext.
    uint8_t frame[136];
    frame[0] = 0x82;        // FIN=1, BINARY
    frame[1] = 0x80 | 126;  // MASK=1, extended 16-bit length follows
    frame[2] = 0x00;        // High byte
    frame[3] = 0x7E;        // Low byte = 126
    frame[4] = 0x00;        // Mask key
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0x00;

    // Fill with test pattern
    for (int i = 0; i < 126; i++) {
        frame[8 + i] = i & 0xFF;
    }

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_BINARY, conn.ws_opcode);
    TEST_ASSERT_EQUAL(126, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(134, consumed); // 1 + 1 + 2 + 4 (mask) + 126

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test parsing 64-bit extended length (len byte 127 + 8 length bytes)
static void test_parse_extended_length_64(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Masked frame with 300-byte payload declared via the 64-bit length field.
    // Mask key is all-zero, so masked payload == plaintext.
    enum { WS64_PAYLOAD = 300 };
    static uint8_t frame[14 + WS64_PAYLOAD];
    frame[0] = 0x82;        // FIN=1, BINARY
    frame[1] = 0x80 | 127;  // MASK=1, 64-bit extended length follows
    memset(&frame[2], 0, 8);
    frame[8] = (WS64_PAYLOAD >> 8) & 0xFF;
    frame[9] = WS64_PAYLOAD & 0xFF;
    memset(&frame[10], 0, 4);  // Mask key (0 => payload unchanged)
    for (int i = 0; i < WS64_PAYLOAD; i++) {
        frame[14 + i] = i & 0xFF;
    }

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_BINARY, conn.ws_opcode);
    TEST_ASSERT_EQUAL(WS64_PAYLOAD, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(sizeof(frame), consumed);
    TEST_ASSERT_EQUAL(WS64_PAYLOAD, ctx.payload_received);
    // Whole payload arrived in one slice: zero-copy contract means
    // payload_ptr points into the input buffer (no accumulation copy).
    TEST_ASSERT_EQUAL_PTR(&frame[14], ctx.payload_ptr);
    TEST_ASSERT_EQUAL(42, ctx.payload_ptr[42]);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// 64-bit length above the 64KB cap must be rejected, not truncated
static void test_parse_extended_length_64_too_large(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    uint8_t frame[10];
    frame[0] = 0x82;
    frame[1] = 0x80 | 127;  // MASK=1 so the too-large check (not the mask rule) fires
    memset(&frame[2], 0, 8);
    frame[6] = 0x00;
    frame[7] = 0x01;  // 0x10000 = 65536 (> 65535 cap)

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    free(ctx.payload_buffer);
}

// 64-bit length with any of the upper 32 bits set must be rejected early
// (would otherwise overflow size arithmetic on a 32-bit target)
static void test_parse_extended_length_64_upper_bits(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    uint8_t frame[10];
    frame[0] = 0x82;
    frame[1] = 0x80 | 127;  // MASK=1 so the upper-bits check (not the mask rule) fires
    memset(&frame[2], 0, 8);
    frame[2] = 0x01;  // bit in the upper 32 bits of the 64-bit length

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                               &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    free(ctx.payload_buffer);
}

// 64-bit length header delivered byte-by-byte must resume correctly
static void test_parse_extended_length_64_split(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Masked frame (14-byte header: opcode + len + 8 length + 4 mask).
    // Mask key is all-zero, so masked payload == plaintext.
    enum { WS64S_PAYLOAD = 200 };
    static uint8_t frame[14 + WS64S_PAYLOAD];
    frame[0] = 0x82;
    frame[1] = 0x80 | 127;  // MASK=1
    memset(&frame[2], 0, 8);
    frame[9] = WS64S_PAYLOAD;  // 200 fits in the low byte
    memset(&frame[10], 0, 4);  // Mask key
    for (int i = 0; i < WS64S_PAYLOAD; i++) {
        frame[14 + i] = i & 0xFF;
    }

    // Feed the 14-byte header one byte at a time
    size_t offset = 0;
    ws_frame_result_t result = WS_FRAME_NEED_MORE;
    for (int i = 0; i < 14; i++) {
        result = ws_process_frame(&conn, frame + offset, 1, &ctx, &consumed);
        TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
        offset += consumed;
    }
    // Then the full payload
    result = ws_process_frame(&conn, frame + offset, sizeof(frame) - offset,
                              &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS64S_PAYLOAD, conn.ws_payload_len);
    TEST_ASSERT_EQUAL(WS64S_PAYLOAD, ctx.payload_received);
    // Only the header was split - the whole payload arrived in the final
    // slice, so the zero-copy contract applies (payload_ptr -> input).
    TEST_ASSERT_EQUAL_PTR(frame + offset, ctx.payload_ptr);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test parsing fragmented frame
static void test_parse_fragmented_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // First fragment: FIN=0, TEXT (masked, mask key 0 => payload unchanged)
    uint8_t frame1[] = {0x01, 0x83, 0x00, 0x00, 0x00, 0x00, 'H', 'e', 'l'};
    ws_frame_result_t result = ws_process_frame(&conn, frame1, sizeof(frame1),
                                               &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_FALSE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_TEXT, conn.ws_opcode);

    // Continuation fragment: FIN=1, CONTINUATION (masked)
    ctx.state = WS_STATE_OPCODE; // Reset for next frame
    uint8_t frame2[] = {0x80, 0x82, 0x00, 0x00, 0x00, 0x00, 'l', 'o'};
    result = ws_process_frame(&conn, frame2, sizeof(frame2), &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_TRUE(conn.ws_fin);
    TEST_ASSERT_EQUAL(WS_OPCODE_CONTINUATION, conn.ws_opcode);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test parsing control frames
static void test_parse_control_frames(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Close frame (masked, mask key 0 => payload unchanged). Code 1000.
    uint8_t close_frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8};
    ws_frame_result_t result = ws_process_frame(&conn, close_frame,
                                               sizeof(close_frame), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_CLOSE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_CLOSE, conn.ws_opcode);

    // Ping frame (masked)
    ctx.state = WS_STATE_OPCODE;
    uint8_t ping_frame[] = {0x89, 0x84, 0x00, 0x00, 0x00, 0x00, 'p', 'i', 'n', 'g'};
    result = ws_process_frame(&conn, ping_frame, sizeof(ping_frame),
                             &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_PING, conn.ws_opcode);

    // Pong frame (masked)
    ctx.state = WS_STATE_OPCODE;
    uint8_t pong_frame[] = {0x8A, 0x84, 0x00, 0x00, 0x00, 0x00, 'p', 'o', 'n', 'g'};
    result = ws_process_frame(&conn, pong_frame, sizeof(pong_frame),
                             &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_PONG, conn.ws_opcode);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test parsing frame received in chunks
static void test_parse_frame_in_chunks(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Masked frame to be received in chunks (mask key 0 => payload unchanged)
    uint8_t full_frame[] = {0x81, 0x85, 0x00, 0x00, 0x00, 0x00,
                            'H', 'e', 'l', 'l', 'o'};

    // First chunk - just opcode byte
    ws_frame_result_t result = ws_process_frame(&conn, full_frame, 1,
                                               &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
    TEST_ASSERT_EQUAL(1, consumed);

    // Second chunk - length byte
    result = ws_process_frame(&conn, &full_frame[1], 1, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Third chunk - mask key (4 bytes)
    result = ws_process_frame(&conn, &full_frame[2], 4, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Fourth chunk - partial payload
    result = ws_process_frame(&conn, &full_frame[6], 3, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Fifth chunk - remaining payload
    result = ws_process_frame(&conn, &full_frame[9], 2, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(5, conn.ws_payload_len);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
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

    // Control frame using the 126 extended-length form (invalid: control
    // frames must be <=125 and must not use the extended length forms).
    // Masked so the extended-length rule fires, not the unmasked-frame rule.
    ctx.state = WS_STATE_OPCODE;
    uint8_t large_control[] = {0x89, 0xFE, 0x00, 0x7E}; // PING, MASK=1, len form 126
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
// Issue #36: RSV bits must be rejected per RFC 6455 (no extensions negotiated)
static void test_frame_rsv_bits_set(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // RSV1 bit set (0x40) - must be rejected
    uint8_t frame_rsv1[] = {0xC1, 0x00}; // FIN + RSV1 + TEXT, len=0
    ws_frame_result_t result = ws_process_frame(&conn, frame_rsv1,
                                                sizeof(frame_rsv1), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    // RSV2 bit set (0x20)
    ctx.state = WS_STATE_OPCODE;
    uint8_t frame_rsv2[] = {0xA1, 0x00}; // FIN + RSV2 + TEXT, len=0
    result = ws_process_frame(&conn, frame_rsv2, sizeof(frame_rsv2), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);

    // RSV3 bit set (0x10)
    ctx.state = WS_STATE_OPCODE;
    uint8_t frame_rsv3[] = {0x91, 0x00}; // FIN + RSV3 + TEXT, len=0
    result = ws_process_frame(&conn, frame_rsv3, sizeof(frame_rsv3), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// Test frame with zero-length payload
static void test_frame_zero_payload(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // FIN=1, TEXT, MASK=1, len=0 (mask key 0)
    uint8_t frame[] = {0x81, 0x80, 0x00, 0x00, 0x00, 0x00};
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

    // Close with no payload (masked, mask key 0)
    uint8_t frame[] = {0x88, 0x80, 0x00, 0x00, 0x00, 0x00};
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_CLOSE, result);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// Test close frame with 1-byte payload (invalid - should have 0 or 2+ bytes)
static void test_close_frame_one_byte(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Close with 1 byte payload (masked, mask key 0)
    uint8_t frame[] = {0x88, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00};
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);
    // 1-byte close payload is technically invalid, but implementations may vary
    TEST_ASSERT_TRUE(result == WS_FRAME_CLOSE || result == WS_FRAME_ERROR);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
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

    // Clean up allocated buffer
    if (ctx.payload_buffer) {
        free(ctx.payload_buffer);
    }
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

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept_key);
}

// Test computing accept key with another known value
static void test_compute_accept_key_another_key(void)
{
    // Another common test key
    const char* client_key = "x3JJHMbDL1EzLkh9GBhXDw==";
    char accept_key[64] = {0};

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    // The accept key should be consistently computed
    // We verify it's not empty and has the right length (28 chars for base64)
    TEST_ASSERT_TRUE(strlen(accept_key) == 28);
    // Accept key should end with '=' (base64 padding)
    TEST_ASSERT_EQUAL('=', accept_key[27]);
}

// Test accept key output is always base64 format
static void test_ws_accept_key_format(void)
{
    char accept_key[64] = {0};
    const char* client_key = "dGhlIHNhbXBsZSBub25jZQ==";

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    // Base64 encoded strings only contain: A-Z, a-z, 0-9, +, /, =
    for (size_t i = 0; i < strlen(accept_key); i++) {
        char c = accept_key[i];
        bool valid = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '+' || c == '/' || c == '=';
        TEST_ASSERT_TRUE_MESSAGE(valid, "Accept key contains invalid base64 character");
    }

    // Should end with = padding (SHA1 is 20 bytes, base64 is 28 chars with 1 pad)
    TEST_ASSERT_EQUAL('=', accept_key[strlen(accept_key) - 1]);
}

// Test accept key is deterministic
static void test_compute_accept_key_deterministic(void)
{
    const char* client_key = "testKey12345678901234==";
    char accept_key1[64] = {0};
    char accept_key2[64] = {0};

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key1, sizeof(accept_key1), NULL));
    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key2, sizeof(accept_key2), NULL));

    // Same input should produce same output
    TEST_ASSERT_EQUAL_STRING(accept_key1, accept_key2);
}

// Test accept key with minimum valid client key
static void test_compute_accept_key_short_key(void)
{
    const char* client_key = "AAAAAAAAAAAAAAAAAAAAAA==";
    char accept_key[64] = {0};

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    // Should produce valid base64 output
    TEST_ASSERT_TRUE(strlen(accept_key) > 0);
    TEST_ASSERT_TRUE(strlen(accept_key) == 28);
}

// Test accept key buffer size limit
static void test_compute_accept_key_small_buffer(void)
{
    const char* client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    char accept_key[32] = {0};  // Just big enough for the output

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    // Should still work with exact-size buffer
    TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept_key);
}

// Test accept key with empty client key
static void test_compute_accept_key_empty(void)
{
    const char* client_key = "";
    char accept_key[64] = {0};

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key(client_key, accept_key, sizeof(accept_key), NULL));

    // Should produce output (SHA1 of just the GUID)
    TEST_ASSERT_TRUE(strlen(accept_key) > 0);
}

// Test different client keys produce different accept keys
static void test_ws_accept_key_uniqueness(void)
{
    char accept1[64] = {0};
    char accept2[64] = {0};
    char accept3[64] = {0};

    TEST_ASSERT_EQUAL(0, ws_compute_accept_key("key1AAAAAAAAAAAAAAAAaa==", accept1, sizeof(accept1), NULL));
    TEST_ASSERT_EQUAL(0, ws_compute_accept_key("key2BBBBBBBBBBBBBBBBBB==", accept2, sizeof(accept2), NULL));
    TEST_ASSERT_EQUAL(0, ws_compute_accept_key("key3CCCCCCCCCCCCCCCCCC==", accept3, sizeof(accept3), NULL));

    // All should be different
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(accept1, accept2), "accept1 and accept2 should differ");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(accept2, accept3), "accept2 and accept3 should differ");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(accept1, accept3), "accept1 and accept3 should differ");
}

// ========== Issue #17: Fast-path goto must set ctx->state ==========

// Test that fast-path partial receive correctly resumes parsing
static void test_fast_path_partial_payload(void) {
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Build a masked frame with 20 bytes payload, but only provide header + 5 bytes
    // This forces the fast path to start, but not complete, so ctx->state must be WS_STATE_PAYLOAD
    uint8_t frame_part1[11];
    frame_part1[0] = 0x81;         // FIN=1, TEXT
    frame_part1[1] = 0x80 | 20;    // MASK=1, len=20
    frame_part1[2] = 0x01;         // Mask key byte 0
    frame_part1[3] = 0x02;         // Mask key byte 1
    frame_part1[4] = 0x03;         // Mask key byte 2
    frame_part1[5] = 0x04;         // Mask key byte 3
    // Only 5 payload bytes (need 20 total)
    memset(&frame_part1[6], 0xAA, 5);

    ws_frame_result_t result = ws_process_frame(&conn, frame_part1, 11, &ctx, &consumed);

    // Should need more data since only 5 of 20 payload bytes provided
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
    TEST_ASSERT_EQUAL(11, consumed);
    // ctx->state MUST be WS_STATE_PAYLOAD so next call resumes correctly
    TEST_ASSERT_EQUAL(WS_STATE_PAYLOAD, ctx.state);

    // Now provide the remaining 15 bytes
    uint8_t frame_part2[15];
    memset(frame_part2, 0xBB, 15);

    result = ws_process_frame(&conn, frame_part2, 15, &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(15, consumed);
    TEST_ASSERT_EQUAL(20, conn.ws_payload_len);
    // Payload spanned two calls: accumulation path, payload_ptr must
    // reference the internal payload buffer.
    TEST_ASSERT_EQUAL_PTR(ctx.payload_buffer, ctx.payload_ptr);

    // Clean up
    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// ========== Issue #36: RSV bits rejected in fast path ==========
static void test_rsv_bits_fast_path_rejected(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Fast path: masked frame with RSV1 set (6+ bytes available)
    uint8_t frame[] = {
        0xC1, 0x82,             // FIN + RSV1 + TEXT, MASK=1, len=2
        0x00, 0x00, 0x00, 0x00, // Mask key
        0x41, 0x42              // Payload
    };

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_ERROR, result);
}

// ========== Issue #34: Split PING payload should not send partial PONGs ==========
static void test_split_ping_payload(void)
{
    connection_t conn = {0};
    conn.fd = -1;  // Prevent actual send attempts
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // PING frame with 4-byte payload, split across two calls (masked, mask key 0)
    // First part: header (opcode + length + mask) but only 2 bytes of payload
    uint8_t part1[] = {0x89, 0x84, 0x00, 0x00, 0x00, 0x00, 'P', 'I'};

    ws_frame_result_t result = ws_process_frame(&conn, part1, sizeof(part1),
                                                &ctx, &consumed);
    // Should need more data (payload not complete)
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);

    // Second part: remaining 2 bytes of payload
    uint8_t part2[] = {'N', 'G'};
    result = ws_process_frame(&conn, part2, sizeof(part2), &ctx, &consumed);

    // Should complete the frame and handle the control frame
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// ========== Issue #37: NULL client_key should not crash ==========
static void test_compute_accept_key_null(void)
{
    char accept[32];
    size_t out_len;

    int ret = ws_compute_accept_key(NULL, accept, sizeof(accept), &out_len);
    TEST_ASSERT_NOT_EQUAL(0, ret);  // Should return error, not crash

    ret = ws_compute_accept_key("key", NULL, 32, &out_len);
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

// ========== Issue #60: Invalid close status codes rejected ==========

static void test_close_frame_invalid_status_codes(void)
{
    // Test code 999 (below valid range 1000-4999)
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        // Close frame with status code 999 (0x03E7), masked (mask key 0)
        uint8_t frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE7};
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_ERROR, result,
            "Close with code 999 should be rejected");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }

    // Test code 1004 (reserved)
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        uint8_t frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xEC};  // 1004, masked
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_ERROR, result,
            "Close with code 1004 should be rejected");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }

    // Test code 5000 (above valid range)
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        uint8_t frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x13, 0x88};  // 5000, masked
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_ERROR, result,
            "Close with code 5000 should be rejected");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }
}

static void test_close_frame_valid_status_codes(void)
{
    // Test code 1000 (normal closure) - should be accepted
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        uint8_t frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8};  // 1000, masked
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_CLOSE, result,
            "Close with code 1000 should be accepted");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }

    // Test code 4999 (private use, max valid) - should be accepted
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        uint8_t frame[] = {0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x13, 0x87};  // 4999, masked
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_CLOSE, result,
            "Close with code 4999 should be accepted");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }
}

// ========== RFC 6455 5.5: control frames must not use extended length ==========

// A control frame that declares its length via the 126 (16-bit) or 127 (64-bit)
// extended forms must be rejected, even before the extended length is read.
static void test_control_frame_extended_length_rejected(void)
{
    // CLOSE using the 126 (16-bit) length form => reject
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        // 0x88 CLOSE, MASK=1, len form 126; 16-bit length says 130
        uint8_t frame[] = {0x88, 0xFE, 0x00, 0x82};
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_ERROR, result,
            "CLOSE with 16-bit extended length must be rejected");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }

    // PING using the 127 (64-bit) length form => reject
    {
        connection_t conn = {0};
        ws_frame_context_t ctx = {0};
        size_t consumed;
        // 0x89 PING, MASK=1, len form 127; 64-bit length says 200
        uint8_t frame[] = {0x89, 0xFF, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0xC8};
        ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                    &ctx, &consumed);
        TEST_ASSERT_EQUAL_MESSAGE(WS_FRAME_ERROR, result,
            "PING with 64-bit extended length must be rejected");
        if (ctx.payload_buffer) free(ctx.payload_buffer);
    }
}

// A valid masked control frame (payload <= 125) must still parse correctly.
static void test_masked_control_frame_ok(void)
{
    connection_t conn = {0};
    conn.fd = -1;  // Prevent actual send attempts on PING->PONG
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // PING, MASK=1, len=4 (mask key 0 => payload unchanged)
    uint8_t frame[] = {0x89, 0x84, 0x00, 0x00, 0x00, 0x00, 'p', 'i', 'n', 'g'};
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_OPCODE_PING, conn.ws_opcode);
    TEST_ASSERT_TRUE(conn.ws_masked);
    TEST_ASSERT_EQUAL(4, conn.ws_payload_len);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// ============================================================================
// payload_ptr zero-copy contract + payload buffer shrink tests
// ============================================================================

// Single-slice masked data frame: payload_ptr must point INTO the input
// buffer (zero-copy) and reference the unmasked payload. No accumulation
// buffer may be allocated for this path.
static void test_payload_ptr_zero_copy_single_slice(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Masked text frame, real (non-zero) mask key: "Hello"
    uint8_t frame[] = {
        0x81, 0x85,             // FIN=1, TEXT, MASK=1, len=5
        0x37, 0xfa, 0x21, 0x3d, // Mask key
        0x7f, 0x9f, 0x4d, 0x51, 0x58  // Masked "Hello"
    };

    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(sizeof(frame), consumed);
    TEST_ASSERT_EQUAL(5, ctx.payload_received);
    // Zero-copy: payload_ptr points into the INPUT buffer, at the
    // (in-place unmasked) payload start.
    TEST_ASSERT_EQUAL_PTR(&frame[6], ctx.payload_ptr);
    TEST_ASSERT_EQUAL_MEMORY("Hello", ctx.payload_ptr, 5);
    // Proof no copy happened: the accumulation buffer was never allocated.
    TEST_ASSERT_NULL(ctx.payload_buffer);
}

// Frame whose payload spans two ws_process_frame calls: must take the
// accumulation path, with payload_ptr == payload_buffer and correct
// (unmask-offset-aware) content.
static void test_payload_ptr_split_frame_accumulates(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    static const char plain[] = "ABCDEFGH";  // 8 bytes
    uint8_t frame[14];
    frame[0] = 0x81;        // FIN=1, TEXT
    frame[1] = 0x80 | 8;    // MASK=1, len=8
    frame[2] = 0x11;        // Mask key
    frame[3] = 0x22;
    frame[4] = 0x33;
    frame[5] = 0x44;
    for (int i = 0; i < 8; i++) {
        frame[6 + i] = (uint8_t)plain[i] ^ frame[2 + (i % 4)];
    }

    // First call: header + 3 of 8 payload bytes
    ws_frame_result_t result = ws_process_frame(&conn, frame, 9, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
    TEST_ASSERT_EQUAL(9, consumed);

    // Second call: remaining 5 payload bytes
    result = ws_process_frame(&conn, &frame[9], 5, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(5, consumed);
    TEST_ASSERT_EQUAL(8, ctx.payload_received);

    // Accumulation path: payload_ptr must reference the internal buffer
    TEST_ASSERT_NOT_NULL(ctx.payload_buffer);
    TEST_ASSERT_EQUAL_PTR(ctx.payload_buffer, ctx.payload_ptr);
    TEST_ASSERT_EQUAL_MEMORY(plain, ctx.payload_ptr, 8);

    free(ctx.payload_buffer);
}

// Zero-length data frame: payload_ptr must be NULL (the only case where
// NULL is allowed by the contract).
static void test_payload_ptr_zero_length_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // Poison payload_ptr to prove it is (re)set on this completion
    ctx.payload_ptr = (const uint8_t*)(uintptr_t)0xDEADBEEF;

    // FIN=1, TEXT, MASK=1, len=0
    uint8_t frame[] = {0x81, 0x80, 0x00, 0x00, 0x00, 0x00};
    ws_frame_result_t result = ws_process_frame(&conn, frame, sizeof(frame),
                                                &ctx, &consumed);

    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(0, ctx.payload_received);
    TEST_ASSERT_NULL(ctx.payload_ptr);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// After a large (>WS_PAYLOAD_SHRINK_THRESHOLD) frame grows the payload
// buffer, starting the next frame must shrink it back to
// WS_DEFAULT_BUFFER_SIZE.
static void test_payload_buffer_shrinks_after_large_frame(void)
{
    connection_t conn = {0};
    ws_frame_context_t ctx = {0};
    size_t consumed;

    // 2000-byte masked binary frame (16-bit extended length, mask key 0).
    // Delivered in two slices so it takes the accumulation path and
    // actually grows payload_buffer (a single slice would be zero-copy).
    enum { BIG_PAYLOAD = 2000, BIG_HDR = 8, SPLIT_AT = BIG_HDR + 1200 };
    static uint8_t frame[BIG_HDR + BIG_PAYLOAD];
    frame[0] = 0x82;                     // FIN=1, BINARY
    frame[1] = 0x80 | 126;               // MASK=1, 16-bit length
    frame[2] = (BIG_PAYLOAD >> 8) & 0xFF;
    frame[3] = BIG_PAYLOAD & 0xFF;
    memset(&frame[4], 0, 4);             // Mask key (0 => payload unchanged)
    for (int i = 0; i < BIG_PAYLOAD; i++) {
        frame[BIG_HDR + i] = i & 0xFF;
    }

    // Slice 1: header + 1200 payload bytes
    ws_frame_result_t result = ws_process_frame(&conn, frame, SPLIT_AT,
                                                &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_NEED_MORE, result);
    TEST_ASSERT_EQUAL(SPLIT_AT, consumed);
    TEST_ASSERT_TRUE(ctx.payload_buffer_size > WS_PAYLOAD_SHRINK_THRESHOLD);

    // Slice 2: remaining payload
    result = ws_process_frame(&conn, frame + SPLIT_AT,
                              sizeof(frame) - SPLIT_AT, &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(BIG_PAYLOAD, ctx.payload_received);
    TEST_ASSERT_EQUAL_PTR(ctx.payload_buffer, ctx.payload_ptr);
    TEST_ASSERT_EQUAL(1500 & 0xFF, ctx.payload_ptr[1500]);

    // Mimic the production caller's per-frame reset, then start a new
    // small frame: the ratcheted buffer must shrink back to default.
    ctx.state = WS_STATE_OPCODE;
    ctx.payload_received = 0;

    uint8_t small[] = {0x81, 0x85, 0x00, 0x00, 0x00, 0x00,
                       'W', 'o', 'r', 'l', 'd'};
    result = ws_process_frame(&conn, small, sizeof(small), &ctx, &consumed);
    TEST_ASSERT_EQUAL(WS_FRAME_COMPLETE, result);
    TEST_ASSERT_EQUAL(WS_DEFAULT_BUFFER_SIZE, ctx.payload_buffer_size);
    // Small frame delivered zero-copy from the new input slice
    TEST_ASSERT_EQUAL_PTR(&small[6], ctx.payload_ptr);
    TEST_ASSERT_EQUAL_MEMORY("World", ctx.payload_ptr, 5);

    if (ctx.payload_buffer) free(ctx.payload_buffer);
}

// ==================== Frame send: scatter-gather vs fallback ====================

// Capture mocks for ws_send_frame's send-function contract. The single-buffer
// mock records each call's length; the two-buffer mock records both parts of
// one scatter-gather call. Both append into one contiguous capture so byte
// order across calls/parts can be asserted.
#define SEND_CAP_MAX 1024
static uint8_t s_send_cap[SEND_CAP_MAX];
static size_t s_send_cap_len;
static int s_send1_calls;
static size_t s_send1_first_len;
static int s_send2_calls;
static size_t s_send2_len0;
static size_t s_send2_len1;

static void reset_send_capture(void)
{
    s_send_cap_len = 0;
    s_send1_calls = 0;
    s_send1_first_len = 0;
    s_send2_calls = 0;
    s_send2_len0 = 0;
    s_send2_len1 = 0;
}

static void capture_bytes(const void* data, size_t len)
{
    if (data && len > 0 && s_send_cap_len + len <= SEND_CAP_MAX) {
        memcpy(s_send_cap + s_send_cap_len, data, len);
        s_send_cap_len += len;
    }
}

static ssize_t mock_send1(connection_t* conn, const void* data, size_t len, int flags)
{
    (void)conn; (void)flags;
    if (s_send1_calls == 0) {
        s_send1_first_len = len;
    }
    s_send1_calls++;
    capture_bytes(data, len);
    return (ssize_t)len;
}

static ssize_t mock_send2(connection_t* conn, const void* buf0, size_t len0,
                          const void* buf1, size_t len1)
{
    (void)conn;
    s_send2_calls++;
    s_send2_len0 = len0;
    s_send2_len1 = len1;
    capture_bytes(buf0, len0);
    capture_bytes(buf1, len1);
    return (ssize_t)(len0 + len1);
}

// >256B frame with a two-buffer send func registered: ONE scatter-gather call
// carrying [frame_header, payload], arriving contiguous and in order
static void test_send_frame_large_uses_two_buffer_func(void)
{
    connection_t conn = {0};
    reset_send_capture();
    ws_set_send_func(mock_send1);
    ws_set_send2_func(mock_send2);

    static uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xFF);
    }

    int ret = ws_send_frame(&conn, WS_OPCODE_BINARY, payload, sizeof(payload), false);

    ws_set_send_func(NULL);
    ws_set_send2_func(NULL);

    // 4-byte header (16-bit extended length) + 300-byte payload
    TEST_ASSERT_EQUAL(304, ret);
    TEST_ASSERT_EQUAL(1, s_send2_calls);           // Exactly one 2-buffer call
    TEST_ASSERT_EQUAL(0, s_send1_calls);           // Single-buffer func unused
    TEST_ASSERT_EQUAL(4, s_send2_len0);            // buf0 = frame header
    TEST_ASSERT_EQUAL(sizeof(payload), s_send2_len1);  // buf1 = payload

    // Contiguous [header|payload]: FIN+BINARY, 126, len hi/lo, then payload
    TEST_ASSERT_EQUAL(304, s_send_cap_len);
    TEST_ASSERT_EQUAL_UINT8(0x82, s_send_cap[0]);
    TEST_ASSERT_EQUAL_UINT8(126, s_send_cap[1]);
    TEST_ASSERT_EQUAL_UINT8((300 >> 8) & 0xFF, s_send_cap[2]);
    TEST_ASSERT_EQUAL_UINT8(300 & 0xFF, s_send_cap[3]);
    TEST_ASSERT_EQUAL_MEMORY(payload, &s_send_cap[4], sizeof(payload));
}

// Same frame with NO two-buffer func (test-mode/mock configuration): the
// two-call fallback must still produce correct, ordered two-part output
static void test_send_frame_large_fallback_two_calls(void)
{
    connection_t conn = {0};
    reset_send_capture();
    ws_set_send_func(mock_send1);
    ws_set_send2_func(NULL);

    static uint8_t payload[300];
    for (size_t i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)((i * 7) & 0xFF);
    }

    int ret = ws_send_frame(&conn, WS_OPCODE_BINARY, payload, sizeof(payload), false);

    ws_set_send_func(NULL);

    TEST_ASSERT_EQUAL(304, ret);
    TEST_ASSERT_EQUAL(0, s_send2_calls);
    TEST_ASSERT_EQUAL(2, s_send1_calls);           // Header call + payload call
    TEST_ASSERT_EQUAL(4, s_send1_first_len);       // First call = 4-byte header

    TEST_ASSERT_EQUAL(304, s_send_cap_len);
    TEST_ASSERT_EQUAL_UINT8(0x82, s_send_cap[0]);
    TEST_ASSERT_EQUAL_UINT8(126, s_send_cap[1]);
    TEST_ASSERT_EQUAL_UINT8((300 >> 8) & 0xFF, s_send_cap[2]);
    TEST_ASSERT_EQUAL_UINT8(300 & 0xFF, s_send_cap[3]);
    TEST_ASSERT_EQUAL_MEMORY(payload, &s_send_cap[4], sizeof(payload));
}

// <=256B combined frames keep the single stack-coalesced send even when the
// two-buffer func is registered (one small buffer beats building an iovec)
static void test_send_frame_small_stays_single_call(void)
{
    connection_t conn = {0};
    reset_send_capture();
    ws_set_send_func(mock_send1);
    ws_set_send2_func(mock_send2);

    static const uint8_t payload[100] = { [0] = 0xAB, [99] = 0xCD };

    int ret = ws_send_frame(&conn, WS_OPCODE_BINARY, payload, sizeof(payload), false);

    ws_set_send_func(NULL);
    ws_set_send2_func(NULL);

    TEST_ASSERT_EQUAL(102, ret);                   // 2-byte header + 100 payload
    TEST_ASSERT_EQUAL(1, s_send1_calls);           // One combined send
    TEST_ASSERT_EQUAL(0, s_send2_calls);           // 2-buffer func not used
    TEST_ASSERT_EQUAL(102, s_send_cap_len);
    TEST_ASSERT_EQUAL_UINT8(0x82, s_send_cap[0]);
    TEST_ASSERT_EQUAL_UINT8(100, s_send_cap[1]);
    TEST_ASSERT_EQUAL_MEMORY(payload, &s_send_cap[2], sizeof(payload));
}

void test_websocket_frame_run(void)
{
    // Core functionality tests
    RUN_TEST(test_parse_unmasked_frame_rejected);
    RUN_TEST(test_parse_masked_data_frame_ok);
    RUN_TEST(test_parse_masked_text_frame);
    RUN_TEST(test_parse_extended_length_16);
    RUN_TEST(test_parse_extended_length_64);
    RUN_TEST(test_parse_extended_length_64_too_large);
    RUN_TEST(test_parse_extended_length_64_upper_bits);
    RUN_TEST(test_parse_extended_length_64_split);
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
    RUN_TEST(test_ws_accept_key_format);
    RUN_TEST(test_compute_accept_key_deterministic);
    RUN_TEST(test_compute_accept_key_short_key);
    RUN_TEST(test_compute_accept_key_small_buffer);
    RUN_TEST(test_compute_accept_key_empty);
    RUN_TEST(test_ws_accept_key_uniqueness);

    // Bug fix regression tests
    RUN_TEST(test_fast_path_partial_payload);
    RUN_TEST(test_rsv_bits_fast_path_rejected);
    RUN_TEST(test_split_ping_payload);
    RUN_TEST(test_compute_accept_key_null);
    RUN_TEST(test_close_frame_invalid_status_codes);
    RUN_TEST(test_close_frame_valid_status_codes);

    // RFC 6455 spec-compliance tests (masking + control-frame length)
    RUN_TEST(test_control_frame_extended_length_rejected);
    RUN_TEST(test_masked_control_frame_ok);

    // payload_ptr zero-copy contract + payload buffer shrink tests
    RUN_TEST(test_payload_ptr_zero_copy_single_slice);
    RUN_TEST(test_payload_ptr_split_frame_accumulates);
    RUN_TEST(test_payload_ptr_zero_length_frame);
    RUN_TEST(test_payload_buffer_shrinks_after_large_frame);

    // Frame send: scatter-gather (2-buffer func) vs two-call fallback
    RUN_TEST(test_send_frame_large_uses_two_buffer_func);
    RUN_TEST(test_send_frame_large_fallback_two_calls);
    RUN_TEST(test_send_frame_small_stays_single_call);

    ESP_LOGI(TAG, "WebSocket frame tests completed");
}