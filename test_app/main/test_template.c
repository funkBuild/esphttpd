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

    // Create a variable name longer than 64 chars
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

    ESP_LOGI(TAG, "Template tests completed");
}