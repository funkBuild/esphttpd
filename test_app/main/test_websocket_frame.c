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

void test_websocket_frame_run(void)
{
    RUN_TEST(test_parse_unmasked_text_frame);
    RUN_TEST(test_parse_masked_text_frame);
    RUN_TEST(test_parse_extended_length_16);
    RUN_TEST(test_parse_fragmented_frame);
    RUN_TEST(test_parse_control_frames);
    RUN_TEST(test_parse_frame_in_chunks);
    RUN_TEST(test_parse_invalid_frames);
    RUN_TEST(test_build_frame_header);
    RUN_TEST(test_mask_unmask_payload);

    ESP_LOGI(TAG, "WebSocket frame tests completed");
}