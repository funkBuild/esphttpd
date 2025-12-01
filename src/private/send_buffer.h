#ifndef _SEND_BUFFER_H_
#define _SEND_BUFFER_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configuration - can be overridden via Kconfig
#ifndef SEND_BUFFER_SIZE
#define SEND_BUFFER_SIZE 8192  // 8KB for better throughput with large transfers
#endif

#ifndef SEND_BUFFER_POOL_SIZE
#define SEND_BUFFER_POOL_SIZE 8  // Max concurrent send buffers
#endif

// Send buffer state
typedef struct {
    // Ring buffer for queued send data
    uint8_t* buffer;            // Buffer memory (NULL if not allocated)
    uint16_t size;              // Buffer capacity
    uint16_t head;              // Write position
    uint16_t tail;              // Read position

    // File streaming state
    int file_fd;                // Open file descriptor (-1 if not streaming)
    uint32_t file_remaining;    // Bytes left to send from file

    // State flags
    uint8_t allocated : 1;      // Buffer is allocated
    uint8_t streaming : 1;      // File streaming active
    uint8_t chunked : 1;        // Using chunked transfer encoding
    uint8_t headers_done : 1;   // HTTP headers fully sent
    uint8_t _reserved : 4;
} send_buffer_t;

// Buffer pool for memory management
typedef struct {
    uint8_t buffers[SEND_BUFFER_POOL_SIZE][SEND_BUFFER_SIZE];
    uint8_t in_use_mask;        // Bitmask of allocated buffers
} send_buffer_pool_t;

// Initialize buffer pool
void send_buffer_pool_init(send_buffer_pool_t* pool);

// Initialize a send buffer (does not allocate memory yet)
void send_buffer_init(send_buffer_t* sb);

// Allocate buffer memory from pool (lazy allocation)
bool send_buffer_alloc(send_buffer_t* sb, send_buffer_pool_t* pool);

// Free buffer back to pool
void send_buffer_free(send_buffer_t* sb, send_buffer_pool_t* pool);

// Reset buffer state (keep allocation)
void send_buffer_reset(send_buffer_t* sb);

// Queue data to send buffer
// Returns bytes queued, or -1 if buffer full
ssize_t send_buffer_queue(send_buffer_t* sb, const void* data, size_t len);

// Get pointer to contiguous data ready to send
// Returns length of contiguous segment (may be less than total pending)
size_t send_buffer_peek(send_buffer_t* sb, const uint8_t** data);

// Consume sent data (advance tail)
void send_buffer_consume(send_buffer_t* sb, size_t len);

// Check if buffer has pending data
static inline bool send_buffer_has_data(send_buffer_t* sb) {
    return sb->head != sb->tail;
}

// Get pending data length
static inline size_t send_buffer_pending(send_buffer_t* sb) {
    if (sb->head >= sb->tail) {
        return sb->head - sb->tail;
    }
    return sb->size - sb->tail + sb->head;
}

// Get available space
static inline size_t send_buffer_space(send_buffer_t* sb) {
    if (!sb->buffer) return 0;
    return sb->size - send_buffer_pending(sb) - 1;  // -1 to distinguish full from empty
}

// Get pointer to contiguous write space (for zero-copy writes)
// Returns pointer and length of contiguous space available (may be less than total space)
static inline size_t send_buffer_write_ptr(send_buffer_t* sb, uint8_t** ptr) {
    if (!sb->buffer) {
        *ptr = NULL;
        return 0;
    }
    *ptr = sb->buffer + sb->head;
    // Contiguous space to end of buffer or to tail (whichever is less)
    size_t to_end = sb->size - sb->head;
    size_t total_space = send_buffer_space(sb);
    return (to_end < total_space) ? to_end : total_space;
}

// Commit written data (advance head after zero-copy write)
static inline void send_buffer_commit(send_buffer_t* sb, size_t len) {
    sb->head += len;
    if (sb->head >= sb->size) {
        sb->head = 0;  // Wrap at boundary
    }
}

// File streaming
bool send_buffer_start_file(send_buffer_t* sb, int file_fd, size_t file_size);
void send_buffer_stop_file(send_buffer_t* sb);

static inline bool send_buffer_is_streaming(send_buffer_t* sb) {
    return sb->streaming && sb->file_fd >= 0;
}

static inline size_t send_buffer_file_remaining(send_buffer_t* sb) {
    return sb->file_remaining;
}

#ifdef __cplusplus
}
#endif

#endif // _SEND_BUFFER_H_
