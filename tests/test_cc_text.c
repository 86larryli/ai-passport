// Host tests for main/cc_text.c.
#include "cc_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)
#define CHECK_STR(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
        failures++; } \
} while (0)

static bool valid_utf8(const char *s)
{
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t used = cc_utf8_decode(s + i, n - i, &cp);
        if (cp == 0xFFFD) return false;
        i += used;
    }
    return true;
}

static void test_decode(void)
{
    uint32_t cp;
    CHECK(cc_utf8_decode("A", 1, &cp) == 1 && cp == 'A');
    CHECK(cc_utf8_decode("\xE4\xB8\xAD", 3, &cp) == 3 && cp == 0x4E2D);         // 中
    CHECK(cc_utf8_decode("\xF0\x9F\x98\x80", 4, &cp) == 4 && cp == 0x1F600);    // emoji
    CHECK(cc_utf8_decode("\xE4\xB8", 2, &cp) == 1 && cp == 0xFFFD);             // truncated
    CHECK(cc_utf8_decode("\xC0\xAF", 2, &cp) == 1 && cp == 0xFFFD);             // overlong
    CHECK(cc_utf8_decode("\xED\xA0\x80", 3, &cp) == 1 && cp == 0xFFFD);         // surrogate
    CHECK(cc_utf8_decode("\xFF", 1, &cp) == 1 && cp == 0xFFFD);
    CHECK(cc_utf8_decode("", 0, &cp) == 0);
    char buf[4];
    CHECK(cc_utf8_encode(0x25A1, buf) == 3 && memcmp(buf, "\xE2\x96\xA1", 3) == 0);
}

static void test_copy(void)
{
    char out[16];
    CHECK(!cc_text_copy(out, sizeof(out), "hello", 5, false));
    CHECK_STR(out, "hello");

    // Controls become spaces; 0x1F becomes a newline only in multiline fields.
    CHECK(!cc_text_copy(out, sizeof(out), "a\x01" "b\x1f" "c", 5, false));
    CHECK_STR(out, "a b c");
    CHECK(!cc_text_copy(out, sizeof(out), "a\x1f" "b", 3, true));
    CHECK_STR(out, "a\nb");

    // Invalid bytes become '?'.
    CHECK(!cc_text_copy(out, sizeof(out), "x\xFFy", 3, false));
    CHECK_STR(out, "x?y");

    // Truncation never splits a character and appends "...".
    char small[10];
    const char *cjk = "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95";   // 中文测试 (12 bytes)
    CHECK(cc_text_copy(small, sizeof(small), cjk, strlen(cjk), false));
    CHECK(valid_utf8(small));
    CHECK(strlen(small) < sizeof(small));
    CHECK_STR(small, "\xE4\xB8\xAD\xE6\x96\x87...");

    char tiny[4];
    CHECK(cc_text_copy(tiny, sizeof(tiny), "abcdef", 6, false));
    CHECK(strlen(tiny) < sizeof(tiny));
    CHECK(valid_utf8(tiny));

    // Exactly fitting text is not truncated.
    char exact[6];
    CHECK(!cc_text_copy(exact, sizeof(exact), "abcde", 5, false));
    CHECK_STR(exact, "abcde");
}

static bool has_glyph_ascii_cjk(uint32_t cp, void *ctx)
{
    (void)ctx;
    return (cp >= 0x20 && cp <= 0x7E) || cp == 0x4E2D || cp == 0x25A1;
}

static bool has_glyph_ascii_only(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp >= 0x20 && cp <= 0x7E;
}

static void test_filter(void)
{
    char out[32];
    // 中 is covered, 文 is not, emoji is not.
    cc_text_filter_glyphs(out, sizeof(out), "\xE4\xB8\xAD\xE6\x96\x87 ok \xF0\x9F\x98\x80\nx",
                          has_glyph_ascii_cjk, NULL);
    CHECK_STR(out, "\xE4\xB8\xAD\xE2\x96\xA1 ok \xE2\x96\xA1\nx");
    // Without the replacement glyph, fall back to '?'.
    cc_text_filter_glyphs(out, sizeof(out), "\xE6\x96\x87!", has_glyph_ascii_only, NULL);
    CHECK_STR(out, "?!");
    // Output is bounded by cap at a character boundary.
    char tiny[5];
    cc_text_filter_glyphs(tiny, sizeof(tiny), "\xE4\xB8\xAD\xE4\xB8\xAD", has_glyph_ascii_cjk, NULL);
    CHECK_STR(tiny, "\xE4\xB8\xAD");
}

static void test_age(void)
{
    char out[16];
    cc_format_age(out, sizeof(out), 0);
    CHECK_STR(out, "now");
    cc_format_age(out, sizeof(out), 42);
    CHECK_STR(out, "42s");
    cc_format_age(out, sizeof(out), 192);
    CHECK_STR(out, "3m12s");
    cc_format_age(out, sizeof(out), 2520);
    CHECK_STR(out, "42m");
    cc_format_age(out, sizeof(out), 3 * 3600 + 5 * 60 + 9);
    CHECK_STR(out, "3h05m");
    cc_format_age(out, sizeof(out), 2 * 86400 + 5);
    CHECK_STR(out, "2d");
}

static void test_clock(void)
{
    char out[8];
    // 2026-11-07 23:48:00 UTC
    cc_format_clock(out, sizeof(out), 1794095280LL, 0);
    CHECK_STR(out, "23:48");
    cc_format_clock(out, sizeof(out), 1794095280LL, -8 * 60);
    CHECK_STR(out, "15:48");
    cc_format_clock(out, sizeof(out), 1794095280LL, 8 * 60);
    CHECK_STR(out, "07:48");
    cc_format_clock(out, sizeof(out), 0, -60);
    CHECK_STR(out, "23:00");
}

static void test_length(void)
{
    CHECK(cc_utf8_length("") == 0);
    CHECK(cc_utf8_length("ab\xE4\xB8\xAD") == 3);
}

int main(void)
{
    test_decode();
    test_copy();
    test_filter();
    test_age();
    test_clock();
    test_length();
    if (failures) {
        fprintf(stderr, "test_cc_text: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_cc_text: PASS\n");
    return EXIT_SUCCESS;
}
