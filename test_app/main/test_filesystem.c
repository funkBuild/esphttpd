/**
 * @file test_filesystem.c
 * @brief Unit tests for filesystem path validation and security
 */

#include "unity.h"
#include "filesystem.h"
#include <string.h>

// ============================================================================
// Path Validation Security Tests
// ============================================================================

// --- Valid Path Tests ---

static void test_validate_path_simple(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/index.html"));
}

static void test_validate_path_nested(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/css/style.css"));
}

static void test_validate_path_deep_nesting(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/assets/js/vendor/jquery.min.js"));
}

static void test_validate_path_with_dots_in_filename(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/file.min.js"));
}

static void test_validate_path_hidden_file(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/.htaccess"));
}

static void test_validate_path_numeric(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/123/456.txt"));
}

static void test_validate_path_hyphen_underscore(void) {
    TEST_ASSERT_TRUE(filesystem_validate_path("/my-file_name.txt"));
}

// --- Null and Empty Tests ---

static void test_validate_path_null(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path(NULL));
}

static void test_validate_path_empty(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path(""));
}

// --- Directory Traversal Tests ---

static void test_validate_path_dot_dot(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("../etc/passwd"));
}

static void test_validate_path_dot_dot_in_middle(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo/../bar"));
}

static void test_validate_path_dot_dot_at_end(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo/bar/.."));
}

static void test_validate_path_multiple_dot_dot(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo/../../bar"));
}

static void test_validate_path_dot_dot_with_slashes(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo/../../../etc/passwd"));
}

// --- URL-encoded Dot Tests ---

static void test_validate_path_encoded_dot_lowercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/%2e%2e/etc/passwd"));
}

static void test_validate_path_encoded_dot_uppercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/%2E%2E/etc/passwd"));
}

static void test_validate_path_encoded_dot_mixed(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/%2e%2E/etc/passwd"));
}

static void test_validate_path_single_encoded_dot(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/%2e./etc/passwd"));
}

static void test_validate_path_dot_and_encoded_dot(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/.%2e/etc/passwd"));
}

// --- URL-encoded Slash Tests ---

static void test_validate_path_encoded_slash_lowercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo%2fetc/passwd"));
}

static void test_validate_path_encoded_slash_uppercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo%2Fetc/passwd"));
}

// --- URL-encoded Backslash Tests ---

static void test_validate_path_encoded_backslash_lowercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo%5cbar"));
}

static void test_validate_path_encoded_backslash_uppercase(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo%5Cbar"));
}

// --- URL-encoded Null Byte Tests ---

static void test_validate_path_encoded_null(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo%00bar"));
}

static void test_validate_path_encoded_null_at_end(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo.txt%00.jpg"));
}

// --- Double Slash Tests ---

static void test_validate_path_double_slash_start(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("//etc/passwd"));
}

static void test_validate_path_double_slash_middle(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo//bar"));
}

// --- Backslash Tests ---

static void test_validate_path_backslash(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("/foo\\bar"));
}

static void test_validate_path_backslash_traversal(void) {
    TEST_ASSERT_FALSE(filesystem_validate_path("\\..\\etc\\passwd"));
}

// --- Edge Cases ---

static void test_validate_path_percent_sign_valid(void) {
    // %20 is a space, should be allowed
    TEST_ASSERT_TRUE(filesystem_validate_path("/file%20name.txt"));
}

static void test_validate_path_incomplete_percent(void) {
    // Incomplete percent encoding at end - should be allowed (not dangerous)
    TEST_ASSERT_TRUE(filesystem_validate_path("/file%2"));
}

static void test_validate_path_percent_at_end(void) {
    // Single percent at end - should be allowed (not dangerous)
    TEST_ASSERT_TRUE(filesystem_validate_path("/file%"));
}

// ============================================================================
// MIME Type Tests
// ============================================================================

static void test_mime_type_html(void) {
    TEST_ASSERT_EQUAL_STRING("text/html", filesystem_get_mime_type("/index.html"));
}

static void test_mime_type_htm(void) {
    TEST_ASSERT_EQUAL_STRING("text/html", filesystem_get_mime_type("/page.htm"));
}

static void test_mime_type_css(void) {
    TEST_ASSERT_EQUAL_STRING("text/css", filesystem_get_mime_type("/style.css"));
}

static void test_mime_type_js(void) {
    TEST_ASSERT_EQUAL_STRING("application/javascript", filesystem_get_mime_type("/app.js"));
}

static void test_mime_type_json(void) {
    TEST_ASSERT_EQUAL_STRING("application/json", filesystem_get_mime_type("/data.json"));
}

static void test_mime_type_png(void) {
    TEST_ASSERT_EQUAL_STRING("image/png", filesystem_get_mime_type("/image.png"));
}

static void test_mime_type_jpg(void) {
    TEST_ASSERT_EQUAL_STRING("image/jpeg", filesystem_get_mime_type("/photo.jpg"));
}

static void test_mime_type_jpeg(void) {
    TEST_ASSERT_EQUAL_STRING("image/jpeg", filesystem_get_mime_type("/photo.jpeg"));
}

static void test_mime_type_svg(void) {
    TEST_ASSERT_EQUAL_STRING("image/svg+xml", filesystem_get_mime_type("/icon.svg"));
}

static void test_mime_type_ico(void) {
    TEST_ASSERT_EQUAL_STRING("image/x-icon", filesystem_get_mime_type("/favicon.ico"));
}

static void test_mime_type_woff2(void) {
    TEST_ASSERT_EQUAL_STRING("font/woff2", filesystem_get_mime_type("/font.woff2"));
}

static void test_mime_type_unknown(void) {
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", filesystem_get_mime_type("/file.xyz"));
}

static void test_mime_type_no_extension(void) {
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", filesystem_get_mime_type("/file"));
}

static void test_mime_type_case_insensitive_html(void) {
    TEST_ASSERT_EQUAL_STRING("text/html", filesystem_get_mime_type("/INDEX.HTML"));
}

static void test_mime_type_case_insensitive_js(void) {
    TEST_ASSERT_EQUAL_STRING("application/javascript", filesystem_get_mime_type("/APP.JS"));
}

static void test_mime_type_nested_path(void) {
    TEST_ASSERT_EQUAL_STRING("text/css", filesystem_get_mime_type("/assets/css/style.css"));
}

// --- Additional MIME Type Coverage ---

static void test_mime_type_xml(void) {
    TEST_ASSERT_EQUAL_STRING("application/xml", filesystem_get_mime_type("/config.xml"));
}

static void test_mime_type_gif(void) {
    TEST_ASSERT_EQUAL_STRING("image/gif", filesystem_get_mime_type("/animation.gif"));
}

static void test_mime_type_webp(void) {
    TEST_ASSERT_EQUAL_STRING("image/webp", filesystem_get_mime_type("/photo.webp"));
}

static void test_mime_type_txt(void) {
    TEST_ASSERT_EQUAL_STRING("text/plain", filesystem_get_mime_type("/readme.txt"));
}

static void test_mime_type_woff(void) {
    TEST_ASSERT_EQUAL_STRING("font/woff", filesystem_get_mime_type("/font.woff"));
}

static void test_mime_type_ttf(void) {
    TEST_ASSERT_EQUAL_STRING("font/ttf", filesystem_get_mime_type("/font.ttf"));
}

static void test_mime_type_otf(void) {
    TEST_ASSERT_EQUAL_STRING("font/otf", filesystem_get_mime_type("/font.otf"));
}

static void test_mime_type_pdf(void) {
    TEST_ASSERT_EQUAL_STRING("application/pdf", filesystem_get_mime_type("/document.pdf"));
}

static void test_mime_type_zip(void) {
    TEST_ASSERT_EQUAL_STRING("application/zip", filesystem_get_mime_type("/archive.zip"));
}

static void test_mime_type_gz(void) {
    TEST_ASSERT_EQUAL_STRING("application/gzip", filesystem_get_mime_type("/file.gz"));
}

static void test_mime_type_double_extension(void) {
    // Should use the last extension
    TEST_ASSERT_EQUAL_STRING("application/gzip", filesystem_get_mime_type("/file.tar.gz"));
}

// ============================================================================
// Test Runner
// ============================================================================

void test_filesystem_run(void) {
    // Valid path tests
    RUN_TEST(test_validate_path_simple);
    RUN_TEST(test_validate_path_nested);
    RUN_TEST(test_validate_path_deep_nesting);
    RUN_TEST(test_validate_path_with_dots_in_filename);
    RUN_TEST(test_validate_path_hidden_file);
    RUN_TEST(test_validate_path_numeric);
    RUN_TEST(test_validate_path_hyphen_underscore);

    // Null and empty tests
    RUN_TEST(test_validate_path_null);
    RUN_TEST(test_validate_path_empty);

    // Directory traversal tests
    RUN_TEST(test_validate_path_dot_dot);
    RUN_TEST(test_validate_path_dot_dot_in_middle);
    RUN_TEST(test_validate_path_dot_dot_at_end);
    RUN_TEST(test_validate_path_multiple_dot_dot);
    RUN_TEST(test_validate_path_dot_dot_with_slashes);

    // URL-encoded dot tests
    RUN_TEST(test_validate_path_encoded_dot_lowercase);
    RUN_TEST(test_validate_path_encoded_dot_uppercase);
    RUN_TEST(test_validate_path_encoded_dot_mixed);
    RUN_TEST(test_validate_path_single_encoded_dot);
    RUN_TEST(test_validate_path_dot_and_encoded_dot);

    // URL-encoded slash tests
    RUN_TEST(test_validate_path_encoded_slash_lowercase);
    RUN_TEST(test_validate_path_encoded_slash_uppercase);

    // URL-encoded backslash tests
    RUN_TEST(test_validate_path_encoded_backslash_lowercase);
    RUN_TEST(test_validate_path_encoded_backslash_uppercase);

    // URL-encoded null byte tests
    RUN_TEST(test_validate_path_encoded_null);
    RUN_TEST(test_validate_path_encoded_null_at_end);

    // Double slash tests
    RUN_TEST(test_validate_path_double_slash_start);
    RUN_TEST(test_validate_path_double_slash_middle);

    // Backslash tests
    RUN_TEST(test_validate_path_backslash);
    RUN_TEST(test_validate_path_backslash_traversal);

    // Edge cases
    RUN_TEST(test_validate_path_percent_sign_valid);
    RUN_TEST(test_validate_path_incomplete_percent);
    RUN_TEST(test_validate_path_percent_at_end);

    // MIME type tests
    RUN_TEST(test_mime_type_html);
    RUN_TEST(test_mime_type_htm);
    RUN_TEST(test_mime_type_css);
    RUN_TEST(test_mime_type_js);
    RUN_TEST(test_mime_type_json);
    RUN_TEST(test_mime_type_png);
    RUN_TEST(test_mime_type_jpg);
    RUN_TEST(test_mime_type_jpeg);
    RUN_TEST(test_mime_type_svg);
    RUN_TEST(test_mime_type_ico);
    RUN_TEST(test_mime_type_woff2);
    RUN_TEST(test_mime_type_unknown);
    RUN_TEST(test_mime_type_no_extension);
    RUN_TEST(test_mime_type_case_insensitive_html);
    RUN_TEST(test_mime_type_case_insensitive_js);
    RUN_TEST(test_mime_type_nested_path);

    // Additional MIME type coverage
    RUN_TEST(test_mime_type_xml);
    RUN_TEST(test_mime_type_gif);
    RUN_TEST(test_mime_type_webp);
    RUN_TEST(test_mime_type_txt);
    RUN_TEST(test_mime_type_woff);
    RUN_TEST(test_mime_type_ttf);
    RUN_TEST(test_mime_type_otf);
    RUN_TEST(test_mime_type_pdf);
    RUN_TEST(test_mime_type_zip);
    RUN_TEST(test_mime_type_gz);
    RUN_TEST(test_mime_type_double_extension);
}
