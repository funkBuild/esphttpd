#ifndef _TEMPLATE_H_
#define _TEMPLATE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Template variable callback function
// Returns number of bytes written, or -1 for error
typedef int (*template_var_callback_t)(const char* var_name,
                                       uint8_t* output,
                                       size_t output_size,
                                       void* user_data);

// Template processing configuration
typedef struct {
    const char* start_delim;     // Variable start delimiter (e.g., "{{")
    const char* end_delim;       // Variable end delimiter (e.g., "}}")
    uint8_t delim_len_start;     // Length of start delimiter
    uint8_t delim_len_end;       // Length of end delimiter
    bool escape_html;            // Auto-escape HTML characters
} template_config_t;

// Template processing context
typedef struct {
    template_config_t config;
    template_var_callback_t callback;
    void* user_data;

    // Internal state
    uint8_t state;              // Parser state
    uint8_t var_name_len;       // Current variable name length
    uint8_t delim_pos;          // Position in delimiter matching
    char var_name[64];          // Variable name buffer
} template_context_t;

// Template processing results
typedef enum {
    TEMPLATE_OK,          // Processing successful
    TEMPLATE_NEED_MORE,   // Need more input data
    TEMPLATE_ERROR,       // Error occurred
    TEMPLATE_COMPLETE     // Template fully processed
} template_result_t;

// Initialize template context with default delimiters {{ }}
void template_init_default(template_context_t* ctx,
                          template_var_callback_t callback,
                          void* user_data);

// Initialize template context with custom configuration
void template_init(template_context_t* ctx,
                  const template_config_t* config,
                  template_var_callback_t callback,
                  void* user_data);

// Process template data (streaming)
// Returns number of bytes written to output
int template_process(template_context_t* ctx,
                    const uint8_t* input,
                    size_t input_len,
                    uint8_t* output,
                    size_t output_size);

// Flush any partial delimiters (for end of processing)
// Returns number of bytes written to output
int template_flush(template_context_t* ctx,
                  uint8_t* output,
                  size_t output_size);

// Process template file
int template_process_file(template_context_t* ctx,
                         int in_fd,
                         int out_fd,
                         uint8_t* buffer,
                         size_t buffer_size);

// Helper function to escape HTML characters
int template_escape_html(const uint8_t* input,
                        size_t input_len,
                        uint8_t* output,
                        size_t output_size);

// Built-in variable handlers
int template_var_env(const char* var_name, uint8_t* output,
                    size_t output_size, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // _TEMPLATE_H_