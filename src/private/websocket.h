#ifndef _WEBSOCKET_FRAME_H_
#define _WEBSOCKET_FRAME_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

// WebSocket frame parsing states
typedef enum {
    WS_STATE_OPCODE,
    WS_STATE_LENGTH,
    WS_STATE_LENGTH_EXT_16,
    WS_STATE_LENGTH_EXT_64,
    WS_STATE_MASK,
    WS_STATE_PAYLOAD,
    WS_STATE_COMPLETE
} ws_frame_state_t;

// Maximum WebSocket payload size we'll buffer (8KB should be enough for most use cases)
#define WS_MAX_PAYLOAD_SIZE 8192

// Default pre-allocated buffer size (256 bytes handles most small messages)
#define WS_DEFAULT_BUFFER_SIZE 256

// WebSocket frame info (per-connection state)
typedef struct {
    uint8_t state;               // ws_frame_state_t (only 7 enum values)
    uint8_t header_bytes;        // Bytes of header parsed
    uint8_t mask_bytes_read;     // Mask key bytes read
    uint8_t length_bytes_needed; // Extended length bytes needed
    uint32_t payload_length;     // Accumulated payload length (max 65535)
    uint8_t* payload_buffer;     // Buffer to accumulate payload data
    uint16_t payload_buffer_size; // Allocated size (max WS_MAX_PAYLOAD_SIZE=8192)
    uint16_t payload_received;   // Bytes of payload received so far
} ws_frame_context_t;

// Frame processing result
typedef enum {
    WS_FRAME_OK,           // Frame processing OK, continue
    WS_FRAME_NEED_MORE,    // Need more data
    WS_FRAME_COMPLETE,     // Frame complete
    WS_FRAME_ERROR,        // Protocol error
    WS_FRAME_CLOSE         // Close frame received
} ws_frame_result_t;

// Process WebSocket frame data in-place
ws_frame_result_t ws_process_frame(connection_t* conn,
                                   uint8_t* buffer,
                                   size_t buffer_len,
                                   ws_frame_context_t* ctx,
                                   size_t* bytes_consumed);

// Build a WebSocket frame header
size_t ws_build_frame_header(uint8_t* buffer,
                            ws_opcode_internal_t opcode,
                            size_t payload_len,
                            bool mask);

// Mask/unmask payload data in-place
void ws_mask_payload(uint8_t* payload,
                    size_t len,
                    uint32_t mask_key,
                    size_t offset);

// Send callback type: used to route WebSocket sends through the server's
// non-blocking send infrastructure instead of direct write() syscalls.
// Signature: (connection, data, len) -> bytes sent/queued, or -1 on error
typedef ssize_t (*ws_send_func_t)(connection_t* conn, const void* data, size_t len);

// Set the send function used by WebSocket frame sending.
// Called by the server during init to route sends through send_nonblocking().
// When NULL (default), falls back to blocking write(conn->fd, ...) for tests.
void ws_set_send_func(ws_send_func_t func);

// Send a WebSocket frame (builds header + payload and sends via send callback)
int ws_send_frame(connection_t* conn,
                 ws_opcode_internal_t opcode,
                 const uint8_t* payload,
                 size_t payload_len,
                 bool mask);

// Send a close frame with optional reason
int ws_send_close(connection_t* conn, uint16_t code, const char* reason);

// Send a ping frame
int ws_send_ping(connection_t* conn, const uint8_t* data, size_t len);

// Send a pong frame
int ws_send_pong(connection_t* conn, const uint8_t* data, size_t len);

// Handle control frames (ping, pong, close)
ws_frame_result_t ws_handle_control_frame(connection_t* conn,
                                         ws_opcode_internal_t opcode,
                                         uint8_t* payload,
                                         size_t payload_len);

// WebSocket handshake helpers
int ws_send_handshake_response(connection_t* conn, const char* key);

// Compute WebSocket accept key from client key
// Returns 0 on success, negative mbedtls error code on failure
// out_len receives the base64 output length (may be NULL)
int ws_compute_accept_key(const char* client_key,
                          char* accept_key,
                          size_t accept_key_size,
                          size_t* out_len);

// Pre-allocate payload buffer to avoid realloc fragmentation
// Call when WebSocket connection is established. Reuses existing buffer if present.
bool ws_frame_ctx_init(ws_frame_context_t* ctx);

// Inline utilities for frame header parsing
static inline bool ws_is_control_frame(ws_opcode_internal_t opcode) {
    return (opcode & 0x08) != 0;
}

static inline bool ws_is_final_frame(uint8_t first_byte) {
    return (first_byte & 0x80) != 0;
}

static inline ws_opcode_internal_t ws_get_opcode(uint8_t first_byte) {
    return (ws_opcode_internal_t)(first_byte & 0x0F);
}

static inline bool ws_is_masked(uint8_t second_byte) {
    return (second_byte & 0x80) != 0;
}

static inline uint8_t ws_get_payload_length(uint8_t second_byte) {
    return second_byte & 0x7F;
}

#ifdef __cplusplus
}
#endif

#endif // _WEBSOCKET_FRAME_H_