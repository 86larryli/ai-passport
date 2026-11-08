// main/cc_text.c — see cc_text.h.
#include "cc_text.h"

#include <stdio.h>
#include <string.h>

size_t cc_utf8_decode(const char *s, size_t n, uint32_t *cp)
{
    if (n == 0) return 0;
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t need;
    uint32_t value;
    uint32_t min;
    if ((c & 0xE0) == 0xC0) {
        need = 2; value = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        need = 3; value = c & 0x0F; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        need = 4; value = c & 0x07; min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (n < need) {
        *cp = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (u[i] & 0x3F);
    }
    // Reject overlong forms, UTF-16 surrogates, and values beyond Unicode.
    if (value < min || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = value;
    return need;
}

size_t cc_utf8_encode(uint32_t cp, char *out)
{
    unsigned char *o = (unsigned char *)out;
    if (cp < 0x80) {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (unsigned char)(0xC0 | (cp >> 6));
        o[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (unsigned char)(0xE0 | (cp >> 12));
        o[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (unsigned char)(0xF0 | (cp >> 18));
    o[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

// Append ellipsis after backing off to keep it inside cap.
static void append_ellipsis(char *dst, size_t cap, size_t len)
{
    // Back off whole characters until "..." plus NUL fits.
    while (len > 0 && len + 4 > cap) {
        len--;
        while (len > 0 && ((unsigned char)dst[len] & 0xC0) == 0x80) len--;
    }
    if (len + 4 <= cap) {
        memcpy(dst + len, "...", 4);
    } else {
        dst[len] = '\0';
    }
}

bool cc_text_copy(char *dst, size_t cap, const char *src, size_t n, bool multiline)
{
    if (cap == 0) return n > 0;
    size_t out = 0;
    size_t i = 0;
    bool truncated = false;
    while (i < n) {
        uint32_t cp;
        size_t used = cc_utf8_decode(src + i, n - i, &cp);
        char enc[4];
        size_t len;
        if (cp == 0xFFFD && used == 1 && (unsigned char)src[i] >= 0x80) {
            enc[0] = '?';
            len = 1;
        } else if (cp == 0x1F) {
            enc[0] = multiline ? '\n' : ' ';
            len = 1;
        } else if (cp < 0x20 || cp == 0x7F) {
            enc[0] = ' ';
            len = 1;
        } else {
            len = cc_utf8_encode(cp, enc);
        }
        if (out + len + 1 > cap) {
            truncated = true;
            break;
        }
        memcpy(dst + out, enc, len);
        out += len;
        i += used;
    }
    dst[out] = '\0';
    if (truncated) append_ellipsis(dst, cap, out);
    return truncated;
}

void cc_text_filter_glyphs(char *dst, size_t cap, const char *src,
                           cc_has_glyph_fn has_glyph, void *ctx)
{
    if (cap == 0) return;
    const bool replacement_ok = has_glyph(CC_REPLACEMENT_CP, ctx);
    size_t n = strlen(src);
    size_t out = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        size_t used = cc_utf8_decode(src + i, n - i, &cp);
        char enc[4];
        size_t len;
        if (cp == '\n' || cp == ' ' || has_glyph(cp, ctx)) {
            len = cc_utf8_encode(cp, enc);
        } else if (replacement_ok) {
            len = cc_utf8_encode(CC_REPLACEMENT_CP, enc);
        } else {
            enc[0] = '?';
            len = 1;
        }
        if (out + len + 1 > cap) break;
        memcpy(dst + out, enc, len);
        out += len;
        i += used;
    }
    dst[out] = '\0';
}

void cc_format_age(char *out, size_t cap, uint32_t s)
{
    if (s < 5) {
        snprintf(out, cap, "now");
    } else if (s < 60) {
        snprintf(out, cap, "%us", (unsigned)s);
    } else if (s < 600) {
        snprintf(out, cap, "%um%02us", (unsigned)(s / 60), (unsigned)(s % 60));
    } else if (s < 3600) {
        snprintf(out, cap, "%um", (unsigned)(s / 60));
    } else if (s < 86400) {
        snprintf(out, cap, "%uh%02um", (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
    } else {
        snprintf(out, cap, "%ud", (unsigned)(s / 86400));
    }
}

void cc_format_clock(char *out, size_t cap, int64_t unix_s, int32_t utc_offset_min)
{
    int64_t local = unix_s + (int64_t)utc_offset_min * 60;
    int64_t day_s = local % 86400;
    if (day_s < 0) day_s += 86400;
    snprintf(out, cap, "%02d:%02d", (int)(day_s / 3600), (int)((day_s % 3600) / 60));
}

size_t cc_utf8_length(const char *s)
{
    size_t n = strlen(s);
    size_t count = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp;
        i += cc_utf8_decode(s + i, n - i, &cp);
        count++;
    }
    return count;
}
