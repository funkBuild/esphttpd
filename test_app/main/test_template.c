#include "unity.h"
#include "template.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "TEST_TEMPLATE";

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

// ========== Bug #5: variable value longer than 128 bytes rendered in FULL
//           under escape_html (previously capped at a 128-byte temp buffer) ==========

// Callback that returns a value longer than the old 128-byte cap, containing
// HTML-sensitive characters so escaping is also exercised.
static int large_html_var_callback(const char* var_name,
                                   uint8_t* output,
                                   size_t output_size,
                                   void* user_data) {
    if (strcmp(var_name, "big") == 0) {
        // 130 'A' (already > 128) followed by "<x>" so the escapable chars
        // sit PAST the old 128-byte truncation point.
        uint8_t val[133];
        for (int i = 0; i < 130; i++) val[i] = 'A';
        val[130] = '<';
        val[131] = 'x';
        val[132] = '>';
        size_t len = sizeof(val);
        if (len > output_size) len = output_size;
        memcpy(output, val, len);
        return (int)len;
    }
    return -1;
}

static void test_template_escape_html_large_value(void) {
    template_context_t ctx;
    template_init_default(&ctx, large_html_var_callback, NULL);  // escape_html = true

    const char* input = "V:{{big}}";
    uint8_t output[512] = {0};

    int result = template_process(&ctx, (const uint8_t*)input,
                                 strlen(input), output, sizeof(output));
    output[result] = '\0';

    // Build the expected fully-rendered, escaped output.
    char expected[600];
    int n = 0;
    expected[n++] = 'V';
    expected[n++] = ':';
    for (int i = 0; i < 130; i++) expected[n++] = 'A';
    memcpy(expected + n, "&lt;x&gt;", 9);
    n += 9;
    expected[n] = '\0';

    // Full value renders (no 128-byte truncation) and the trailing chars past
    // byte 128 are present and correctly escaped.
    TEST_ASSERT_EQUAL_STRING(expected, (char*)output);
    TEST_ASSERT_EQUAL(n, result);
    TEST_ASSERT_NOT_NULL(strstr((char*)output, "&lt;x&gt;"));
    TEST_ASSERT_NULL(strstr((char*)output, "<x>"));

    // Terminal call of the run: releases the reusable escape scratch the
    // large value forced the context to allocate.
    uint8_t tail[8];
    template_flush(&ctx, tail, sizeof(tail));
}

// ========== Escape-scratch reuse: multiple large escaped variables in ONE
//           template must all render in full. Internally the second and third
//           variable reuse the per-context scratch allocated for the first
//           (no per-variable malloc/free); externally we assert full,
//           correctly escaped rendering of all three. ==========

// Returns ~300 raw bytes for "big1"/"big2"/"big3": 300 fill chars (distinct
// per variable) followed by "<x>", so escapable chars sit far past both the
// 128-byte stack fast path and the old truncation point.
static int three_large_var_callback(const char* var_name,
                                    uint8_t* output,
                                    size_t output_size,
                                    void* user_data) {
    char fill;
    if (strcmp(var_name, "big1") == 0)      fill = 'A';
    else if (strcmp(var_name, "big2") == 0) fill = 'B';
    else if (strcmp(var_name, "big3") == 0) fill = 'C';
    else return -1;

    uint8_t val[303];
    memset(val, fill, 300);
    val[300] = '<';
    val[301] = 'x';
    val[302] = '>';
    size_t len = sizeof(val);
    if (len > output_size) len = output_size;
    memcpy(output, val, len);
    return (int)len;
}

static void test_template_escape_scratch_reuse_three_large(void) {
    template_context_t ctx;
    template_init_default(&ctx, three_large_var_callback, NULL); // escape_html = true

    const char* input = "{{big1}}|{{big2}}|{{big3}}";
    // Each variable: 300 fill + "&lt;x&gt;" (9) = 309 escaped bytes.
    // 3*309 + 2 separators = 929 -> fits in 1024.
    static uint8_t output[1024];
    memset(output, 0, sizeof(output));

    int result = template_process(&ctx, (const uint8_t*)input,
                                  strlen(input), output, sizeof(output));
    TEST_ASSERT_TRUE(result > 0);
    output[result] = '\0';

    static char expected[1024];
    int n = 0;
    const char fills[3] = { 'A', 'B', 'C' };
    for (int v = 0; v < 3; v++) {
        if (v > 0) expected[n++] = '|';
        memset(expected + n, fills[v], 300);
        n += 300;
        memcpy(expected + n, "&lt;x&gt;", 9);
        n += 9;
    }
    expected[n] = '\0';

    TEST_ASSERT_EQUAL(n, result);
    TEST_ASSERT_EQUAL_STRING(expected, (char*)output);

    uint8_t tail[8];
    template_flush(&ctx, tail, sizeof(tail));
}

// ========== Context reuse: a second template_init + process + flush cycle on
//           the same context (after the first run's flush freed the scratch)
//           must work correctly - the scratch is re-allocated on demand and
//           freed again. ==========

static void test_template_ctx_reinit_second_cycle(void) {
    template_context_t ctx;

    for (int cycle = 0; cycle < 2; cycle++) {
        template_init_default(&ctx, large_html_var_callback, NULL);

        const char* input = "V:{{big}}";
        uint8_t output[512] = {0};
        int result = template_process(&ctx, (const uint8_t*)input,
                                      strlen(input), output, sizeof(output));
        TEST_ASSERT_TRUE(result > 0);
        output[result] = '\0';

        // Same expected output both cycles: full 130-'A' value with the
        // trailing "<x>" escaped.
        TEST_ASSERT_NOT_NULL(strstr((char*)output, "&lt;x&gt;"));
        TEST_ASSERT_NULL(strstr((char*)output, "<x>"));
        TEST_ASSERT_EQUAL(2 + 130 + 9, result);

        // Flush ends the run and frees the scratch, so the next cycle's
        // template_init starts from a clean (non-leaking) context.
        uint8_t tail[8];
        template_flush(&ctx, tail, sizeof(tail));
    }
}

// ========== Bug #4: expansive input (short placeholders -> large escaped
//           values) round-trips with NO dropped bytes across chunk boundaries.
//           This is the streaming invariant template_process_file now relies on
//           (its consume/refill loop). fd-driven testing is not possible here:
//           ESP-IDF provides no pipe()/tmpfile() and no FS is mounted. ==========

// Each {{esc}} (7 input bytes) expands to "&lt;&amp;&gt;" (13 output bytes).
static int expansive_var_callback(const char* var_name,
                                  uint8_t* output,
                                  size_t output_size,
                                  void* user_data) {
    if (strcmp(var_name, "esc") == 0) {
        const char* value = "<&>";
        size_t len = strlen(value);
        if (len > output_size) len = output_size;
        memcpy(output, value, len);
        return (int)len;
    }
    return -1;
}

static void test_template_expansive_no_dropped_bytes(void) {
    template_context_t ctx;
    template_init_default(&ctx, expansive_var_callback, NULL);  // escape_html = true

    // 20 placeholders: 140 input bytes -> 260 output bytes (1.86x expansion).
    const int REPEATS = 20;
    char input[8 * 32];
    int in_len = 0;
    for (int i = 0; i < REPEATS; i++) {
        memcpy(input + in_len, "{{esc}}", 7);
        in_len += 7;
    }
    input[in_len] = '\0';

    // Drive the engine in small slices (like successive file reads) with an
    // ample accumulating output buffer, so state carries across chunk
    // boundaries and delimiters may be split. No byte may be dropped.
    uint8_t output[1024] = {0};
    int out_pos = 0;
    const int SLICE = 5;  // deliberately not aligned to the 7-byte placeholder
    for (int off = 0; off < in_len; off += SLICE) {
        int chunk = (in_len - off < SLICE) ? (in_len - off) : SLICE;
        out_pos += template_process(&ctx, (const uint8_t*)input + off, chunk,
                                    output + out_pos, sizeof(output) - out_pos);
    }
    out_pos += template_flush(&ctx, output + out_pos, sizeof(output) - out_pos);
    output[out_pos] = '\0';

    // Build expected: REPEATS copies of the escaped value.
    char expected[1024];
    int n = 0;
    for (int i = 0; i < REPEATS; i++) {
        memcpy(expected + n, "&lt;&amp;&gt;", 13);
        n += 13;
    }
    expected[n] = '\0';

    TEST_ASSERT_EQUAL(n, out_pos);
    TEST_ASSERT_EQUAL_STRING(expected, (char*)output);
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
    RUN_TEST(test_template_escape_html_large_value);
    RUN_TEST(test_template_escape_scratch_reuse_three_large);
    RUN_TEST(test_template_ctx_reinit_second_cycle);
    RUN_TEST(test_template_expansive_no_dropped_bytes);
    RUN_TEST(test_template_init_null_config);
    RUN_TEST(test_template_init_null_ctx);

    ESP_LOGI(TAG, "Template tests completed");
}