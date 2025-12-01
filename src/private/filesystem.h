#ifndef _FILESYSTEM_H_
#define _FILESYSTEM_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "connection.h"

#ifdef __cplusplus
extern "C" {
#endif

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
} file_metadata_t;

// File system context
typedef struct {
    bool mounted;
    char base_path[32];
    uint8_t open_files;
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

// Directory listing (optional, returns JSON)
int filesystem_list_directory(filesystem_t* fs,
                             connection_t* conn,
                             const char* path);

// Get MIME type from file extension
const char* filesystem_get_mime_type(const char* path);

// Default MIME types
extern const mime_type_t default_mime_types[];
extern const size_t default_mime_types_count;

// Cache control helpers
void filesystem_set_cache_headers(connection_t* conn,
                                 bool cacheable,
                                 uint32_t max_age);

// ETags support
bool filesystem_check_etag(connection_t* conn,
                          const char* etag);

void filesystem_set_etag(connection_t* conn,
                        file_metadata_t* metadata);

// Path security validation
// Returns true if path is safe to use, false if it contains traversal attempts
bool filesystem_validate_path(const char* path);

#ifdef __cplusplus
}
#endif

#endif // _FILESYSTEM_H_