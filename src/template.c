#include "private/template.h"
#include <string.h>
#include <unistd.h>
#include "esp_log.h"

static const char* TAG = "TEMPLATE";

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
    memcpy(&ctx->config, config, sizeof(template_config_t));
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
                // Check if NOT start delimiter first (most common path)
                if (__builtin_expect(c != first_delim_char, 1) && ctx->delim_pos == 0) {
                    // Fast path: normal text character
                    output[out_pos++] = c;
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
                    if (out_pos + to_copy > output_size) {
                        to_copy = output_size - out_pos;
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
                            int written = ctx->callback(ctx->var_name,
                                                       output + out_pos,
                                                       output_size - out_pos,
                                                       ctx->user_data);
                            if (written > 0) {
                                out_pos += written;
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
    return out_pos;
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
            if (write(out_fd, out_buffer, bytes_processed) != bytes_processed) {
                ESP_LOGE(TAG, "Failed to write output");
                return -1;
            }
            total_written += bytes_processed;
        }
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
    if (strncmp(var_name, "env.", 4) == 0) {
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