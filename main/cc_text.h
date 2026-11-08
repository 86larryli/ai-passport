// main/cc_text.h — UTF-8 and formatting helpers (no ESP-IDF/LVGL dependency).
//
// Everything here is pure and covered by tests/test_cc_text.c. Text that reaches
// LVGL labels passes through these helpers so it is always valid UTF-8, never
// cut in the middle of a character, and free of control characters.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CC_REPLACEMENT_CP 0x25A1u   // U+25A1 WHITE SQUARE, our deliberate "no glyph" mark
#define CC_TEXT_FILTER_MAX 512      // longest label text after glyph filtering

// Decode one code point from s[0..n). Returns the bytes consumed (1..4), or 0
// when n == 0. Malformed or overlong sequences consume one byte and yield
// U+FFFD so callers always make progress.
size_t cc_utf8_decode(const char *s, size_t n, uint32_t *cp);

// Encode cp into out (at least 4 bytes). Returns the byte count (1..4).
size_t cc_utf8_encode(uint32_t cp, char *out);

// Copy src[0..n) into dst (capacity cap, cap >= 4) as clean UTF-8:
//   - invalid sequences become '?',
//   - 0x1F (the protocol's in-field line break) becomes '\n' when multiline,
//     otherwise a space; other C0 controls and DEL become spaces,
//   - output is cut at a character boundary; when it does not fit, "..." is
//     appended within cap.
// Returns true when the text was truncated.
bool cc_text_copy(char *dst, size_t cap, const char *src, size_t n, bool multiline);

// Replace every code point for which has_glyph() is false with
// CC_REPLACEMENT_CP (or '?' when that is also missing). Newlines and spaces are
// kept. Output is truncated at a character boundary to fit cap.
typedef bool (*cc_has_glyph_fn)(uint32_t cp, void *ctx);
void cc_text_filter_glyphs(char *dst, size_t cap, const char *src,
                           cc_has_glyph_fn has_glyph, void *ctx);

// Compact elapsed time: "now", "42s", "3m12s", "42m", "3h05m", "2d".
void cc_format_age(char *out, size_t cap, uint32_t seconds);

// Local wall-clock "HH:MM" from a Unix time and a UTC offset in minutes.
void cc_format_clock(char *out, size_t cap, int64_t unix_s, int32_t utc_offset_min);

// Count code points in a NUL-terminated UTF-8 string.
size_t cc_utf8_length(const char *s);
