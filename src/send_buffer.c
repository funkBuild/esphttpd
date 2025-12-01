#include "private/send_buffer.h"
#include <string.h>
#include <unistd.h>
#include "esp_log.h"

static const char* TAG = "SEND_BUF";

void send_buffer_pool_init(send_buffer_pool_t* pool) {
    pool->in_use_mask = 0;
}

void send_buffer_init(send_buffer_t* sb) {
    memset(sb, 0, sizeof(send_buffer_t));
    sb->file_fd = -1;
}

bool send_buffer_alloc(send_buffer_t* sb, send_buffer_pool_t* pool) {
    if (sb->allocated) {
        return true;  // Already allocated
    }

    // Find free buffer in pool
    uint8_t free_mask = ~pool->in_use_mask;
    if (free_mask == 0) {
        ESP_LOGW(TAG, "Send buffer pool exhausted");
        return false;
    }

    int slot = __builtin_ctz(free_mask);
    if (slot >= SEND_BUFFER_POOL_SIZE) {
        return false;
    }

    pool->in_use_mask |= (1U << slot);
    sb->buffer = pool->buffers[slot];
    sb->size = SEND_BUFFER_SIZE;
    sb->head = 0;
    sb->tail = 0;
    sb->allocated = 1;

    ESP_LOGD(TAG, "Allocated send buffer slot %d", slot);
    return true;
}

void send_buffer_free(send_buffer_t* sb, send_buffer_pool_t* pool) {
    if (!sb->allocated || !sb->buffer) {
        return;
    }

    // Find which slot this buffer belongs to
    for (int i = 0; i < SEND_BUFFER_POOL_SIZE; i++) {
        if (sb->buffer == pool->buffers[i]) {
            pool->in_use_mask &= ~(1U << i);
            ESP_LOGD(TAG, "Freed send buffer slot %d", i);
            break;
        }
    }

    // Close any open file
    if (sb->file_fd >= 0) {
        close(sb->file_fd);
    }

    send_buffer_init(sb);
}

void send_buffer_reset(send_buffer_t* sb) {
    sb->head = 0;
    sb->tail = 0;

    if (sb->file_fd >= 0) {
        close(sb->file_fd);
        sb->file_fd = -1;
    }
    sb->file_remaining = 0;
    sb->streaming = 0;
    sb->chunked = 0;
    sb->headers_done = 0;
}

ssize_t send_buffer_queue(send_buffer_t* sb, const void* data, size_t len) {
    if (!sb->buffer || len == 0) {
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
    if (len == 0) return;

    size_t pending = send_buffer_pending(sb);
    if (len > pending) {
        len = pending;  // Don't consume more than available
    }

    sb->tail = (sb->tail + len) % sb->size;

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

    // Close any existing file
    if (sb->file_fd >= 0) {
        close(sb->file_fd);
    }

    sb->file_fd = file_fd;
    sb->file_remaining = file_size;
    sb->streaming = 1;

    ESP_LOGD(TAG, "Started file stream: fd=%d, size=%zu", file_fd, file_size);
    return true;
}

void send_buffer_stop_file(send_buffer_t* sb) {
    if (sb->file_fd >= 0) {
        close(sb->file_fd);
        sb->file_fd = -1;
    }
    sb->file_remaining = 0;
    sb->streaming = 0;
}
