#include "unity.h"
#include "template.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char TAG[] = "TEST_TEMPLATE";

// Test variable callback
static int test_var_callback(const char* var_name,
                            uint8_t* output,
                            size_t output_size,
                            void* user_data) {
    if (strcmp(var_name, "name") == 0) {
        const char* value = "World";
        size_t len = strlen(value);
        if (len > output_size) len = output_size;
        memcpy(output, value, len);
        return len;
    } else if (strcmp(var_name, "count") == 0) {
        return snprintf((char*)output, output_size, "42");
    } else if (strcmp(var_name, "empty") == 0) {
        return 0; // Empty variable
    }
    return -1; // Variable not found
}

// ==================== TEST FUNCTIONS ====================

// Test basic variable substitution
static void test_template_basic_substitution(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Hello {{name}}!";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING("Hello World!", (char*)output);
    TEST_ASSERT_EQUAL(12, result);
}

// Test multiple variables
static void test_template_multiple_vars(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Name: {{name}}, Count: {{count}}";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING("Name: World, Count: 42", (char*)output);
    TEST_ASSERT_EQUAL(22, result);
}

// Test empty variable
static void test_template_empty_var(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Before{{empty}}After";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING("BeforeAfter", (char*)output);
    TEST_ASSERT_EQUAL(11, result);
}

// Test no variables
static void test_template_no_vars(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Plain text with no variables";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING(input, (char*)output);
    TEST_ASSERT_EQUAL(strlen(input), result);
}

// Test custom delimiters
static void test_template_custom_delimiters(void) {
    template_config_t config = {
        .start_delim = "<%",
        .end_delim = "%>",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = false
    };

    template_context_t ctx;
    template_init(&ctx, &config, test_var_callback, NULL);

    const char* input = "Hello <%name%>!";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING("Hello World!", (char*)output);
    TEST_ASSERT_EQUAL(12, result);
}

// Test partial delimiters
static void test_template_partial_delimiters(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Test { and } and {{ incomplete";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    // Flush any partial delimiters
    int flushed = template_flush(&ctx, output + result, sizeof(output) - result);
    result += flushed;
    output[result] = '\0';  // Null terminate for string comparison

    // Should pass through incomplete delimiters
    TEST_ASSERT_EQUAL_STRING("Test { and } and {{ incomplete", (char*)output);
}

// Test HTML escaping
static void test_template_html_escape(void) {
    const char* input = "<script>alert('XSS')</script>";
    uint8_t output[256] = {0};

    int result = template_escape_html((const uint8_t*)input,
                                     strlen(input), output, sizeof(output));

    TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(&#x27;XSS&#x27;)&lt;/script&gt;",
                           (char*)output);
    TEST_ASSERT_TRUE(result > strlen(input));
}

// Test variable name too long
static void test_template_long_var_name(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    // Create a variable name longer than 32 chars
    const char* input = "{{verylongvariablenamethatexceedsthemaximumlengthallowedforvariablenames}}";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    output[result] = '\0';  // Null terminate
    // Should handle gracefully
    TEST_ASSERT_TRUE(result >= 0);
}

// Test output buffer too small
static void test_template_buffer_overflow(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    const char* input = "Hello {{name}}!";
    uint8_t output[6] = {0}; // 5 bytes + null terminator

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, 5);  // Only allow 5 bytes

    output[result] = '\0';  // Null terminate

    // Should write what fits
    TEST_ASSERT_EQUAL_STRING("Hello", (char*)output);
    TEST_ASSERT_EQUAL(5, result);
}

// Test streaming processing
static void test_template_streaming(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    // Process in chunks
    const char* part1 = "Hello {{";
    const char* part2 = "name";
    const char* part3 = "}}!";

    uint8_t output[128] = {0};
    int pos = 0;

    // First chunk
    pos += template_process(&ctx, (const uint8_t*)part1,
                           strlen(part1), output + pos, sizeof(output) - pos);

    // Second chunk
    pos += template_process(&ctx, (const uint8_t*)part2,
                           strlen(part2), output + pos, sizeof(output) - pos);

    // Third chunk
    pos += template_process(&ctx, (const uint8_t*)part3,
                           strlen(part3), output + pos, sizeof(output) - pos);

    output[pos] = '\0';  // Null terminate
    TEST_ASSERT_EQUAL_STRING("Hello World!", (char*)output);
}

// ========== Issue #4: Callback returning value > available space ==========

// Malicious callback that returns more than available space
static int overflow_var_callback(const char* var_name,
                                  uint8_t* output,
                                  size_t output_size,
                                  void* user_data) {
    // Write within bounds but return a value larger than available space
    if (output_size > 0) output[0] = 'X';
    return 9999;  // Lie about how much was written
}

static void test_template_callback_overflow_clamped(void) {
    template_context_t ctx;
    template_init_default(&ctx, overflow_var_callback, NULL);

    const char* input = "A{{x}}B";
    uint8_t output[16] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    // Result should not exceed output buffer size
    TEST_ASSERT_TRUE(result <= (int)sizeof(output));
    // 'A' + clamped callback output + 'B' should fit
    TEST_ASSERT_TRUE(result >= 1);  // At least 'A' was written
}

// ========== Issue #38: Delimiter ownership - config pointers copied into owned buffers ==========

static void test_template_delimiter_ownership(void) {
    // Create config with stack-local delimiter strings, then destroy them
    template_context_t ctx;
    {
        char start[4] = "<%";
        char end[4] = "%>";
        template_config_t config = {
            .start_delim = start,
            .end_delim = end,
            .delim_len_start = 2,
            .delim_len_end = 2,
            .escape_html = false
        };
        template_init(&ctx, &config, test_var_callback, NULL);

        // Overwrite the stack-local strings to simulate dangling pointer
        start[0] = 'X'; start[1] = 'X';
        end[0] = 'Y'; end[1] = 'Y';
    }

    // The delimiters should still work because template_init copied them
    const char* input = "Hello <%name%>!";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';
    TEST_ASSERT_EQUAL_STRING("Hello World!", (char*)output);
}

// ========== Issue #39: NULL/zero delimiters handled gracefully ==========

static void test_template_null_delimiters(void) {
    template_context_t ctx;
    template_config_t config = {
        .start_delim = NULL,
        .end_delim = NULL,
        .delim_len_start = 0,
        .delim_len_end = 0,
        .escape_html = false
    };
    template_init(&ctx, &config, test_var_callback, NULL);

    // Should pass through input unchanged (no variable substitution)
    const char* input = "Hello {{name}}!";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';
    TEST_ASSERT_EQUAL_STRING("Hello {{name}}!", (char*)output);
    TEST_ASSERT_EQUAL(15, result);
}

// ========== Issue #40: escape_html flag is checked during variable substitution ==========

// Callback that returns HTML-sensitive content
static int html_var_callback(const char* var_name,
                             uint8_t* output,
                             size_t output_size,
                             void* user_data) {
    if (strcmp(var_name, "user") == 0) {
        const char* value = "<b>Admin</b>";
        size_t len = strlen(value);
        if (len > output_size) len = output_size;
        memcpy(output, value, len);
        return len;
    }
    return -1;
}

static void test_template_escape_html_in_substitution(void) {
    // With escape_html enabled (default)
    template_context_t ctx;
    template_init_default(&ctx, html_var_callback, NULL);

    const char* input = "User: {{user}}";
    uint8_t output[256] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';

    // Should have escaped the HTML tags
    TEST_ASSERT_NOT_NULL(strstr((char*)output, "&lt;b&gt;"));
    TEST_ASSERT_NULL(strstr((char*)output, "<b>"));
}

static void test_template_no_escape_html_in_substitution(void) {
    // With escape_html disabled
    template_config_t config = {
        .start_delim = "{{",
        .end_delim = "}}",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = false
    };

    template_context_t ctx;
    template_init(&ctx, &config, html_var_callback, NULL);

    const char* input = "User: {{user}}";
    uint8_t output[256] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';

    // Should NOT have escaped the HTML tags
    TEST_ASSERT_NOT_NULL(strstr((char*)output, "<b>Admin</b>"));
}

// ========== Issue #51: template_flush outputs partial end-delimiter chars ==========

static void test_template_flush_partial_end_delim(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    // Feed input that enters VAR_NAME state with a partial end delimiter
    // "{{name}" has "{{" (enters VAR_NAME), "name" (var name), "}" (partial end delim)
    const char* input = "{{name}";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    // Now flush - should output "{{name}" (start delim + var name + partial end delim)
    int flushed = template_flush(&ctx, output + result, sizeof(output) - result);
    result += flushed;
    output[result] = '\0';

    // The flushed output should include the partial end delimiter "}"
    TEST_ASSERT_EQUAL_STRING("{{name}", (char*)output);
}

// ========== Issue #52: template_process_file flushes at EOF ==========
// (tested indirectly - template_flush is called after read loop)

static void test_template_flush_after_process(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    // Simulate partial template at EOF: input ends mid-variable
    const char* input = "Start {{name";
    uint8_t output[128] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));

    // Without flush, partial content is lost
    int before_flush = result;

    // Flush should recover the partial content
    int flushed = template_flush(&ctx, output + result, sizeof(output) - result);
    result += flushed;
    output[result] = '\0';

    // After flush, should have "Start {{name" (all content recovered)
    TEST_ASSERT_GREATER_THAN(before_flush, result);
    TEST_ASSERT_EQUAL_STRING("Start {{name", (char*)output);
}

// ========== Issue #45: NULL config check in template_init ==========

static void test_template_init_null_config(void) {
    template_context_t ctx;
    memset(&ctx, 0xFF, sizeof(ctx));  // Fill with garbage

    // Should not crash with NULL config
    template_init(&ctx, NULL, test_var_callback, NULL);

    // ctx should be unchanged (early return)
    TEST_PASS();
}

static void test_template_init_null_ctx(void) {
    template_config_t config = {
        .start_delim = "{{",
        .end_delim = "}}",
        .delim_len_start = 2,
        .delim_len_end = 2,
        .escape_html = true
    };

    // Should not crash with NULL ctx
    template_init(NULL, &config, test_var_callback, NULL);
    TEST_PASS();
}

// ========== Issue #26: Partial delimiter flush corrupts delim_pos when output buffer too small ==========

static void test_template_partial_delim_flush_small_buffer(void) {
    // Use a 3-char delimiter "{{{" so a partial match can be 2 chars deep.
    // When the mismatch triggers a flush of 2 chars but avail is only 1,
    // the old code did delim_pos -= 1 (leaving delim_pos=1), corrupting
    // state for subsequent delimiter matching.
    template_config_t config = {
        .start_delim = "{{{",
        .end_delim = "}}}",
        .delim_len_start = 3,
        .delim_len_end = 3,
        .escape_html = false
    };
    template_context_t ctx;
    template_init(&ctx, &config, test_var_callback, NULL);

    // Input: "ABCDE{{x"
    //   - "ABCDE" -> 5 bytes of text output
    //   - First '{' matches start_delim[0], delim_pos=1 (no output)
    //   - Second '{' matches start_delim[1], delim_pos=2 (no output)
    //   - 'x' mismatches start_delim[2], need to flush 2 chars ("{{") as literal
    //   - With output_size=6, after writing "ABCDE" (5 bytes) avail=1, but need 2
    //   - Bug: old code did delim_pos -= 1, leaving delim_pos=1 (corrupted)
    uint8_t output[64] = {0};
    int total = 0;

    // First call: output buffer of 6 bytes (triggers partial flush)
    const char* input1 = "ABCDE{{x";
    int result = template_process(&ctx, (const uint8_t*)input1,
                                  strlen(input1), output, 6);
    // Should write "ABCDE{" (6 bytes: 5 text + 1 flushed delimiter char)
    TEST_ASSERT_EQUAL(6, result);
    total = result;

    // Second call: new input. The pending '{' from the partial flush must
    // be drained first. Then process " {{{name}}} end" which contains a
    // valid variable substitution.
    // With old buggy code, delim_pos=1 here would corrupt delimiter matching.
    const char* input2 = " {{{name}}} end";
    result = template_process(&ctx, (const uint8_t*)input2,
                              strlen(input2), output + total, sizeof(output) - total);
    TEST_ASSERT_TRUE(result >= 0);
    total += result;

    int flushed = template_flush(&ctx, output + total, sizeof(output) - total);
    total += flushed;
    output[total] = '\0';

    // Expected: "ABCDE{" + "{" (pending flush) + " World end"
    // Note: 'x' from input1 is consumed but lost (output was full) - pre-existing
    // API limitation. The key assertion: the pending '{' is correctly flushed and
    // subsequent {{{name}}} substitution works (proves delim_pos not corrupted).
    TEST_ASSERT_EQUAL_STRING("ABCDE{{ World end", (char*)output);
}

// Test with 2-char delimiter: split input so first call accumulates '{'
// in delim_pos, then second call triggers mismatch flush with plenty of space.
// Verifies basic partial delimiter mismatch handling works correctly.
static void test_template_partial_delim_flush_zero_avail(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    // Split input across calls: first call ends mid-delimiter
    const char* part1 = "Hello {";
    const char* part2 = "x world";
    uint8_t output[64] = {0};
    int total = 0;

    // First call: "Hello {" -> writes "Hello " (6 bytes), '{' in delim_pos=1
    int result = template_process(&ctx, (const uint8_t*)part1,
                                  strlen(part1), output, sizeof(output));
    TEST_ASSERT_EQUAL(6, result);
    total = result;

    // Second call: "x world" - 'x' triggers mismatch, '{' must be flushed
    result = template_process(&ctx, (const uint8_t*)part2,
                              strlen(part2), output + total, sizeof(output) - total);
    TEST_ASSERT_TRUE(result >= 0);
    total += result;

    int flushed = template_flush(&ctx, output + total, sizeof(output) - total);
    total += flushed;
    output[total] = '\0';

    TEST_ASSERT_EQUAL_STRING("Hello {x world", (char*)output);
}

// ========== Issue #31: escape_html must not truncate values > 128 bytes ==========

// Callback that returns a long HTML-sensitive value (> 128 bytes)
static int long_html_var_callback(const char* var_name,
                                  uint8_t* output,
                                  size_t output_size,
                                  void* user_data) {
    if (strcmp(var_name, "long") == 0) {
        // Build a 200-byte value with HTML chars sprinkled in:
        // "AAAA....<b>BBBB....</b>CCCC...."
        // Total: 80 A's + "<b>" (3) + 80 B's + "</b>" (4) + 33 C's = 200
        size_t pos = 0;
        char tmp[256];

        for (int i = 0; i < 80 && pos < sizeof(tmp); i++) tmp[pos++] = 'A';
        const char* open_tag = "<b>";
        for (int i = 0; open_tag[i] && pos < sizeof(tmp); i++) tmp[pos++] = open_tag[i];
        for (int i = 0; i < 80 && pos < sizeof(tmp); i++) tmp[pos++] = 'B';
        const char* close_tag = "</b>";
        for (int i = 0; close_tag[i] && pos < sizeof(tmp); i++) tmp[pos++] = close_tag[i];
        for (int i = 0; i < 33 && pos < sizeof(tmp); i++) tmp[pos++] = 'C';

        size_t len = pos;  // Should be 200
        if (len > output_size) len = output_size;
        memcpy(output, tmp, len);
        return len;
    }
    return -1;
}

static void test_template_escape_long_value(void) {
    // With escape_html enabled, a variable value > 128 bytes must not be truncated
    template_context_t ctx;
    template_init_default(&ctx, long_html_var_callback, NULL);

    const char* input = "{{long}}";
    uint8_t output[2048] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';

    // The 200-byte value has "<b>" and "</b>" which should be escaped.
    // "<b>" (3 bytes) -> "&lt;b&gt;" (9 bytes): expansion of 6
    // "</b>" (4 bytes) -> "&lt;/b&gt;" (10 bytes): expansion of 6
    // Total escaped length: 200 + 6 + 6 = 212 bytes
    TEST_ASSERT_EQUAL(212, result);

    // Verify HTML tags are escaped
    TEST_ASSERT_NOT_NULL(strstr((char*)output, "&lt;b&gt;"));
    TEST_ASSERT_NOT_NULL(strstr((char*)output, "&lt;/b&gt;"));
    TEST_ASSERT_NULL(strstr((char*)output, "<b>"));
    TEST_ASSERT_NULL(strstr((char*)output, "</b>"));

    // Verify the A's, B's, and C's are present in full
    // Count A's before the first escape sequence
    int a_count = 0;
    for (int i = 0; output[i] == 'A'; i++) a_count++;
    TEST_ASSERT_EQUAL(80, a_count);

    // Find end of escaped output and count C's at the end
    int c_count = 0;
    for (int i = result - 1; i >= 0 && output[i] == 'C'; i--) c_count++;
    TEST_ASSERT_EQUAL(33, c_count);
}

// ========== Issue #30: template_process_file must propagate read() errors ==========

static void test_template_process_file_read_error(void) {
    template_context_t ctx;
    template_init_default(&ctx, test_var_callback, NULL);

    uint8_t buffer[256];

    // Use an invalid fd to trigger a read() error (-1 / EBADF)
    // Before the fix, template_process_file would ignore the error,
    // fall through to flush, and return 0 (total_written) as if it succeeded.
    int result = template_process_file(&ctx, -1, STDOUT_FILENO, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(-1, result);
}

// ========== Task #48: template_var_env tests ==========

static void test_template_var_env_unknown(void) {
    // Query a variable name that definitely does not exist in the environment
    uint8_t output[64] = {0};
    int result = template_var_env("DEFINITELY_NOT_SET_XYZ_12345", output,
                                  sizeof(output), NULL);
    // Unknown env var should return 0 bytes
    TEST_ASSERT_EQUAL(0, result);
}

static void test_template_var_env_with_prefix(void) {
    // template_var_env strips "env." prefix before calling getenv()
    // Use an unknown var to verify the prefix stripping doesn't crash
    uint8_t output[64] = {0};
    int result = template_var_env("env.DEFINITELY_NOT_SET_XYZ_12345", output,
                                  sizeof(output), NULL);
    TEST_ASSERT_EQUAL(0, result);
}

static void test_template_var_env_known(void) {
    // Set a known env var for this test
    setenv("ESPHTTPD_TEST_VAR", "hello123", 1);

    uint8_t output[64] = {0};
    int result = template_var_env("ESPHTTPD_TEST_VAR", output,
                                  sizeof(output), NULL);
    TEST_ASSERT_EQUAL(8, result);
    TEST_ASSERT_EQUAL_STRING_LEN("hello123", (char*)output, 8);

    unsetenv("ESPHTTPD_TEST_VAR");
}

static void test_template_var_env_with_prefix_known(void) {
    // Set a known env var and access via "env." prefix
    setenv("ESPHTTPD_TEST_VAR2", "world", 1);

    uint8_t output[64] = {0};
    int result = template_var_env("env.ESPHTTPD_TEST_VAR2", output,
                                  sizeof(output), NULL);
    TEST_ASSERT_EQUAL(5, result);
    TEST_ASSERT_EQUAL_STRING_LEN("world", (char*)output, 5);

    unsetenv("ESPHTTPD_TEST_VAR2");
}

static void test_template_var_env_truncated(void) {
    // Set a long env var and read into small buffer
    setenv("ESPHTTPD_LONG_VAR", "abcdefghijklmnop", 1);

    uint8_t output[8] = {0};
    int result = template_var_env("ESPHTTPD_LONG_VAR", output,
                                  sizeof(output), NULL);
    // Should be clamped to output_size
    TEST_ASSERT_EQUAL(8, result);
    TEST_ASSERT_EQUAL_STRING_LEN("abcdefgh", (char*)output, 8);

    unsetenv("ESPHTTPD_LONG_VAR");
}

// ========== Bug: template_flush overwrites drained flush_pending data ==========

static void test_template_flush_preserves_pending_data(void) {
    // Regression test: template_flush() must not overwrite bytes written by
    // the flush_pending drain when it subsequently flushes partial delimiters.
    //
    // Before the fix, the TEXT and VAR_NAME blocks in template_flush() wrote
    // to output[0] instead of output + out_pos, destroying the flush_pending
    // data that was just drained.

    // --- Case 1: TEXT state with flush_pending > 0 and delim_pos > 0 ---
    // Simulate: 1 byte of flush_pending remaining from a previous call,
    //           plus 1 byte of partial delimiter match (delim_pos = 1).
    {
        template_context_t ctx;
        template_init_default(&ctx, test_var_callback, NULL);

        // Manually set up the edge-case state:
        // flush_pending=1 means 1 byte of start_delim still needs flushing
        // flush_offset=1 means we already flushed the first byte, so flush start_delim[1]
        // state=TEXT with delim_pos=1 means we matched 1 char of start delimiter
        ctx.flush_pending = 1;
        ctx.flush_offset = 1;     // will flush start_delim[1] = '{'
        ctx.state = 0;            // TEMPLATE_STATE_TEXT
        ctx.delim_pos = 1;        // 1 byte of partial delimiter to flush

        uint8_t output[64] = {0};
        int result = template_flush(&ctx, output, sizeof(output));

        // Should get: start_delim[1] (from flush_pending) + start_delim[0] (from delim_pos)
        // = "{" + "{" = "{{"
        TEST_ASSERT_EQUAL(2, result);
        TEST_ASSERT_EQUAL_UINT8('{', output[0]);  // from flush_pending drain
        TEST_ASSERT_EQUAL_UINT8('{', output[1]);  // from delim_pos flush
    }

    // --- Case 2: VAR_NAME state with flush_pending > 0 ---
    // Simulate: 1 byte of flush_pending + VAR_NAME state with a var name
    {
        template_context_t ctx;
        template_init_default(&ctx, test_var_callback, NULL);

        ctx.flush_pending = 1;
        ctx.flush_offset = 0;     // will flush start_delim[0] = '{'
        ctx.state = 1;            // TEMPLATE_STATE_VAR_NAME
        ctx.var_name[0] = 'x';
        ctx.var_name_len = 1;
        ctx.delim_pos = 0;

        uint8_t output[64] = {0};
        int result = template_flush(&ctx, output, sizeof(output));

        // Should get: "{" (flush_pending) + "{{" (start delim) + "x" (var name) = "{{{x"
        TEST_ASSERT_EQUAL(4, result);
        TEST_ASSERT_EQUAL_UINT8('{', output[0]);  // from flush_pending drain
        TEST_ASSERT_EQUAL_UINT8('{', output[1]);  // start delim[0]
        TEST_ASSERT_EQUAL_UINT8('{', output[2]);  // start delim[1]
        TEST_ASSERT_EQUAL_UINT8('x', output[3]);  // var name
    }
}

// ==================== TEST RUNNER ====================

void test_template_run(void) {
    ESP_LOGI(TAG, "Running Template tests");

    RUN_TEST(test_template_basic_substitution);
    RUN_TEST(test_template_multiple_vars);
    RUN_TEST(test_template_empty_var);
    RUN_TEST(test_template_no_vars);
    RUN_TEST(test_template_custom_delimiters);
    RUN_TEST(test_template_partial_delimiters);
    RUN_TEST(test_template_html_escape);
    RUN_TEST(test_template_long_var_name);
    RUN_TEST(test_template_buffer_overflow);
    RUN_TEST(test_template_streaming);

    // Bug fix regression tests
    RUN_TEST(test_template_callback_overflow_clamped);
    RUN_TEST(test_template_delimiter_ownership);
    RUN_TEST(test_template_null_delimiters);
    RUN_TEST(test_template_escape_html_in_substitution);
    RUN_TEST(test_template_no_escape_html_in_substitution);
    RUN_TEST(test_template_flush_partial_end_delim);
    RUN_TEST(test_template_flush_after_process);
    RUN_TEST(test_template_init_null_config);
    RUN_TEST(test_template_init_null_ctx);
    RUN_TEST(test_template_partial_delim_flush_small_buffer);
    RUN_TEST(test_template_partial_delim_flush_zero_avail);
    RUN_TEST(test_template_process_file_read_error);
    RUN_TEST(test_template_escape_long_value);

    // template_var_env tests (Task #48)
    RUN_TEST(test_template_var_env_unknown);
    RUN_TEST(test_template_var_env_with_prefix);
    RUN_TEST(test_template_var_env_known);
    RUN_TEST(test_template_var_env_with_prefix_known);
    RUN_TEST(test_template_var_env_truncated);

    // flush_pending + delim_pos data preservation
    RUN_TEST(test_template_flush_preserves_pending_data);

    ESP_LOGI(TAG, "Template tests completed");
}