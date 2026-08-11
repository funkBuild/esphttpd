#include "private/send_buffer.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_log.h"

static const char TAG[] = "SEND_BUF";

// Optional hook: invoked whenever the buffer closes a streaming file fd, so the
// owner (the filesystem module) can release its in-flight open-file count. The
// server registers it via send_buffer_set_file_close_cb(); it stays NULL (and
// thus a no-op) in standalone/test builds. Kept out of send_buffer.h so the
// shared private header does not have to change.
static void (*s_file_close_cb)(void) = NULL;

void send_buffer_set_file_close_cb(void (*cb)(void)) {
    s_file_close_cb = cb;
}

// Close the streaming file fd (if one is open) exactly once and notify the
// owner so its open-file accounting stays balanced. Every fd that reaches this
// buffer came through send_buffer_start_file after the owner incremented its
// count, so one close == one decrement.
static void sb_close_stream_fd(send_buffer_t* sb) {
    if (sb->file_fd >= 0) {
        close(sb->file_fd);
        sb->file_fd = -1;
        if (s_file_close_cb) {
            s_file_close_cb();
        }
    }
}

void send_buffer_init(send_buffer_t* sb) {
    memset(sb, 0, sizeof(send_buffer_t));
    sb->file_fd = -1;
}

bool send_buffer_alloc(send_buffer_t* sb) {
    if (sb->allocated && sb->buffer) {
        return true;  // Already allocated
    }

    // Dynamically allocate buffer
    sb->buffer = (uint8_t*)malloc(SEND_BUFFER_SIZE);
    if (!sb->buffer) {
        ESP_LOGW(TAG, "Failed to allocate send buffer (%d bytes)", SEND_BUFFER_SIZE);
        return false;
    }

    sb->size = SEND_BUFFER_SIZE;
    sb->head = 0;
    sb->tail = 0;
    sb->allocated = 1;

    ESP_LOGD(TAG, "Allocated send buffer (%d bytes)", SEND_BUFFER_SIZE);
    return true;
}

void send_buffer_free(send_buffer_t* sb) {
    // Close any open streaming file (releases the owner's open-file count)
    sb_close_stream_fd(sb);

    // Free owned memory buffer
    if (sb->mem_owned) {
        free(sb->mem_owned);
    }

    // Free dynamically allocated buffer
    if (sb->buffer) {
        free(sb->buffer);
        ESP_LOGD(TAG, "Freed send buffer");
    }

    send_buffer_init(sb);
}

void send_buffer_reset(send_buffer_t* sb) {
    sb->head = 0;
    sb->tail = 0;

    sb_close_stream_fd(sb);
    sb->file_remaining = 0;
    sb->streaming = 0;
    sb->chunked = 0;
    sb->headers_done = 0;

    // Clean up memory streaming
    if (sb->mem_owned) {
        free(sb->mem_owned);
        sb->mem_owned = NULL;
    }
    sb->mem_ptr = NULL;
    sb->mem_remaining = 0;
    sb->mem_streaming = 0;
    sb->overflow_warned = 0;  // New response gets a fresh backlog-cap warning
}

ssize_t send_buffer_queue(send_buffer_t* __restrict sb, const void* __restrict data, size_t len) {
    if (!sb->buffer || !data || len == 0) {
        return -1;
    }

    size_t available = send_buffer_space(sb);
    if (len > available) {
        ESP_LOGD(TAG, "Buffer full: need %zu, have %zu", len, available);
        return -1;  // Not enough space
    }

    const uint8_t* src = (const uint8_t*)data;

    // Optimized: at most 2 memcpy calls, no loop or modulo per iteration
    size_t to_end = sb->size - sb->head;
    if (len <= to_end) {
        // Fast path: data fits without wrapping
        memcpy(sb->buffer + sb->head, src, len);
        sb->head += len;
        if (sb->head == sb->size) {
            sb->head = 0;  // Wrap exactly at boundary
        }
    } else {
        // Data wraps: copy to end, then from start
        memcpy(sb->buffer + sb->head, src, to_end);
        memcpy(sb->buffer, src + to_end, len - to_end);
        sb->head = len - to_end;
    }

    return (ssize_t)len;
}

size_t send_buffer_peek(send_buffer_t* sb, const uint8_t** data) {
    if (!sb->buffer || sb->head == sb->tail) {
        *data = NULL;
        return 0;
    }

    *data = sb->buffer + sb->tail;

    // Return contiguous length (may wrap)
    if (sb->head > sb->tail) {
        return sb->head - sb->tail;
    } else {
        return sb->size - sb->tail;  // To end of buffer
    }
}

void send_buffer_consume(send_buffer_t* sb, size_t len) {
    if (!sb || !sb->buffer || len == 0) return;

    size_t pending = send_buffer_pending(sb);
    if (len > pending) {
        len = pending;  // Don't consume more than available
    }

    sb->tail = (sb->tail + len) & (sb->size - 1);

    // Reset head/tail when buffer becomes empty to maximize contiguous space
    // This is critical for chunked encoding which needs ~10 bytes overhead per chunk
    if (sb->head == sb->tail) {
        sb->head = 0;
        sb->tail = 0;
    }
}

bool send_buffer_start_file(send_buffer_t* sb, int file_fd, size_t file_size) {
    if (file_fd < 0) {
        return false;
    }

    // Clamp file_size to uint32_t capacity
    if (file_size > UINT32_MAX) {
        ESP_LOGW(TAG, "File size %zu exceeds uint32_t, clamping", file_size);
        file_size = UINT32_MAX;
    }

    // Close any existing stream first (releases the replaced stream's count)
    sb_close_stream_fd(sb);

    sb->file_fd = file_fd;
    sb->file_remaining = (uint32_t)file_size;
    sb->streaming = 1;

    ESP_LOGD(TAG, "Started file stream: fd=%d, size=%zu", file_fd, file_size);
    return true;
}

void send_buffer_stop_file(send_buffer_t* sb) {
    sb_close_stream_fd(sb);
    sb->file_remaining = 0;
    sb->streaming = 0;
}

bool send_buffer_start_mem(send_buffer_t* sb, const uint8_t* data, size_t len) {
    return send_buffer_start_mem2(sb, data, len, NULL, 0);
}

// Two-part variant: appends [d0][d1] atomically. ONE cap check up front for
// the combined length, ONE allocation/grow, then both copies — so a two-part
// caller (scatter-gather send residue: header block + body) can never end up
// with d0 queued and d1 rejected, which would put a torn response on the wire.
bool send_buffer_start_mem2(send_buffer_t* sb, const uint8_t* d0, size_t l0,
                            const uint8_t* d1, size_t l1) {
    // Normalize empty parts; shift a lone second part into the first slot
    if (!d0 || l0 == 0) {
        d0 = d1;
        l0 = l1;
        d1 = NULL;
        l1 = 0;
    }
    if (!d1 || l1 == 0) {
        d1 = NULL;
        l1 = 0;
    }
    if (!d0 || l0 == 0) {
        return false;  // Both parts empty/NULL (matches start_mem's contract)
    }
    size_t total = l0 + l1;

    // If a stream is already pending, append behind its unsent bytes —
    // replacing the buffer here would silently drop response data.
    if (sb->mem_owned) {
        size_t unsent = sb->mem_remaining;
        // Cap the per-connection send backlog. A peer that stops reading
        // (e.g. a WebSocket subscriber continuously fed by httpd_ws_publish)
        // would otherwise grow this overflow stream without bound until OOM.
        // Ring-pending bytes count against the cap too: they are the same
        // unacknowledged backlog, just staged one hop closer to the socket.
        // Checked ONCE for the combined length BEFORE anything is copied.
        size_t backlog = send_buffer_pending(sb) + unsent + total;
        if (backlog > SEND_BUFFER_MAX_PENDING) {
            if (!sb->overflow_warned) {
                sb->overflow_warned = 1;  // Warn once per response, not per call
                ESP_LOGW(TAG, "Send backlog cap exceeded (%zu pending + %zu new > %d), dropping",
                         send_buffer_pending(sb) + unsent, total, SEND_BUFFER_MAX_PENDING);
            }
            return false;
        }
        // Compact unsent bytes to the start of the allocation, then grow.
        memmove(sb->mem_owned, sb->mem_ptr, unsent);
        sb->mem_ptr = sb->mem_owned;
        uint8_t* grown = (uint8_t*)realloc(sb->mem_owned, unsent + total);
        if (!grown) {
            ESP_LOGW(TAG, "Failed to grow memory stream buffer (%zu bytes)", unsent + total);
            return false;
        }
        memcpy(grown + unsent, d0, l0);
        if (l1 > 0) {
            memcpy(grown + unsent + l0, d1, l1);
        }
        sb->mem_owned = grown;
        sb->mem_ptr = grown;
        sb->mem_remaining = (uint32_t)(unsent + total);
        sb->mem_streaming = 1;
        ESP_LOGD(TAG, "Appended %zu bytes to memory stream (%" PRIu32 " pending)",
                 total, sb->mem_remaining);
        return true;
    }

    // Allocate a copy so the caller's buffers can go out of scope
    uint8_t* copy = (uint8_t*)malloc(total);
    if (!copy) {
        ESP_LOGW(TAG, "Failed to allocate memory stream buffer (%zu bytes)", total);
        return false;
    }
    memcpy(copy, d0, l0);
    if (l1 > 0) {
        memcpy(copy + l0, d1, l1);
    }

    sb->mem_owned = copy;
    sb->mem_ptr = copy;
    sb->mem_remaining = (uint32_t)total;
    sb->mem_streaming = 1;

    ESP_LOGD(TAG, "Started memory stream: %zu bytes", total);
    return true;
}

void send_buffer_stop_mem(send_buffer_t* sb) {
    if (sb->mem_owned) {
        free(sb->mem_owned);
        sb->mem_owned = NULL;
    }
    sb->mem_ptr = NULL;
    sb->mem_remaining = 0;
    sb->mem_streaming = 0;
}
