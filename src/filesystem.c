#include "private/filesystem.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "FILESYSTEM";

// Helper to send all data, handling partial writes
static ssize_t send_all(int fd, const void* data, size_t len) {
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(1);
                continue;
            }
            ESP_LOGE(TAG, "send_all failed: %s", strerror(errno));
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }
    return (ssize_t)len;
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

    char full_path[256];
    // Build path once, then append .gz suffix instead of rebuilding
    size_t path_len = snprintf(full_path, sizeof(full_path), "%s%s", fs->base_path, path);

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

    char full_path[256];
    struct stat st;
    bool is_gzipped = false;

    // Build path once, then append .gz suffix instead of rebuilding
    size_t path_len = snprintf(full_path, sizeof(full_path), "%s%s", fs->base_path, path);
    if (stat(full_path, &st) != 0) {
        // Try gzipped version - append .gz to existing path
        if (path_len + 3 < sizeof(full_path)) {
            full_path[path_len] = '.';
            full_path[path_len + 1] = 'g';
            full_path[path_len + 2] = 'z';
            full_path[path_len + 3] = '\0';
            if (stat(full_path, &st) != 0) {
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
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    // Check for literal ".." sequences
    if (strstr(path, "..") != NULL) {
        ESP_LOGW(TAG, "Directory traversal attempt (..): %s", path);
        return false;
    }

    // Check for URL-encoded dangerous characters
    // %2e = '.', %2f = '/', %5c = '\' (backslash)
    const char* p = path;
    while ((p = strchr(p, '%')) != NULL) {
        if (p[1] != '\0' && p[2] != '\0') {
            char c1 = p[1];
            char c2 = p[2];

            // Check for %2e or %2E (URL-encoded '.')
            if ((c1 == '2' || c1 == '0') &&
                (c2 == 'e' || c2 == 'E')) {
                ESP_LOGW(TAG, "URL-encoded dot rejected: %s", path);
                return false;
            }

            // Check for %2f or %2F (URL-encoded '/')
            if ((c1 == '2' || c1 == '0') &&
                (c2 == 'f' || c2 == 'F')) {
                ESP_LOGW(TAG, "URL-encoded slash rejected: %s", path);
                return false;
            }

            // Check for %5c or %5C (URL-encoded backslash)
            if ((c1 == '5') && (c2 == 'c' || c2 == 'C')) {
                ESP_LOGW(TAG, "URL-encoded backslash rejected: %s", path);
                return false;
            }

            // Check for %00 (null byte)
            if (c1 == '0' && c2 == '0') {
                ESP_LOGW(TAG, "URL-encoded null byte rejected: %s", path);
                return false;
            }
        }
        p++;
    }

    // Reject double slashes (path confusion)
    if (strstr(path, "//") != NULL) {
        ESP_LOGW(TAG, "Double slash in path rejected: %s", path);
        return false;
    }

    // Reject backslashes
    if (strchr(path, '\\') != NULL) {
        ESP_LOGW(TAG, "Backslash in path rejected: %s", path);
        return false;
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
    static char index_path[256];
    const char* actual_path = path;

    // Get file metadata
    file_metadata_t metadata;
    if (!filesystem_get_metadata(fs, actual_path, &metadata)) {
        // Try index.html for directories
        if (path[strlen(path) - 1] == '/') {
            snprintf(index_path, sizeof(index_path), "%sindex.html", path);
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
    char full_path[256];
    if (metadata->is_gzipped) {
        snprintf(full_path, sizeof(full_path), "%s%s.gz", fs->base_path, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s", fs->base_path, path);
    }

    // Open file
    int file_fd = open(full_path, O_RDONLY);
    if (file_fd < 0) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return -1;
    }

    // Send HTTP headers
    char headers[512];
    int header_len = snprintf(headers, sizeof(headers),
                             "HTTP/1.1 200 OK\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %" PRIu32 "\r\n",
                             metadata->mime_type,
                             metadata->size);

    if (metadata->is_gzipped) {
        header_len += snprintf(headers + header_len,
                              sizeof(headers) - header_len,
                              "Content-Encoding: gzip\r\n");
    }

    // Add cache headers using pre-computed flag (avoids duplicate MIME table lookup)
    if (metadata->cacheable) {
        header_len += snprintf(headers + header_len,
                             sizeof(headers) - header_len,
                             "Cache-Control: public, max-age=86400\r\n");
    }

    header_len += snprintf(headers + header_len,
                          sizeof(headers) - header_len,
                          "\r\n");

    // Send headers
    if (send_all(conn->fd, headers, header_len) < 0) {
        close(file_fd);
        return -1;
    }

    // Stream file content
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

void filesystem_set_cache_headers(connection_t* conn,
                                 bool cacheable,
                                 uint32_t max_age) {
    // This would be integrated with the response sending
    // For now, just a placeholder
    (void)conn;
    (void)cacheable;
    (void)max_age;
}

bool filesystem_check_etag(connection_t* conn,
                          const char* etag) {
    // Would check If-None-Match header
    // Placeholder for now
    (void)conn;
    (void)etag;
    return false;
}

void filesystem_set_etag(connection_t* conn,
                        file_metadata_t* metadata) {
    // Would generate and set ETag header
    // Placeholder for now
    (void)conn;
    (void)metadata;
}