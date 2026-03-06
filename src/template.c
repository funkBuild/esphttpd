#include "private/template.h"
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include "esp_log.h"

static const char TAG[] = "TEMPLATE";

// Template parser states
enum {
    TEMPLATE_STATE_TEXT,        // Processing normal text
    TEMPLATE_STATE_DELIM_START, // Matching start delimiter
    TEMPLATE_STATE_VAR_NAME,    // Reading variable name
    TEMPLATE_STATE_DELIM_END    // Matching end delimiter
};

void template_init_default(template_context_t* ctx,
                          template_var_callback_t callback,
                          void* user_data) {
    template_config_t config = {
        .start_delim = "{{",
        .end_delim = "}}",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = true
    };
    template_init(ctx, &config, callback, user_data);
}

void template_init(template_context_t* ctx,
                  const template_config_t* config,
                  template_var_callback_t callback,
                  void* user_data) {
    if (!ctx || !config) return;
    memcpy(&ctx->config, config, sizeof(template_config_t));

    // Copy delimiter strings into owned buffers to prevent dangling pointers
    if (config->start_delim && config->delim_len_start > 0) {
        size_t len = config->delim_len_start < sizeof(ctx->start_delim_buf) - 1
                   ? config->delim_len_start : sizeof(ctx->start_delim_buf) - 1;
        memcpy(ctx->start_delim_buf, config->start_delim, len);
        ctx->start_delim_buf[len] = '\0';
        ctx->config.start_delim = ctx->start_delim_buf;
        ctx->config.delim_len_start = len;
    }
    if (config->end_delim && config->delim_len_end > 0) {
        size_t len = config->delim_len_end < sizeof(ctx->end_delim_buf) - 1
                   ? config->delim_len_end : sizeof(ctx->end_delim_buf) - 1;
        memcpy(ctx->end_delim_buf, config->end_delim, len);
        ctx->end_delim_buf[len] = '\0';
        ctx->config.end_delim = ctx->end_delim_buf;
        ctx->config.delim_len_end = len;
    }

    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->state = TEMPLATE_STATE_TEXT;
    ctx->var_name_len = 0;
    ctx->delim_pos = 0;
}

int template_process(template_context_t* ctx,
                    const uint8_t* input,
                    size_t input_len,
                    uint8_t* output,
                    size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    // Cache delimiter config in local variables for faster access
    const char* start_delim = ctx->config.start_delim;
    const char* end_delim = ctx->config.end_delim;
    uint8_t delim_len_start = ctx->config.delim_len_start;
    uint8_t delim_len_end = ctx->config.delim_len_end;

    // Guard against invalid delimiter config
    if (!start_delim || !end_delim || delim_len_start == 0 || delim_len_end == 0) {
        size_t to_copy = input_len < output_size ? input_len : output_size;
        memcpy(output, input, to_copy);
        return to_copy;
    }

    char first_delim_char = start_delim[0];

    // Fast path: if in TEXT state with no partial delimiter match,
    // scan for first delimiter char using memchr and copy plain text in bulk
    if (ctx->state == TEMPLATE_STATE_TEXT && ctx->delim_pos == 0) {
        const uint8_t* delim_start = (const uint8_t*)memchr(input, first_delim_char, input_len);
        if (!delim_start) {
            // No delimiter found - copy entire input
            size_t to_copy = input_len < output_size ? input_len : output_size;
            memcpy(output, input, to_copy);
            return to_copy;
        }
        // Copy text before delimiter
        size_t plain_len = delim_start - input;
        if (plain_len > 0) {
            size_t to_copy = plain_len < output_size ? plain_len : output_size;
            memcpy(output, input, to_copy);
            in_pos = plain_len;
            out_pos = to_copy;
        }
    }

    while (in_pos < input_len && out_pos < output_size) {
        uint8_t c = input[in_pos];

        switch (ctx->state) {
            case TEMPLATE_STATE_TEXT:
                // Most common case: normal text character (not start of delimiter)
                // Bulk scan: use memchr to find next potential delimiter start
                if (__builtin_expect(c != first_delim_char, 1) && ctx->delim_pos == 0) {
                    // Bulk copy text until next delimiter char or end of input
                    const uint8_t* next_delim = (const uint8_t*)memchr(&input[in_pos + 1], first_delim_char, input_len - in_pos - 1);
                    size_t span = next_delim ? (size_t)(next_delim - &input[in_pos]) : (input_len - in_pos);
                    size_t avail = output_size - out_pos;
                    if (span > avail) span = avail;
                    memcpy(output + out_pos, &input[in_pos], span);
                    out_pos += span;
                    in_pos += span;
                    continue;
                } else if (c == start_delim[ctx->delim_pos]) {
                    ctx->delim_pos++;
                    if (ctx->delim_pos == delim_len_start) {
                        // Found complete start delimiter
                        ctx->state = TEMPLATE_STATE_VAR_NAME;
                        ctx->var_name_len = 0;
                        ctx->delim_pos = 0;
                    }
                } else if (__builtin_expect(ctx->delim_pos > 0, 0)) {
                    // Partial delimiter match failed, output buffered chars using memcpy
                    size_t to_copy = ctx->delim_pos;
                    size_t avail = output_size - out_pos;
                    if (to_copy > avail) {
                        // Not enough space - output what we can, keep remainder buffered
                        memcpy(output + out_pos, start_delim, avail);
                        out_pos += avail;
                        // Shift remaining delimiter chars (rare path)
                        ctx->delim_pos -= avail;
                        break;  // Output full, exit loop
                    }
                    memcpy(output + out_pos, start_delim, to_copy);
                    out_pos += to_copy;
                    ctx->delim_pos = 0;
                    // Reprocess current character
                    continue;
                } else {
                    // Normal text character (boundary already checked by outer loop)
                    output[out_pos++] = c;
                }
                break;

            case TEMPLATE_STATE_VAR_NAME:
                // Check for end delimiter
                if (c == end_delim[ctx->delim_pos]) {
                    ctx->delim_pos++;
                    if (ctx->delim_pos == delim_len_end) {
                        // Found complete end delimiter
                        ctx->var_name[ctx->var_name_len] = '\0';

                        // Call variable callback
                        if (ctx->callback) {
                            size_t avail = output_size - out_pos;
                            if (ctx->config.escape_html) {
                                // Write to temp buffer, then escape into output
                                uint8_t tmp[128];
                                size_t tmp_avail = sizeof(tmp) < avail ? sizeof(tmp) : avail;
                                int written = ctx->callback(ctx->var_name,
                                                           tmp, tmp_avail,
                                                           ctx->user_data);
                                if (written > 0) {
                                    size_t src_len = ((size_t)written <= tmp_avail) ? (size_t)written : tmp_avail;
                                    int escaped = template_escape_html(tmp, src_len,
                                                                      output + out_pos, avail);
                                    if (escaped > 0) out_pos += escaped;
                                } else if (written < 0) {
                                    ESP_LOGW(TAG, "Variable callback error for '%s'", ctx->var_name);
                                }
                            } else {
                                int written = ctx->callback(ctx->var_name,
                                                           output + out_pos,
                                                           avail,
                                                           ctx->user_data);
                                if (written > 0) {
                                    out_pos += ((size_t)written <= avail) ? (size_t)written : avail;
                                } else if (written < 0) {
                                    ESP_LOGW(TAG, "Variable callback error for '%s'", ctx->var_name);
                                }
                            }
                        }

                        ctx->state = TEMPLATE_STATE_TEXT;
                        ctx->delim_pos = 0;
                    }
                } else if (ctx->delim_pos > 0) {
                    // Partial end delimiter match failed
                    // Add buffered delimiter chars to variable name using memcpy
                    size_t to_copy = ctx->delim_pos;
                    size_t space_left = sizeof(ctx->var_name) - 1 - ctx->var_name_len;
                    if (to_copy > space_left) to_copy = space_left;
                    memcpy(ctx->var_name + ctx->var_name_len, end_delim, to_copy);
                    ctx->var_name_len += to_copy;
                    ctx->delim_pos = 0;
                    // Reprocess current character
                    continue;
                } else {
                    // Variable name character
                    if (ctx->var_name_len < sizeof(ctx->var_name) - 1) {
                        ctx->var_name[ctx->var_name_len++] = c;
                    } else {
                        ESP_LOGE(TAG, "Variable name too long");
                        ctx->state = TEMPLATE_STATE_TEXT;
                        ctx->delim_pos = 0;
                    }
                }
                break;
        }

        in_pos++;
    }

    // Don't null terminate - let caller handle it
    // The function should return the number of bytes written
    // without including null terminator
    return (out_pos <= (size_t)INT_MAX) ? (int)out_pos : INT_MAX;
}

int template_flush(template_context_t* ctx,
                  uint8_t* output,
                  size_t output_size) {
    size_t out_pos = 0;

    // Flush any partial delimiter when processing is complete
    if (ctx->state == TEMPLATE_STATE_TEXT && ctx->delim_pos > 0) {
        // Output the partial delimiter using memcpy (faster than byte-by-byte)
        size_t to_copy = ctx->delim_pos;
        if (to_copy > output_size) to_copy = output_size;
        memcpy(output, ctx->config.start_delim, to_copy);
        out_pos = to_copy;
        ctx->delim_pos = 0;
    } else if (ctx->state == TEMPLATE_STATE_VAR_NAME) {
        // We were in the middle of reading a variable name
        // Output the start delimiter using memcpy
        size_t delim_to_copy = ctx->config.delim_len_start;
        if (delim_to_copy > output_size) delim_to_copy = output_size;
        memcpy(output, ctx->config.start_delim, delim_to_copy);
        out_pos = delim_to_copy;

        // Output variable name using memcpy
        if (out_pos < output_size && ctx->var_name_len > 0) {
            size_t var_to_copy = ctx->var_name_len;
            if (var_to_copy > output_size - out_pos) var_to_copy = output_size - out_pos;
            memcpy(output + out_pos, ctx->var_name, var_to_copy);
            out_pos += var_to_copy;
        }

        // Issue #51: Output partially-matched end-delimiter characters
        if (out_pos < output_size && ctx->delim_pos > 0) {
            size_t end_to_copy = ctx->delim_pos;
            if (end_to_copy > output_size - out_pos) end_to_copy = output_size - out_pos;
            memcpy(output + out_pos, ctx->config.end_delim, end_to_copy);
            out_pos += end_to_copy;
        }

        // Reset state
        ctx->state = TEMPLATE_STATE_TEXT;
        ctx->var_name_len = 0;
        ctx->delim_pos = 0;
    }

    // Null terminate if there's room
    if (out_pos < output_size) {
        output[out_pos] = '\0';
    }

    return out_pos;
}

int template_process_file(template_context_t* ctx,
                         int in_fd,
                         int out_fd,
                         uint8_t* buffer,
                         size_t buffer_size) {
    // Use half buffer for input, half for output
    size_t half_size = buffer_size / 2;
    uint8_t* in_buffer = buffer;
    uint8_t* out_buffer = buffer + half_size;

    int total_written = 0;
    ssize_t bytes_read;

    while ((bytes_read = read(in_fd, in_buffer, half_size)) > 0) {
        int bytes_processed = template_process(ctx,
                                              in_buffer,
                                              bytes_read,
                                              out_buffer,
                                              half_size);
        if (bytes_processed > 0) {
            ssize_t written = 0;
            while (written < bytes_processed) {
                ssize_t w = write(out_fd, out_buffer + written, bytes_processed - written);
                if (w <= 0) {
                    ESP_LOGE(TAG, "Failed to write output");
                    return -1;
                }
                written += w;
            }
            total_written += bytes_processed;
        }
    }

    // Flush any trailing partial content from the state machine
    int flushed = template_flush(ctx, out_buffer, half_size);
    if (flushed > 0) {
        ssize_t written = 0;
        while (written < flushed) {
            ssize_t w = write(out_fd, out_buffer + written, flushed - written);
            if (w <= 0) {
                ESP_LOGE(TAG, "Failed to write flush output");
                return -1;
            }
            written += w;
        }
        total_written += flushed;
    }

    return total_written;
}

int template_escape_html(const uint8_t* input,
                        size_t input_len,
                        uint8_t* output,
                        size_t output_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;

    // Single unified loop with inline escape info
    // Eliminates duplicate switch statement and improves I-cache utilization
    while (in_pos < input_len && out_pos < output_size) {
        uint8_t c = input[in_pos];
        const char* escape = NULL;
        size_t escape_len = 1;

        // Single switch for all cases
        switch (c) {
            case '<': escape = "&lt;"; escape_len = 4; break;
            case '>': escape = "&gt;"; escape_len = 4; break;
            case '&': escape = "&amp;"; escape_len = 5; break;
            case '"': escape = "&quot;"; escape_len = 6; break;
            case '\'': escape = "&#x27;"; escape_len = 6; break;
        }

        // Single boundary check
        if (__builtin_expect(out_pos + escape_len > output_size, 0)) break;

        in_pos++;
        if (escape) {
            memcpy(output + out_pos, escape, escape_len);
        } else {
            output[out_pos] = c;
        }
        out_pos += escape_len;
    }

    return out_pos;
}

// Built-in variable handler for environment variables
int template_var_env(const char* var_name, uint8_t* output,
                    size_t output_size, void* user_data) {
    // Skip "env." prefix if present
    if (memcmp(var_name, "env.", 4) == 0) {
        var_name += 4;
    }

    const char* value = getenv(var_name);
    if (!value) {
        return 0;
    }

    size_t len = strlen(value);
    if (len > output_size) {
        len = output_size;
    }

    memcpy(output, value, len);
    return len;
}