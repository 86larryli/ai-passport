// main/cc_fonts.c — see cc_fonts.h.
#include "cc_fonts.h"

#include "cc_text.h"
#include "esp_log.h"

static const char *TAG = "cc_fonts";

LV_FONT_DECLARE(cc_font_cjk_16);
LV_FONT_DECLARE(cc_font_ui_12);
LV_FONT_DECLARE(cc_font_ui_20);
LV_FONT_DECLARE(cc_font_num_40);

// Writable descriptor copies with application lifetime, so fallbacks can be set
// without casting away const on the Flash-resident originals.
static lv_font_t s_small;
static lv_font_t s_title;

cc_fonts_t cc_fonts;

void cc_fonts_init(void)
{
    s_small = cc_font_ui_12;
    s_small.fallback = &cc_font_cjk_16;
    s_title = cc_font_ui_20;
    s_title.fallback = &cc_font_cjk_16;
    cc_fonts.body = &cc_font_cjk_16;
    cc_fonts.small = &s_small;
    cc_fonts.title = &s_title;
    cc_fonts.num = &cc_font_num_40;
    cc_fonts.icon = &lv_font_montserrat_14;
}

bool cc_fonts_has_glyph(const lv_font_t *font, uint32_t cp)
{
    if (!font) return false;
    lv_font_glyph_dsc_t glyph = { 0 };
    return lv_font_get_glyph_dsc(font, &glyph, cp, 0) && !glyph.is_placeholder;
}

static bool has_glyph_cb(uint32_t cp, void *ctx)
{
    return cc_fonts_has_glyph((const lv_font_t *)ctx, cp);
}

void cc_fonts_filter(const lv_font_t *font, const char *text, char *out, size_t cap)
{
    cc_text_filter_glyphs(out, cap, text ? text : "", has_glyph_cb, (void *)font);
}

void cc_fonts_set_text(lv_obj_t *label, const char *text)
{
    static char buf[CC_TEXT_FILTER_MAX];   // LVGL task only
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    cc_text_filter_glyphs(buf, sizeof(buf), text ? text : "", has_glyph_cb, (void *)font);
    lv_label_set_text(label, buf);
}

static size_t check(const char *name, const lv_font_t *font, const uint32_t *cps, size_t n)
{
    size_t missing = 0;
    for (size_t i = 0; i < n; i++) {
        if (!cc_fonts_has_glyph(font, cps[i])) {
            ESP_LOGE(TAG, "%s lacks U+%04X", name, (unsigned)cps[i]);
            missing++;
        }
    }
    return missing;
}

size_t cc_fonts_self_test(void)
{
    // Replacement mark, ellipsis, CJK punctuation, sample hanzi, and the
    // non-ASCII symbols the fixed UI strings use.
    static const uint32_t BODY[] = { 0x25A1, 0x2026, 0x3002, 0xFF0C, 0x201C, 0x4E2D, 0x6587,
                                     0x9F99, 0x2713, 0x2192 };
    static const uint32_t SMALL[] = { 0x25B2, 0x25BC, 0x2713, 0x00D7, 0x2022, 0x2026, 0x25CF };
    static const uint32_t NUM[] = { '0', '9', ' ', ':' };
    size_t missing = check("body", cc_fonts.body, BODY, sizeof(BODY) / sizeof(BODY[0]));
    missing += check("small", cc_fonts.small, SMALL, sizeof(SMALL) / sizeof(SMALL[0]));
    missing += check("num", cc_fonts.num, NUM, sizeof(NUM) / sizeof(NUM[0]));
    // U+9F98 (龘) is outside GB2312 and must not resolve, or the check is vacuous.
    if (cc_fonts_has_glyph(cc_fonts.body, 0x9F98)) {
        ESP_LOGE(TAG, "coverage check is vacuous: U+9F98 unexpectedly present");
        missing++;
    }
    ESP_LOGI(TAG, "glyph self-test: %u missing", (unsigned)missing);
    return missing;
}
