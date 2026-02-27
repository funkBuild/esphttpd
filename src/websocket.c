#include "private/websocket.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"

static const char TAG[] = "WS_FRAME";
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Send function callback - set by server to route through send_nonblocking().
// When NULL, falls back to blocking write() (used in tests with mock fd=-1).
static ws_send_func_t s_send_func = NULL;

void ws_set_send_func(ws_send_func_t func) {
    s_send_func = func;
}

// Internal helper: send data through the registered send function, or fall
// back to write() when no send function is registered (test mode).
static ssize_t ws_do_send(connection_t* conn, const void* data, size_t len) {
    if (s_send_func) {
        return s_send_func(conn, data, len);
    }
    // Fallback for tests and standalone usage
    return write(conn->fd, data, len);
}

// Initialize frame context with pre-allocated buffer
// Reuses existing buffer if already allocated and large enough
bool ws_frame_ctx_init(ws_frame_context_t* ctx) {
    if (!ctx) return false;

    // Reset parsing state
    ctx->state = WS_STATE_OPCODE;
    ctx->header_bytes = 0;
    ctx->mask_bytes_read = 0;
    ctx->length_bytes_needed = 0;
    ctx->payload_length = 0;
    ctx->payload_received = 0;

    // Pre-allocate buffer if not already present or too small
    if (ctx->payload_buffer_size < WS_DEFAULT_BUFFER_SIZE) {
        uint8_t* new_buffer = realloc(ctx->payload_buffer, WS_DEFAULT_BUFFER_SIZE);
        if (!new_buffer) {
            ESP_LOGE(TAG, "Failed to pre-allocate WS buffer");
            return false;
        }
        ctx->payload_buffer = new_buffer;
        ctx->payload_buffer_size = WS_DEFAULT_BUFFER_SIZE;
    }

    return true;
}

// Helper to allocate/reallocate payload buffer
static bool ensure_payload_buffer(ws_frame_context_t* ctx, size_t required_size) {
    if (required_size > WS_MAX_PAYLOAD_SIZE) {
        ESP_LOGE(TAG, "Payload too large: %zu > %d", required_size, WS_MAX_PAYLOAD_SIZE);
        return false;
    }

    if (ctx->payload_buffer_size >= required_size) {
        return true;  // Already big enough
    }

    uint8_t* new_buffer = realloc(ctx->payload_buffer, required_size);
    if (!new_buffer) {
        ESP_LOGE(TAG, "Failed to allocate payload buffer of size %zu", required_size);
        return false;
    }

    ctx->payload_buffer = new_buffer;
    ctx->payload_buffer_size = required_size;
    return true;
}

ws_frame_result_t ws_process_frame(connection_t* conn,
                                   uint8_t* buffer,
                                   size_t buffer_len,
                                   ws_frame_context_t* ctx,
                                   size_t* bytes_consumed) {
    // Guard against NULL parameters
    if (!conn || !ctx || !bytes_consumed) {
        return WS_FRAME_ERROR;
    }
    if (!buffer && buffer_len > 0) {
        return WS_FRAME_ERROR;
    }

    size_t i = 0;
    *bytes_consumed = 0;

    while (i < buffer_len) {
        switch (ctx->state) {
            case WS_STATE_OPCODE: {
                uint8_t first_byte = buffer[i];
                conn->ws_fin = ws_is_final_frame(first_byte);
                conn->ws_opcode = ws_get_opcode(first_byte);

                // Validate opcode (invalid opcodes are rare - use branch hint)
                if (__builtin_expect(conn->ws_opcode > WS_OPCODE_PONG ||
                    (conn->ws_opcode >= 0x3 && conn->ws_opcode <= 0x7), 0)) {
                    ESP_LOGE(TAG, "Invalid opcode: 0x%x", conn->ws_opcode);
                    return WS_FRAME_ERROR;
                }

                // Control frames must not be fragmented (rare error case)
                if (__builtin_expect(ws_is_control_frame(conn->ws_opcode) && !conn->ws_fin, 0)) {
                    ESP_LOGE(TAG, "Fragmented control frame");
                    return WS_FRAME_ERROR;
                }

                ctx->state = WS_STATE_LENGTH;
                i++;
                break;
            }

            case WS_STATE_LENGTH: {
                uint8_t second_byte = buffer[i];
                conn->ws_masked = ws_is_masked(second_byte);
                uint8_t payload_len = ws_get_payload_length(second_byte);

                // Most common case: payload_len < 126 (~95% of messages)
                if (__builtin_expect(payload_len < 126, 1)) {
                    conn->ws_payload_len = payload_len;
                    // Handle zero-length unmasked payload immediately (common for close/ping/pong)
                    if (payload_len == 0 && !conn->ws_masked) {
                        // For control frames with no payload, handle them now
                        if (ws_is_control_frame(conn->ws_opcode)) {
                            ws_frame_result_t result = ws_handle_control_frame(conn,
                                conn->ws_opcode, NULL, 0);
                            if (result == WS_FRAME_CLOSE) {
                                *bytes_consumed = i + 1;
                                return WS_FRAME_CLOSE;
                            }
                        }
                        conn->ws_payload_read = 0;
                        ctx->state = WS_STATE_COMPLETE;
                        *bytes_consumed = i + 1;
                        return WS_FRAME_COMPLETE;
                    }
                    ctx->state = conn->ws_masked ? WS_STATE_MASK : WS_STATE_PAYLOAD;
                } else if (payload_len == 126) {
                    ctx->state = WS_STATE_LENGTH_EXT_16;
                    ctx->length_bytes_needed = 2;
                    conn->ws_payload_len = 0;
                } else { // payload_len == 127
                    ctx->state = WS_STATE_LENGTH_EXT_64;
                    ctx->length_bytes_needed = 8;
                    ctx->payload_length = 0;
                }
                i++;
                break;
            }

            case WS_STATE_LENGTH_EXT_16: {
                conn->ws_payload_len = (conn->ws_payload_len << 8) | buffer[i];
                ctx->length_bytes_needed--;
                i++;

                if (ctx->length_bytes_needed == 0) {
                    // Control frames must have payload <= 125
                    if (ws_is_control_frame(conn->ws_opcode) && conn->ws_payload_len > 125) {
                        ESP_LOGE(TAG, "Control frame payload too large");
                        return WS_FRAME_ERROR;
                    }
                    ctx->state = conn->ws_masked ? WS_STATE_MASK : WS_STATE_PAYLOAD;
                }
                break;
            }

            case WS_STATE_LENGTH_EXT_64: {
                // Reject early if any of the upper 4 bytes are non-zero
                // (bytes_needed 8..5 are the upper 32 bits of the 64-bit length)
                if (ctx->length_bytes_needed > 4 && buffer[i] != 0) {
                    ESP_LOGE(TAG, "Payload too large (upper bits set)");
                    return WS_FRAME_ERROR;
                }
                // Only accumulate the lower 4 bytes into payload_length
                if (ctx->length_bytes_needed <= 4) {
                    ctx->payload_length = (ctx->payload_length << 8) | buffer[i];
                }
                ctx->length_bytes_needed--;
                i++;

                if (ctx->length_bytes_needed == 0) {
                    // We only support up to 64KB payloads
                    if (ctx->payload_length > 65535) {
                        ESP_LOGE(TAG, "Payload too large: %" PRIu32, ctx->payload_length);
                        return WS_FRAME_ERROR;
                    }
                    conn->ws_payload_len = (uint16_t)ctx->payload_length;
                    ctx->state = conn->ws_masked ? WS_STATE_MASK : WS_STATE_PAYLOAD;
                }
                break;
            }

            case WS_STATE_MASK: {
                ((uint8_t*)&conn->ws_mask_key)[ctx->mask_bytes_read] = buffer[i];
                ctx->mask_bytes_read++;
                i++;

                if (ctx->mask_bytes_read == 4) {
                    ctx->mask_bytes_read = 0;
                    conn->ws_payload_read = 0;
                    ctx->state = WS_STATE_PAYLOAD;
                }
                break;
            }

            case WS_STATE_PAYLOAD: {
                // Cache control frame check result to avoid duplicate function calls
                bool is_control = ws_is_control_frame(conn->ws_opcode);

                // Allocate payload buffer on first entry (if not a control frame)
                if (ctx->payload_received == 0 && conn->ws_payload_len > 0 && !is_control) {
                    if (!ensure_payload_buffer(ctx, conn->ws_payload_len)) {
                        return WS_FRAME_ERROR;
                    }
                }

                size_t payload_remaining = conn->ws_payload_len - ctx->payload_received;
                size_t buffer_remaining = buffer_len - i;
                size_t to_process = payload_remaining < buffer_remaining ?
                                   payload_remaining : buffer_remaining;

                // Unmask in place if needed (ws_mask_payload handles zero-length gracefully)
                if (conn->ws_masked) {
                    ws_mask_payload(&buffer[i], to_process,
                                  conn->ws_mask_key, ctx->payload_received);
                }

                // Handle control frames immediately (they use the buffer directly)
                if (is_control) {
                    ws_frame_result_t result = ws_handle_control_frame(conn,
                                                                      conn->ws_opcode,
                                                                      &buffer[i],
                                                                      to_process);
                    if (result == WS_FRAME_CLOSE) {
                        *bytes_consumed = i + to_process;
                        return WS_FRAME_CLOSE;
                    }
                } else {
                    // Copy unmasked data to payload buffer for data frames
                    memcpy(ctx->payload_buffer + ctx->payload_received, &buffer[i], to_process);
                }

                ctx->payload_received += to_process;
                i += to_process;

                if (ctx->payload_received >= conn->ws_payload_len) {
                    // Frame complete - sync ws_payload_read only when needed
                    conn->ws_payload_read = ctx->payload_received;
                    ctx->state = WS_STATE_COMPLETE;
                    *bytes_consumed = i;
                    return WS_FRAME_COMPLETE;
                }
                break;
            }

            case WS_STATE_COMPLETE:
                // Reset for next frame
                ctx->state = WS_STATE_OPCODE;
                ctx->header_bytes = 0;
                ctx->mask_bytes_read = 0;
                conn->ws_payload_read = 0;
                conn->ws_payload_len = 0;
                return WS_FRAME_COMPLETE;
        }
    }

    *bytes_consumed = i;
    return WS_FRAME_NEED_MORE;
}

void ws_mask_payload(uint8_t* __restrict payload, size_t len, uint32_t mask_key, size_t offset) {
    // Guard against NULL or zero length
    if (!payload || len == 0) return;

    const uint8_t* mask_bytes = (const uint8_t*)&mask_key;
    size_t i = 0;

    // Handle bytes until both payload pointer AND mask rotation are aligned
    // We need (offset + i) % 4 == 0 AND payload + i aligned for the fast path
    // Process byte-by-byte until BOTH are aligned (worst case: 7 bytes)
    while (i < len && (((uintptr_t)(payload + i) & 3) != 0 || ((offset + i) & 3) != 0)) {
        payload[i] ^= mask_bytes[(offset + i) & 3];
        i++;
    }

    // Fast path: Process 8 bytes at a time (unrolled for better ILP)
    // Now both pointer and mask rotation are aligned
    for (; i + 8 <= len; i += 8) {
        *(uint32_t*)(payload + i) ^= mask_key;
        *(uint32_t*)(payload + i + 4) ^= mask_key;
    }

    // Handle remaining 4-byte chunk
    if (i + 4 <= len) {
        *(uint32_t*)(payload + i) ^= mask_key;
        i += 4;
    }

    // Handle final 0-3 bytes with correct mask offset
    for (; i < len; i++) {
        payload[i] ^= mask_bytes[(offset + i) & 3];
    }
}

size_t ws_build_frame_header(uint8_t* buffer, ws_opcode_internal_t opcode,
                            size_t payload_len, bool mask) {
    // Guard against NULL buffer
    if (!buffer) return 0;

    size_t header_len = 2;

    // First byte: FIN=1, RSV=0, Opcode
    buffer[0] = 0x80 | (opcode & 0x0F);

    // Second byte: Mask bit and payload length
    if (payload_len < 126) {
        buffer[1] = (mask ? 0x80 : 0x00) | (uint8_t)payload_len;
    } else if (payload_len <= 65535) {
        buffer[1] = (mask ? 0x80 : 0x00) | 126;
        buffer[2] = (payload_len >> 8) & 0xFF;
        buffer[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        // We don't support > 64KB frames
        return 0;
    }

    if (mask) {
        // Add mask key (would be random in production)
        memset(buffer + header_len, 0, 4);
        header_len += 4;
    }

    return header_len;
}

int ws_send_frame(connection_t* conn, ws_opcode_internal_t opcode, const uint8_t* payload,
                 size_t payload_len, bool mask) {
    if (!conn) return -1;

    // Normalize: NULL payload means zero-length payload
    if (!payload) payload_len = 0;

    uint8_t header[14]; // Max header size
    size_t header_len = ws_build_frame_header(header, opcode, payload_len, mask);

    if (header_len == 0) {
        return -1;
    }

    size_t total_len = header_len + payload_len;

    // Build complete frame into a single buffer to ensure atomic send.
    // For small frames (<=256 bytes), use the stack. For larger frames,
    // send header and payload as two separate calls - the send_buffer
    // infrastructure ensures ordering.
    if (total_len <= 256) {
        uint8_t frame_buf[256];
        memcpy(frame_buf, header, header_len);
        if (payload_len > 0) {
            memcpy(frame_buf + header_len, payload, payload_len);
        }
        if (ws_do_send(conn, frame_buf, total_len) < 0) {
            return -1;
        }
    } else {
        // Larger frame: send header then payload (send_buffer preserves order)
        if (ws_do_send(conn, header, header_len) < 0) {
            return -1;
        }
        if (payload_len > 0) {
            if (ws_do_send(conn, payload, payload_len) < 0) {
                return -1;
            }
        }
    }

    return (int)total_len;
}

int ws_send_close(connection_t* conn, uint16_t code, const char* reason) {
    uint8_t payload[125];
    size_t payload_len = 0;

    if (code != 0) {
        // Add status code (network byte order)
        uint16_t code_be = htons(code);
        memcpy(payload, &code_be, 2);
        payload_len = 2;

        // Add reason if provided
        if (reason != NULL) {
            size_t reason_len = strlen(reason);
            if (reason_len > 123) {
                reason_len = 123;
            }
            memcpy(payload + 2, reason, reason_len);
            payload_len += reason_len;
        }
    }

    // Pass NULL when no payload to avoid uninitialized access warning
    return ws_send_frame(conn, WS_OPCODE_CLOSE, payload_len > 0 ? payload : NULL, payload_len, false);
}

int ws_send_ping(connection_t* conn, const uint8_t* data, size_t len) {
    if (len > 125) len = 125; // Control frames limited to 125 bytes
    return ws_send_frame(conn, WS_OPCODE_PING, data, len, false);
}

int ws_send_pong(connection_t* conn, const uint8_t* data, size_t len) {
    if (len > 125) len = 125; // Control frames limited to 125 bytes
    return ws_send_frame(conn, WS_OPCODE_PONG, data, len, false);
}

ws_frame_result_t ws_handle_control_frame(connection_t* conn,
                                         ws_opcode_internal_t opcode,
                                         uint8_t* payload,
                                         size_t payload_len) {
    // Guard against NULL connection
    if (!conn) return WS_FRAME_ERROR;

    switch (opcode) {
        case WS_OPCODE_CLOSE:
            // Only echo close frame if this is a client-initiated close.
            // If we're in WS_CLOSING state, the server already sent a close
            // frame and this is the client's acknowledgment (RFC 6455 5.5.1).
            if (conn->state != CONN_STATE_WS_CLOSING) {
                ws_send_close(conn, 0, NULL);
            }
            return WS_FRAME_CLOSE;

        case WS_OPCODE_PING:
            // Respond with pong
            ws_send_pong(conn, payload, payload_len);
            return WS_FRAME_OK;

        case WS_OPCODE_PONG:
            // Pong received, update activity time
            return WS_FRAME_OK;

        default:
            return WS_FRAME_OK;
    }
}

int ws_compute_accept_key(const char* client_key, char* accept_key, size_t accept_key_size, size_t* out_len) {
    // Streaming SHA1: feed client key and GUID separately to avoid concat buffer
    size_t key_len = strlen(client_key);

    mbedtls_sha1_context sha_ctx;
    mbedtls_sha1_init(&sha_ctx);
    mbedtls_sha1_starts(&sha_ctx);
    mbedtls_sha1_update(&sha_ctx, (const unsigned char*)client_key, key_len);
    mbedtls_sha1_update(&sha_ctx, (const unsigned char*)WS_GUID, sizeof(WS_GUID) - 1);
    unsigned char hash[20];
    mbedtls_sha1_finish(&sha_ctx, hash);
    mbedtls_sha1_free(&sha_ctx);

    // Base64 encode the hash
    size_t olen;
    int ret = mbedtls_base64_encode((unsigned char*)accept_key, accept_key_size,
                                     &olen, hash, 20);
    if (ret == 0 && out_len) {
        *out_len = olen;
    }
    return ret;
}

int ws_send_handshake_response(connection_t* conn, const char* key) {
    if (!conn) return -1;

    char accept_key[32];
    size_t accept_len;
    int ret = ws_compute_accept_key(key, accept_key, sizeof(accept_key), &accept_len);
    if (ret != 0) {
        return -1;
    }

    // Build response using memcpy of static prefix/suffix around the accept key
    static const char ws_prefix[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    static const char ws_suffix[] = "\r\n\r\n";

    char response[192];
    size_t pos = 0;

    memcpy(response, ws_prefix, sizeof(ws_prefix) - 1);
    pos += sizeof(ws_prefix) - 1;
    memcpy(response + pos, accept_key, accept_len);
    pos += accept_len;
    memcpy(response + pos, ws_suffix, sizeof(ws_suffix) - 1);
    pos += sizeof(ws_suffix) - 1;

    return (int)ws_do_send(conn, response, pos);
}