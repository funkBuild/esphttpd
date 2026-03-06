#ifndef _FILESYSTEM_H_
#define _FILESYSTEM_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connection.h"

#include "send_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Send callback type: routes filesystem sends through the server's non-blocking
// send infrastructure. Signature: (connection, data, len) -> bytes sent/queued, or -1 on error
typedef ssize_t (*fs_send_func_t)(connection_t* conn, const void* data, size_t len);

// File stream callback type: initiates non-blocking file streaming via send buffer.
// Opens the file descriptor for streaming and marks the connection as write-pending.
// Signature: (connection, file_fd, file_size) -> 0 on success, -1 on error
// Note: ownership of file_fd transfers to the callback (it will be closed by send_buffer)
typedef int (*fs_start_file_stream_func_t)(connection_t* conn, int file_fd, size_t file_size);

// Set the send function used by filesystem operations.
// Called by the server during init to route sends through send_nonblocking().
// When NULL (default), falls back to blocking send() for backward compatibility.
void fs_set_send_func(fs_send_func_t func);

// Set the file stream function used by filesystem operations.
// Called by the server during init to route file streaming through send_buffer.
// When NULL (default), falls back to blocking read/send loop.
void fs_set_file_stream_func(fs_start_file_stream_func_t func);

// File system configuration for LittleFS
typedef struct {
    const char* base_path;        // Mount point (e.g., "/www")
    size_t max_open_files;        // Maximum open files
    bool format_on_fail;          // Format if mount fails
    size_t partition_size;        // Partition size (0 = use full partition)
    const char* partition_label;  // Partition label (NULL = use default)
} filesystem_config_t;

// MIME type mapping
typedef struct {
    const char* extension;
    uint8_t ext_len;             // Precomputed strlen(extension) for fast comparison
    const char* mime_type;
    bool compress;               // Whether to use gzip if available
    bool cache;                  // Whether to set cache headers
} mime_type_t;

// File metadata
typedef struct {
    uint32_t size;
    uint32_t mtime;              // Modification time
    bool is_directory;
    bool is_gzipped;
    bool cacheable;              // Whether to set cache headers (avoids duplicate MIME lookup)
    const char* mime_type;
    char full_path[128];         // Pre-built full path (avoids rebuild in send_file)
} file_metadata_t;

// File system context
typedef struct {
    bool mounted;
    char base_path[32];
    uint8_t base_path_len;           // Cached strlen(base_path) for fast path building
    uint8_t open_files;
    uint8_t max_open_files;          // Maximum concurrent open files (0 = unlimited)
} filesystem_t;

// Initialize LittleFS with default configuration
int filesystem_init_default(filesystem_t* fs);

// Initialize LittleFS with custom configuration
int filesystem_init(filesystem_t* fs, const filesystem_config_t* config);

// Unmount filesystem
void filesystem_unmount(filesystem_t* fs);

// Serve a file to connection
int filesystem_serve_file(filesystem_t* fs,
                         connection_t* conn,
                         const char* path,
                         bool use_template);

// Check if file exists (also checks for .gz version)
bool filesystem_file_exists(filesystem_t* fs, const char* path);

// Get file metadata
bool filesystem_get_metadata(filesystem_t* fs,
                            const char* path,
                            file_metadata_t* metadata);

// Send file response with proper headers
int filesystem_send_file(filesystem_t* fs,
                        connection_t* conn,
                        const char* path,
                        file_metadata_t* metadata);

// Stream file content efficiently
int filesystem_stream_file(int file_fd,
                          int socket_fd,
                          size_t file_size,
                          uint8_t* buffer,
                          size_t buffer_size);

// Get MIME type from file extension
const char* filesystem_get_mime_type(const char* path);

// Default MIME types
extern const mime_type_t default_mime_types[];
extern const size_t default_mime_types_count;

// Path security validation
// Returns true if path is safe to use, false if it contains traversal attempts
bool filesystem_validate_path(const char* path);

#ifdef __cplusplus
}
#endif

#endif // _FILESYSTEM_H_