#include "private/filesystem.h"
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "FILESYSTEM";

// Send function callback - set by server to route through send_nonblocking().
// When NULL, falls back to blocking write() for backward compatibility (tests).
static fs_send_func_t s_send_func = NULL;

// File stream function callback - set by server to route through send_buffer.
// When NULL, falls back to blocking read/send loop (tests).
static fs_start_file_stream_func_t s_file_stream_func = NULL;

void fs_set_send_func(fs_send_func_t func) {
    s_send_func = func;
}

void fs_set_file_stream_func(fs_start_file_stream_func_t func) {
    s_file_stream_func = func;
}

// Send helper: uses non-blocking callback when available, falls back to blocking write
static ssize_t fs_send(connection_t* conn, const void* data, size_t len) {
    if (s_send_func) {
        return s_send_func(conn, data, len);
    }
    // Fallback for tests: blocking write
    return write(conn->fd, data, len);
}

// Format uint32_t as decimal digits. Returns number of digits written.
static inline int format_uint(char* buf, uint32_t value) {
    char tmp[10]; int n = 0;
    do { tmp[n++] = '0' + (value % 10); value /= 10; } while (value);
    for (int i = n - 1; i >= 0; i--) *buf++ = tmp[i];
    return n;
}

// Default MIME type mappings - ordered by request frequency for early exit
// ext_len is precomputed to avoid strlen() in hot path
const mime_type_t default_mime_types[] = {
    // Most frequently requested (web assets)
    {".html", 5, "text/html", true, false},
    {".js",   3, "application/javascript", true, true},
    {".css",  4, "text/css", true, true},
    {".json", 5, "application/json", true, false},
    {".png",  4, "image/png", false, true},
    {".jpg",  4, "image/jpeg", false, true},
    {".svg",  4, "image/svg+xml", true, true},
    {".ico",  4, "image/x-icon", false, true},

    // Less frequent
    {".htm",  4, "text/html", true, false},
    {".xml",  4, "application/xml", true, false},
    {".jpeg", 5, "image/jpeg", false, true},
    {".gif",  4, "image/gif", false, true},
    {".webp", 5, "image/webp", false, true},
    {".txt",  4, "text/plain", true, false},

    // Fonts
    {".woff2", 6, "font/woff2", false, true},
    {".woff",  5, "font/woff", false, true},
    {".ttf",   4, "font/ttf", false, true},
    {".otf",   4, "font/otf", false, true},

    // Rare
    {".pdf",  4, "application/pdf", false, true},
    {".zip",  4, "application/zip", false, false},
    {".gz",   3, "application/gzip", false, false},
};

const size_t default_mime_types_count = sizeof(default_mime_types) / sizeof(mime_type_t);

int filesystem_init_default(filesystem_t* fs) {
    filesystem_config_t config = {
        .base_path = "/www",
        .max_open_files = 5,
        .format_on_fail = false,
        .partition_size = 0,  // Use full partition
        .partition_label = "littlefs"
    };
    return filesystem_init(fs, &config);
}

int filesystem_init(filesystem_t* fs, const filesystem_config_t* config) {
    if (fs->mounted) {
        ESP_LOGW(TAG, "Filesystem already mounted");
        return 0;
    }

    // Configure LittleFS
    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = config->base_path,
        .partition_label = config->partition_label,
        .format_if_mount_failed = config->format_on_fail,
        .dont_mount = false
    };

    // Register and mount LittleFS
    esp_err_t ret = esp_vfs_littlefs_register(&lfs_conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount LittleFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return -1;
    }

    // Store configuration
    strncpy(fs->base_path, config->base_path, sizeof(fs->base_path) - 1);
    fs->base_path_len = (uint8_t)strlen(fs->base_path);
    fs->mounted = true;
    fs->open_files = 0;

    // Get filesystem info
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(config->partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted at %s (total: %d KB, used: %d KB)",
                 config->base_path, total / 1024, used / 1024);
    }

    return 0;
}

void filesystem_unmount(filesystem_t* fs) {
    if (!fs->mounted) {
        return;
    }

    esp_vfs_littlefs_unregister(fs->base_path);
    fs->mounted = false;
    ESP_LOGI(TAG, "Filesystem unmounted");
}

bool filesystem_file_exists(filesystem_t* fs, const char* path) {
    if (!fs->mounted) return false;

    char full_path[128];
    // Build path with memcpy using cached base_path_len
    size_t input_len = strlen(path);
    size_t path_len = fs->base_path_len + input_len;
    if (path_len >= sizeof(full_path)) return false;
    memcpy(full_path, fs->base_path, fs->base_path_len);
    memcpy(full_path + fs->base_path_len, path, input_len + 1);

    struct stat st;
    if (stat(full_path, &st) == 0) {
        return !S_ISDIR(st.st_mode);
    }

    // Check for gzipped version - append .gz to existing path
    if (path_len + 3 < sizeof(full_path)) {
        full_path[path_len] = '.';
        full_path[path_len + 1] = 'g';
        full_path[path_len + 2] = 'z';
        full_path[path_len + 3] = '\0';
        if (stat(full_path, &st) == 0) {
            return !S_ISDIR(st.st_mode);
        }
    }

    return false;
}

// Helper to get both MIME type and cache flag in single lookup
static const mime_type_t* filesystem_find_mime_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) {
        return NULL;
    }

    // Calculate extension length once for fast filtering
    size_t ext_len = strlen(ext);

    // Early reject for invalid lengths (all extensions are 3-6 chars including dot)
    if (__builtin_expect(ext_len < 3 || ext_len > 6, 0)) {
        return NULL;
    }

    for (size_t i = 0; i < default_mime_types_count; i++) {
        // Use precomputed ext_len for fast integer comparison before strcasecmp
        if (ext_len == default_mime_types[i].ext_len &&
            strcasecmp(ext, default_mime_types[i].extension) == 0) {
            return &default_mime_types[i];
        }
    }

    return NULL;
}

bool filesystem_get_metadata(filesystem_t* fs,
                            const char* path,
                            file_metadata_t* metadata) {
    if (!fs->mounted) return false;

    struct stat st;
    bool is_gzipped = false;

    // Build path directly into metadata->full_path with memcpy (avoids rebuild in send_file)
    size_t input_len = strlen(path);
    size_t path_len = fs->base_path_len + input_len;
    if (path_len >= sizeof(metadata->full_path)) return false;
    memcpy(metadata->full_path, fs->base_path, fs->base_path_len);
    memcpy(metadata->full_path + fs->base_path_len, path, input_len + 1);
    if (stat(metadata->full_path, &st) != 0) {
        // Try gzipped version - append .gz to existing path
        if (path_len + 3 < sizeof(metadata->full_path)) {
            metadata->full_path[path_len] = '.';
            metadata->full_path[path_len + 1] = 'g';
            metadata->full_path[path_len + 2] = 'z';
            metadata->full_path[path_len + 3] = '\0';
            if (stat(metadata->full_path, &st) != 0) {
                return false;
            }
            is_gzipped = true;
        } else {
            return false;
        }
    }

    metadata->size = st.st_size;
    metadata->mtime = st.st_mtime;
    metadata->is_directory = S_ISDIR(st.st_mode);
    metadata->is_gzipped = is_gzipped;

    // Single MIME lookup sets both mime_type and cacheable (avoids duplicate in send_file)
    const mime_type_t* mime = filesystem_find_mime_type(path);
    metadata->mime_type = mime ? mime->mime_type : "application/octet-stream";
    metadata->cacheable = mime ? mime->cache : false;

    return true;
}

const char* filesystem_get_mime_type(const char* path) {
    const mime_type_t* mime = filesystem_find_mime_type(path);
    return mime ? mime->mime_type : "application/octet-stream";
}

bool filesystem_validate_path(const char* path) {
    if (!path || !*path) return false;

    // Single-pass validation: check all dangerous patterns in one scan
    char prev = 0;
    for (const char* p = path; *p; p++) {
        char c = *p;
        if (c == '\\') return false;
        if (c == '.' && prev == '.') return false;
        if (c == '/' && prev == '/') return false;
        if (c == '%' && p[1] && p[2]) {
            // Check percent-encoded dangerous chars
            char c1 = p[1], c2 = p[2] | 0x20;  // lowercase the second hex digit
            if ((c1 == '2' && (c2 == 'e' || c2 == 'f')) ||  // %2e=. %2f=/
                (c1 == '5' && c2 == 'c') ||                  // %5c=backslash
                (c1 == '0' && p[2] == '0')) return false;     // %00=null
        }
        prev = c;
    }
    return true;
}

int filesystem_serve_file(filesystem_t* fs,
                         connection_t* conn,
                         const char* path,
                         bool use_template) {
    if (!fs->mounted) {
        return -1;
    }

    // Security: validate path before processing
    if (!filesystem_validate_path(path)) {
        return -1;
    }

    // Buffer for index path if needed
    char index_path[128];
    const char* actual_path = path;
    size_t path_len = strlen(path);

    // Get file metadata
    file_metadata_t metadata;
    if (!filesystem_get_metadata(fs, actual_path, &metadata)) {
        // Try index.html for directories
        if (path_len > 0 && path[path_len - 1] == '/') {
            static const char index_suffix[] = "index.html";
            if (path_len + sizeof(index_suffix) > sizeof(index_path)) return -1;
            memcpy(index_path, path, path_len);
            memcpy(index_path + path_len, index_suffix, sizeof(index_suffix));
            if (!filesystem_get_metadata(fs, index_path, &metadata)) {
                return -1;
            }
            actual_path = index_path;
        } else {
            return -1;
        }
    }

    if (metadata.is_directory) {
        // Could return directory listing if enabled
        return -1;
    }

    // Send file
    return filesystem_send_file(fs, conn, actual_path, &metadata);
}

int filesystem_send_file(filesystem_t* fs,
                        connection_t* conn,
                        const char* path,
                        file_metadata_t* metadata) {
    (void)fs;    // No longer needed: path is pre-built in metadata
    (void)path;  // Path is now pre-built in metadata->full_path
    const char* full_path = metadata->full_path;

    // Open file
    int file_fd = open(full_path, O_RDONLY);
    if (file_fd < 0) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return -1;
    }

    // Send HTTP headers: memcpy static parts + dynamic mime_type and size
    char headers[512];
    static const char h1[] = "HTTP/1.1 200 OK\r\nContent-Type: ";
    static const char h2[] = "\r\nContent-Length: ";
    static const char h3[] = "\r\n";
    char* p = headers;
    memcpy(p, h1, sizeof(h1) - 1); p += sizeof(h1) - 1;
    size_t mime_len = strlen(metadata->mime_type);
    memcpy(p, metadata->mime_type, mime_len); p += mime_len;
    memcpy(p, h2, sizeof(h2) - 1); p += sizeof(h2) - 1;
    p += format_uint(p, metadata->size);
    memcpy(p, h3, sizeof(h3) - 1); p += sizeof(h3) - 1;
    int header_len = (int)(p - headers);

    // Append conditional static headers with memcpy
    if (metadata->is_gzipped) {
        static const char ce_gzip[] = "Content-Encoding: gzip\r\n";
        memcpy(headers + header_len, ce_gzip, sizeof(ce_gzip) - 1);
        header_len += sizeof(ce_gzip) - 1;
    }

    if (metadata->cacheable) {
        static const char cc_cache[] = "Cache-Control: public, max-age=86400\r\n";
        memcpy(headers + header_len, cc_cache, sizeof(cc_cache) - 1);
        header_len += sizeof(cc_cache) - 1;
    }

    // End headers
    headers[header_len++] = '\r';
    headers[header_len++] = '\n';

    // Send headers via non-blocking send
    if (fs_send(conn, headers, header_len) < 0) {
        close(file_fd);
        return -1;
    }

    // Stream file content - use non-blocking file streaming when available
    if (s_file_stream_func) {
        // Non-blocking: hand off file to send_buffer infrastructure.
        // Ownership of file_fd transfers to the stream function (it will close it).
        if (s_file_stream_func(conn, file_fd, metadata->size) < 0) {
            close(file_fd);
            return -1;
        }
        return (int)metadata->size;
    }

    // Fallback: blocking read/send loop (for tests without server infrastructure)
    uint8_t buffer[1024];
    int total_sent = filesystem_stream_file(file_fd, conn->fd,
                                           metadata->size,
                                           buffer, sizeof(buffer));

    close(file_fd);
    return total_sent;
}

int filesystem_stream_file(int file_fd,
                          int socket_fd,
                          size_t file_size,
                          uint8_t* buffer,
                          size_t buffer_size) {
    size_t total_sent = 0;
    ssize_t bytes_read;

    while (total_sent < file_size &&
           (bytes_read = read(file_fd, buffer,
                             (file_size - total_sent > buffer_size) ?
                             buffer_size : (file_size - total_sent))) > 0) {
        // Handle partial sends - loop until entire buffer is sent
        size_t sent_from_buffer = 0;
        while (sent_from_buffer < (size_t)bytes_read) {
            ssize_t bytes_sent = send(socket_fd, buffer + sent_from_buffer,
                                      bytes_read - sent_from_buffer, 0);
            if (bytes_sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket buffer full, yield briefly and retry
                    vTaskDelay(1);
                    continue;
                }
                ESP_LOGE(TAG, "Failed to send file data: %s", strerror(errno));
                return -1;
            }
            sent_from_buffer += bytes_sent;
        }
        total_sent += sent_from_buffer;
    }

    return total_sent;
}

