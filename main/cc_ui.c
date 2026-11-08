// main/cc_ui.c — see cc_ui.h. Visual language: a warm near-black canvas, one
// rounded card per focus, Claude's coral for work in progress, amber for
// "needs you", green for done, red for errors, and a small animated spark in
// place of Claude Code's terminal spinner.
#include "cc_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "bsp_pins.h"
#include "cc_fonts.h"
#include "cc_sound.h"
#include "cc_store.h"
#include "cc_text.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "cc_ui";

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
#define C_BG 0x0F0F0E
#define C_SURFACE 0x1C1B19
#define C_SURFACE2 0x2A2926
#define C_TEXT 0xF2F0E9
#define C_DIM 0x9B988F
#define C_FAINT 0x66635D
#define C_CORAL 0xD97757
#define C_AMBER 0xF2B14C
#define C_GREEN 0x6FCF97
#define C_RED 0xEB5757
#define C_IDLE 0x8C8A84
#define C_BLUE 0x6CA0F6
#define C_TEAL 0x4FB8C2      // background work after the main turn
#define C_ALERT_BG 0x241C10

#define TICK_MS 100
#define ROWS 3
#define DETAIL_STEP 48
#define PAIR_WINDOW_S 60

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static cc_settings_t *s_settings;
static const char *s_fw;
static bool s_sound_ok;
static cc_view_t s_view;
static cc_link_info_t s_link;
static cc_link_ui_t s_link_ui = CC_LINK_STARTING;
static uint32_t s_seen_version = UINT32_MAX;
static uint32_t s_seen_link_version = UINT32_MAX;
static uint32_t s_last_second;
static uint8_t s_applied_backlight = 255;
static uint32_t s_pair_until_ms;

typedef struct {
    lv_obj_t *cont;
    lv_obj_t *line[3];
    lv_point_precise_t pts[3][2];
    int32_t r;
    uint8_t phase;
} spark_t;

// Top bar
static lv_obj_t *s_ble_icon, *s_clock, *s_batt_icon, *s_batt_pct;
// Home
static lv_obj_t *s_home, *s_home_header, *s_home_count, *s_home_hint;
static lv_obj_t *s_card, *s_card_dot, *s_card_state, *s_card_age, *s_card_project, *s_card_activity;
static lv_obj_t *s_card_agent_dots[CC_MAX_AGENTS], *s_card_agents;
static spark_t s_card_spark;
static lv_obj_t *s_row[ROWS], *s_row_dot[ROWS], *s_row_name[ROWS], *s_row_state[ROWS];
static lv_obj_t *s_empty;
// Details
static lv_obj_t *s_detail, *s_d_scroll, *s_d_project, *s_d_dot, *s_d_state, *s_d_age;
static lv_obj_t *s_d_qbox, *s_d_qtitle, *s_d_qbody, *s_d_now, *s_d_activity, *s_d_note_head, *s_d_note;
static lv_obj_t *s_d_agents_head, *s_d_agent[CC_MAX_AGENTS], *s_d_agent_dot[CC_MAX_AGENTS];
static lv_obj_t *s_d_agent_type[CC_MAX_AGENTS], *s_d_agent_age[CC_MAX_AGENTS], *s_d_agent_act[CC_MAX_AGENTS];
// Settings
static lv_obj_t *s_set_page, *s_set_row[CC_SET_COUNT], *s_set_label[CC_SET_COUNT], *s_set_value[CC_SET_COUNT];
static lv_obj_t *s_set_info;
// Waiting and pairing
static lv_obj_t *s_wait, *s_wait_title, *s_wait_body, *s_wait_name;
static spark_t s_wait_spark;
static lv_obj_t *s_pair, *s_pair_code, *s_pair_name;
// Overlays
static lv_obj_t *s_alert, *s_alert_badge, *s_alert_mark, *s_alert_kind, *s_alert_project;
static lv_obj_t *s_alert_qtitle, *s_alert_body;
static lv_obj_t *s_toast, *s_toast_dot, *s_toast_head, *s_toast_project, *s_toast_text;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// Widget helpers
// ---------------------------------------------------------------------------
static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg,
                     int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *layer(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, BSP_LCD_W, BSP_LCD_H);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t x, int32_t y,
                      int32_t w, lv_label_long_mode_t mode)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    if (w > 0) lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, mode);
    // A fixed-width DOTS/CLIP label with automatic height still wraps, so pin it
    // to one line; callers that want more lines set a larger height afterwards.
    if (w > 0 && (mode == LV_LABEL_LONG_MODE_DOTS || mode == LV_LABEL_LONG_MODE_CLIP)) {
        lv_obj_set_height(l, lv_font_get_line_height(font));
    }
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t d, uint32_t color)
{
    lv_obj_t *o = box(parent, x, y, d, d, color, LV_RADIUS_CIRCLE);
    return o;
}

static void show(lv_obj_t *o, bool visible)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == visible) {
        if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// Set text only when it changes, so unchanged labels are not invalidated. Text
// that is not pure ASCII is first filtered against the label's font chain.
static void set_text(lv_obj_t *label, const char *t)
{
    static char filtered[CC_TEXT_FILTER_MAX];   // LVGL task only
    const char *value = t;
    for (const char *p = t; *p; p++) {
        if ((unsigned char)*p >= 0x80) {
            cc_fonts_filter(lv_obj_get_style_text_font(label, LV_PART_MAIN), t, filtered,
                            sizeof(filtered));
            value = filtered;
            break;
        }
    }
    if (strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static void set_color(lv_obj_t *o, uint32_t color, bool text_color)
{
    lv_color_t c = lv_color_hex(color);
    if (text_color) {
        if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) {
            lv_obj_set_style_text_color(o, c, 0);
        }
    } else if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(o, c, 0);
    }
}

// ---------------------------------------------------------------------------
// Spark: three rotating strokes, a nod to Claude Code's "thinking" glyph.
// ---------------------------------------------------------------------------
// cos(k * 15 deg) * 64; sin is the same table shifted by 18 steps (270 deg).
static const int8_t COS24[24] = { 64, 62, 55, 45, 32, 17, 0, -17, -32, -45, -55, -62,
                                  -64, -62, -55, -45, -32, -17, 0, 17, 32, 45, 55, 62 };

static void spark_update(spark_t *s)
{
    for (int i = 0; i < 3; i++) {
        int a = (s->phase + i * 4) % 24;               // three strokes, 60 degrees apart
        int b = (a + 18) % 24;
        // Pulse the length slightly so the spark breathes while it turns.
        int32_t r = s->r - ((s->phase + i) & 3) / 2;
        int32_t dx = COS24[a] * r / 64;
        int32_t dy = COS24[b] * r / 64;
        s->pts[i][0].x = s->r - dx;
        s->pts[i][0].y = s->r - dy;
        s->pts[i][1].x = s->r + dx;
        s->pts[i][1].y = s->r + dy;
        lv_line_set_points(s->line[i], s->pts[i], 2);
    }
}

static void spark_create(spark_t *s, lv_obj_t *parent, int32_t x, int32_t y, int32_t size,
                         int32_t width, uint32_t color)
{
    s->r = size / 2;
    s->cont = lv_obj_create(parent);
    lv_obj_remove_style_all(s->cont);
    lv_obj_set_pos(s->cont, x, y);
    lv_obj_set_size(s->cont, size + 1, size + 1);
    lv_obj_remove_flag(s->cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        s->line[i] = lv_line_create(s->cont);
        lv_obj_set_style_line_width(s->line[i], width, 0);
        lv_obj_set_style_line_color(s->line[i], lv_color_hex(color), 0);
        lv_obj_set_style_line_rounded(s->line[i], true, 0);
    }
    spark_update(s);
}

static void spark_step(spark_t *s)
{
    if (!s->cont || lv_obj_has_flag(s->cont, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_t *parent = lv_obj_get_parent(s->cont);
    while (parent) {
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return;
        parent = lv_obj_get_parent(parent);
    }
    s->phase = (uint8_t)((s->phase + 1) % 24);
    spark_update(s);
}

static void spark_color(spark_t *s, uint32_t color)
{
    for (int i = 0; i < 3; i++) {
        lv_color_t c = lv_color_hex(color);
        if (!lv_color_eq(lv_obj_get_style_line_color(s->line[i], LV_PART_MAIN), c)) {
            lv_obj_set_style_line_color(s->line[i], c, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------
static uint32_t state_color(cc_session_state_t s)
{
    switch (s) {
    case CC_SESSION_WORK: return C_CORAL;
    case CC_SESSION_BG: return C_TEAL;
    case CC_SESSION_PERM:
    case CC_SESSION_ASK: return C_AMBER;
    case CC_SESSION_DONE: return C_GREEN;
    case CC_SESSION_ERR: return C_RED;
    case CC_SESSION_IDLE:
    default: return C_IDLE;
    }
}

static const char *state_label(cc_session_state_t s)
{
    switch (s) {
    case CC_SESSION_WORK: return "WORKING";
    case CC_SESSION_BG: return "BACKGROUND";
    case CC_SESSION_PERM: return "NEEDS APPROVAL";
    case CC_SESSION_ASK: return "QUESTION";
    case CC_SESSION_DONE: return "DONE";
    case CC_SESSION_ERR: return "ERROR";
    case CC_SESSION_IDLE:
    default: return "IDLE";
    }
}

static const char *state_short(cc_session_state_t s)
{
    switch (s) {
    case CC_SESSION_WORK: return "working";
    case CC_SESSION_BG: return "running";
    case CC_SESSION_PERM: return "approve";
    case CC_SESSION_ASK: return "answer";
    case CC_SESSION_DONE: return "done";
    case CC_SESSION_ERR: return "error";
    case CC_SESSION_IDLE:
    default: return "idle";
    }
}

static uint32_t agent_color(cc_agent_state_t s)
{
    return s == CC_AGENT_RUN ? C_CORAL : (s == CC_AGENT_DONE ? C_GREEN : C_RED);
}

static uint32_t age_now(uint32_t age_s, uint32_t now)
{
    return age_s + (now - cc_store_commit_ms()) / 1000u;
}

// First line of a multi-line text into out.
static void first_line(char *out, size_t cap, const char *in)
{
    size_t n = strcspn(in, "\n");
    cc_text_copy(out, cap, in, n, false);
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
static void build_top_bar(lv_obj_t *scr)
{
    s_ble_icon = text(scr, cc_fonts.icon, C_FAINT, 26, 10, 0, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(s_ble_icon, LV_SYMBOL_BLUETOOTH);
    s_clock = text(scr, cc_fonts.small, C_DIM, 70, 11, 100, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    s_batt_pct = text(scr, cc_fonts.small, C_DIM, 150, 11, 44, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_batt_pct, LV_TEXT_ALIGN_RIGHT, 0);
    s_batt_icon = text(scr, cc_fonts.icon, C_DIM, 196, 9, 0, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_EMPTY);
}

static void build_home(lv_obj_t *scr)
{
    s_home = layer(scr);
    s_home_header = text(s_home, cc_fonts.small, C_DIM, 20, 38, 140, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_home_header, 2, 0);
    s_home_count = text(s_home, cc_fonts.small, C_FAINT, 160, 38, 60, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_home_count, LV_TEXT_ALIGN_RIGHT, 0);

    s_card = box(s_home, 12, 58, 216, 148, C_SURFACE, 16);
    lv_obj_set_style_border_side(s_card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_card, 4, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(C_CORAL), 0);
    s_card_dot = dot(s_card, 18, 15, 10, C_CORAL);
    spark_create(&s_card_spark, s_card, 16, 12, 14, 2, C_CORAL);
    // Wide enough for "NEEDS APPROVAL" with letter spacing; the age is right-aligned
    // and short, so the two boxes overlap without the texts touching.
    s_card_state = text(s_card, cc_fonts.small, C_CORAL, 38, 12, 126, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_card_state, 1, 0);
    s_card_age = text(s_card, cc_fonts.small, C_DIM, 150, 12, 54, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_card_age, LV_TEXT_ALIGN_RIGHT, 0);
    s_card_project = text(s_card, cc_fonts.title, C_TEXT, 16, 32, 190, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_card_project, 26);
    s_card_activity = text(s_card, cc_fonts.body, C_DIM, 16, 62, 190, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_card_activity, 60);
    for (int i = 0; i < CC_MAX_AGENTS; i++) {
        s_card_agent_dots[i] = dot(s_card, 16 + i * 12, 130, 8, C_FAINT);
    }
    s_card_agents = text(s_card, cc_fonts.small, C_FAINT, 16, 126, 190, LV_LABEL_LONG_MODE_DOTS);

    for (int i = 0; i < ROWS; i++) {
        int32_t y = 214 + i * 26;
        s_row[i] = layer(s_home);
        lv_obj_set_pos(s_row[i], 12, y);
        lv_obj_set_size(s_row[i], 216, 24);
        s_row_dot[i] = dot(s_row[i], 8, 8, 8, C_IDLE);
        s_row_name[i] = text(s_row[i], cc_fonts.body, C_TEXT, 24, 1, 128, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_row_name[i], 20);
        s_row_state[i] = text(s_row[i], cc_fonts.small, C_DIM, 152, 4, 58, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_row_state[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    s_empty = layer(s_home);
    lv_obj_t *title = text(s_empty, cc_fonts.title, C_TEXT, 0, 150, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "No active sessions");
    lv_obj_t *body = text(s_empty, cc_fonts.small, C_DIM, 20, 182, 200, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(body, "Start claude in any terminal.\nSessions appear here.");

    s_home_hint = text(s_home, cc_fonts.small, C_FAINT, 0, 296, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_home_hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_detail(lv_obj_t *scr)
{
    s_detail = layer(scr);
    s_d_scroll = lv_obj_create(s_detail);
    lv_obj_remove_style_all(s_d_scroll);
    lv_obj_set_pos(s_d_scroll, 0, 34);
    lv_obj_set_size(s_d_scroll, 240, 256);
    lv_obj_set_style_pad_hor(s_d_scroll, 16, 0);
    lv_obj_set_style_pad_ver(s_d_scroll, 4, 0);
    lv_obj_set_style_pad_row(s_d_scroll, 6, 0);
    lv_obj_set_flex_flow(s_d_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_d_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_d_scroll, LV_OBJ_FLAG_CLICKABLE);

    s_d_project = text(s_d_scroll, cc_fonts.title, C_TEXT, 0, 0, 208, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *state_row = layer(s_d_scroll);
    lv_obj_set_size(state_row, 208, 16);
    s_d_dot = dot(state_row, 0, 4, 8, C_CORAL);
    s_d_state = text(state_row, cc_fonts.small, C_CORAL, 14, 0, 130, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_d_state, 1, 0);
    s_d_age = text(state_row, cc_fonts.small, C_DIM, 144, 0, 64, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_d_age, LV_TEXT_ALIGN_RIGHT, 0);

    s_d_qbox = lv_obj_create(s_d_scroll);
    lv_obj_remove_style_all(s_d_qbox);
    lv_obj_set_size(s_d_qbox, 208, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_d_qbox, lv_color_hex(C_ALERT_BG), 0);
    lv_obj_set_style_bg_opa(s_d_qbox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_d_qbox, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_border_width(s_d_qbox, 1, 0);
    lv_obj_set_style_radius(s_d_qbox, 10, 0);
    lv_obj_set_style_pad_all(s_d_qbox, 8, 0);
    lv_obj_set_style_pad_row(s_d_qbox, 4, 0);
    lv_obj_set_flex_flow(s_d_qbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_d_qbox, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_d_qtitle = text(s_d_qbox, cc_fonts.small, C_AMBER, 0, 0, 190, LV_LABEL_LONG_MODE_DOTS);
    s_d_qbody = text(s_d_qbox, cc_fonts.body, C_TEXT, 0, 0, 190, LV_LABEL_LONG_MODE_WRAP);

    s_d_now = text(s_d_scroll, cc_fonts.small, C_FAINT, 0, 0, 208, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_d_now, 2, 0);
    lv_label_set_text(s_d_now, "MAIN AGENT");
    s_d_activity = text(s_d_scroll, cc_fonts.body, C_TEXT, 0, 0, 208, LV_LABEL_LONG_MODE_WRAP);
    s_d_note_head = text(s_d_scroll, cc_fonts.small, C_FAINT, 0, 0, 208, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_d_note_head, 2, 0);
    lv_label_set_text(s_d_note_head, "LATEST");
    s_d_note = text(s_d_scroll, cc_fonts.body, C_DIM, 0, 0, 208, LV_LABEL_LONG_MODE_WRAP);
    s_d_agents_head = text(s_d_scroll, cc_fonts.small, C_FAINT, 0, 0, 208, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_d_agents_head, 2, 0);

    for (int i = 0; i < CC_MAX_AGENTS; i++) {
        // Height follows the wrapped activity (up to ~3 lines for 95 bytes).
        s_d_agent[i] = box(s_d_scroll, 0, 0, 208, LV_SIZE_CONTENT, C_SURFACE, 10);
        lv_obj_set_style_pad_bottom(s_d_agent[i], 6, 0);
        s_d_agent_dot[i] = dot(s_d_agent[i], 10, 9, 8, C_CORAL);
        s_d_agent_type[i] = text(s_d_agent[i], cc_fonts.small, C_TEXT, 24, 5, 120, LV_LABEL_LONG_MODE_DOTS);
        s_d_agent_age[i] = text(s_d_agent[i], cc_fonts.small, C_FAINT, 140, 5, 60, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_d_agent_age[i], LV_TEXT_ALIGN_RIGHT, 0);
        s_d_agent_act[i] = text(s_d_agent[i], cc_fonts.body, C_DIM, 10, 22, 190, LV_LABEL_LONG_MODE_WRAP);
    }

    lv_obj_t *hint = text(s_detail, cc_fonts.small, C_FAINT, 0, 296, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "\xE2\x96\xB2\xE2\x96\xBC scroll    OK back");
}

static const char *const SETTING_NAMES[CC_SET_COUNT] = {
    "Sound", "Brightness", "Screen", "Pair new Mac", "Forget Macs", "About",
};

static void build_settings(lv_obj_t *scr)
{
    s_set_page = layer(scr);
    lv_obj_t *title = text(s_set_page, cc_fonts.small, C_DIM, 20, 38, 200, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "SETTINGS");
    for (int i = 0; i < CC_SET_COUNT; i++) {
        s_set_row[i] = box(s_set_page, 12, 58 + i * 34, 216, 30, C_BG, 10);
        lv_obj_set_style_border_side(s_set_row[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(s_set_row[i], 0, 0);
        lv_obj_set_style_border_color(s_set_row[i], lv_color_hex(C_CORAL), 0);
        s_set_label[i] = text(s_set_row[i], cc_fonts.body, C_TEXT, 12, 5, 110, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(s_set_label[i], SETTING_NAMES[i]);
        s_set_value[i] = text(s_set_row[i], cc_fonts.small, C_DIM, 104, 8, 104, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(s_set_value[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    s_set_info = text(s_set_page, cc_fonts.small, C_FAINT, 20, 266, 200, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *hint = text(s_set_page, cc_fonts.small, C_FAINT, 0, 300, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "OK change    hold OK back");
}

static void build_wait_and_pair(lv_obj_t *scr)
{
    s_wait = layer(scr);
    spark_create(&s_wait_spark, s_wait, 96, 74, 48, 5, C_CORAL);
    s_wait_title = text(s_wait, cc_fonts.title, C_TEXT, 0, 140, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_wait_title, LV_TEXT_ALIGN_CENTER, 0);
    s_wait_body = text(s_wait, cc_fonts.small, C_DIM, 22, 172, 196, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_wait_body, LV_TEXT_ALIGN_CENTER, 0);
    s_wait_name = text(s_wait, cc_fonts.small, C_FAINT, 0, 280, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_wait_name, LV_TEXT_ALIGN_CENTER, 0);

    s_pair = layer(scr);
    lv_obj_t *title = text(s_pair, cc_fonts.small, C_AMBER, 0, 70, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "PAIR WITH YOUR MAC");
    lv_obj_t *panel = box(s_pair, 20, 100, 200, 76, C_SURFACE, 16);
    s_pair_code = text(panel, cc_fonts.num, C_TEXT, 0, 14, 200, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_pair_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *body = text(s_pair, cc_fonts.small, C_DIM, 22, 196, 196, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(body, "Type this code in the macOS\nBluetooth pairing dialog.");
    s_pair_name = text(s_pair, cc_fonts.small, C_FAINT, 0, 280, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_pair_name, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_overlays(lv_obj_t *scr)
{
    // Covers the whole "other sessions" list so no half-hidden row peeks out.
    // Rows: status word (12 px), project (16 px, one line), message (two lines).
    s_toast = box(scr, 12, 208, 216, 84, C_SURFACE2, 14);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(C_GREEN), 0);
    s_toast_dot = dot(s_toast, 12, 10, 8, C_GREEN);
    s_toast_head = text(s_toast, cc_fonts.small, C_GREEN, 26, 6, 180, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(s_toast_head, 1, 0);
    s_toast_project = text(s_toast, cc_fonts.body, C_TEXT, 12, 22, 192, LV_LABEL_LONG_MODE_DOTS);
    s_toast_text = text(s_toast, cc_fonts.body, C_DIM, 12, 42, 192, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_toast_text, 40);

    // The alert covers the whole rounded panel; its 30 px radius matches the glass.
    s_alert = box(scr, 0, 0, 240, 320, C_ALERT_BG, 30);
    lv_obj_set_style_border_width(s_alert, 2, 0);
    lv_obj_set_style_border_color(s_alert, lv_color_hex(C_AMBER), 0);
    s_alert_badge = box(s_alert, 98, 26, 44, 44, C_AMBER, LV_RADIUS_CIRCLE);
    s_alert_mark = text(s_alert_badge, cc_fonts.title, C_ALERT_BG, 0, 9, 44, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_alert_mark, LV_TEXT_ALIGN_CENTER, 0);
    s_alert_kind = text(s_alert, cc_fonts.small, C_AMBER, 0, 80, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_alert_kind, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_alert_kind, 2, 0);
    s_alert_project = text(s_alert, cc_fonts.title, C_TEXT, 20, 98, 200, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_alert_project, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(s_alert_project, 26);
    s_alert_qtitle = text(s_alert, cc_fonts.body, C_AMBER, 20, 128, 200, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_alert_qtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(s_alert_qtitle, 20);
    lv_obj_t *panel = box(s_alert, 16, 154, 208, 106, 0x15120C, 12);
    s_alert_body = text(panel, cc_fonts.body, C_TEXT, 10, 6, 188, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_alert_body, 96);
    lv_obj_t *hint = text(s_alert, cc_fonts.small, C_DIM, 0, 268, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "Answer in your terminal");
    lv_obj_t *keys = text(s_alert, cc_fonts.small, C_FAINT, 0, 288, 240, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(keys, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(keys, "OK dismiss   \xE2\x96\xB2 details   \xE2\x96\xBC next");
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void render_top_bar(uint32_t now)
{
    uint32_t ble = C_FAINT;
    switch (s_link_ui) {
    case CC_LINK_LIVE: ble = C_BLUE; break;
    case CC_LINK_STALE:
    case CC_LINK_SECURING:
    case CC_LINK_PAIRING:
    case CC_LINK_WAIT_BRIDGE: ble = C_AMBER; break;
    case CC_LINK_OFF: ble = C_RED; break;
    default: break;
    }
    set_color(s_ble_icon, ble, true);

    int64_t unix_s;
    int32_t offset;
    char buf[16] = "";
    if (cc_store_clock(&unix_s, &offset, now)) cc_format_clock(buf, sizeof(buf), unix_s, offset);
    set_text(s_clock, buf);

    int pct = cc_store_battery();
    const char *icon = LV_SYMBOL_BATTERY_EMPTY;
    if (pct >= 85) icon = LV_SYMBOL_BATTERY_FULL;
    else if (pct >= 60) icon = LV_SYMBOL_BATTERY_3;
    else if (pct >= 35) icon = LV_SYMBOL_BATTERY_2;
    else if (pct >= 10) icon = LV_SYMBOL_BATTERY_1;
    set_text(s_batt_icon, icon);
    // Unavailable reading (-1): icon only, no invented number.
    set_color(s_batt_icon, pct < 0 ? C_FAINT : (pct < 15 ? C_RED : C_DIM), true);
    if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct);
    else buf[0] = '\0';
    set_text(s_batt_pct, buf);
}

static void render_card(const cc_session_t *s, uint32_t now)
{
    uint32_t color = state_color(s->state);
    lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_border_color(s_card, LV_PART_MAIN), c)) {
        lv_obj_set_style_border_color(s_card, c, 0);
    }
    // The spark animates while anything runs: the main agent or background work.
    bool working = s->state == CC_SESSION_WORK || s->state == CC_SESSION_BG;
    show(s_card_spark.cont, working);
    show(s_card_dot, !working);
    if (working) spark_color(&s_card_spark, color);
    set_color(s_card_dot, color, false);
    set_color(s_card_state, color, true);
    set_text(s_card_state, state_label(s->state));
    char buf[CC_Q_TITLE_MAX + CC_Q_BODY_MAX + 4];   // "title: line"
    cc_format_age(buf, sizeof(buf), age_now(s->age_s, now));
    set_text(s_card_age, buf);
    set_text(s_card_project, s->project);

    if (cc_session_needs_user(s->state) && s->q_kind != CC_Q_NONE) {
        char line[CC_Q_BODY_MAX];
        first_line(line, sizeof(line), s->q_body);
        if (s->q_kind == CC_Q_PERM) snprintf(buf, sizeof(buf), "%s: %s", s->q_title, line);
        else snprintf(buf, sizeof(buf), "%s", line);
        set_color(s_card_activity, C_TEXT, true);
        set_text(s_card_activity, buf);
    } else {
        set_color(s_card_activity, s->state == CC_SESSION_DONE ? C_TEXT : C_DIM, true);
        set_text(s_card_activity, s->state == CC_SESSION_DONE && s->note[0] ? s->note : s->activity);
    }

    int shown = s->agent_count;
    for (int i = 0; i < CC_MAX_AGENTS; i++) {
        show(s_card_agent_dots[i], i < shown);
        if (i < shown) set_color(s_card_agent_dots[i], agent_color(s->agents[i].state), false);
    }
    int32_t x = shown ? 16 + shown * 12 + 4 : 16;
    if (lv_obj_get_style_x(s_card_agents, LV_PART_MAIN) != x) {
        lv_obj_set_x(s_card_agents, x);
        lv_obj_set_width(s_card_agents, 206 - x);
    }
    // "Tasks" covers subagents plus background shells, monitors, and workflows.
    const char *plural = s->agents_total == 1 ? "" : "s";
    if (s->agents_total == 0) snprintf(buf, sizeof(buf), "No subagents or tasks");
    else if (s->agents_running) snprintf(buf, sizeof(buf), "%u of %u task%s running",
                                         (unsigned)s->agents_running, (unsigned)s->agents_total, plural);
    else snprintf(buf, sizeof(buf), "%u task%s finished", (unsigned)s->agents_total, plural);
    set_text(s_card_agents, buf);
}

static void render_home(const cc_snapshot_t *snap, uint32_t now)
{
    bool stale = s_link_ui == CC_LINK_STALE;
    set_text(s_home_header, stale ? "BRIDGE QUIET" : "SESSIONS");
    set_color(s_home_header, stale ? C_AMBER : C_DIM, true);
    char buf[32];
    if (snap->count > 0 && s_view.focus >= 0) {
        snprintf(buf, sizeof(buf), "%d / %u", s_view.focus + 1,
                 (unsigned)(snap->total_sessions > snap->count ? snap->total_sessions : snap->count));
    } else {
        buf[0] = '\0';
    }
    set_text(s_home_count, buf);

    bool empty = snap->count == 0 || s_view.focus < 0;
    show(s_empty, empty);
    show(s_card, !empty);
    set_text(s_home_hint, empty ? "hold OK  menu"
                                : "\xE2\x96\xB2\xE2\x96\xBC select   OK open   hold OK menu");
    if (empty) {
        for (int i = 0; i < ROWS; i++) show(s_row[i], false);
        return;
    }
    render_card(&snap->sessions[s_view.focus], now);

    // Rows show what comes next in the loop, so DOWN pulls the first row up into
    // the card. From the top session (the idle default) this is priority order.
    int row = 0;
    for (int k = 1; k < snap->count && row < ROWS; k++) {
        const cc_session_t *s = &snap->sessions[(s_view.focus + k) % snap->count];
        show(s_row[row], true);
        set_color(s_row_dot[row], state_color(s->state), false);
        set_text(s_row_name[row], s->project);
        set_color(s_row_state[row], state_color(s->state), true);
        set_text(s_row_state[row], state_short(s->state));
        row++;
    }
    for (; row < ROWS; row++) show(s_row[row], false);
}

static void render_detail(const cc_snapshot_t *snap, uint32_t now)
{
    if (s_view.focus < 0 || s_view.focus >= snap->count) return;
    const cc_session_t *s = &snap->sessions[s_view.focus];
    uint32_t color = state_color(s->state);
    char buf[CC_Q_TITLE_MAX + 16];   // "QUESTION  " + title
    set_text(s_d_project, s->project);
    set_color(s_d_dot, color, false);
    set_color(s_d_state, color, true);
    set_text(s_d_state, state_label(s->state));
    cc_format_age(buf, sizeof(buf), age_now(s->age_s, now));
    set_text(s_d_age, buf);

    bool q = s->q_kind != CC_Q_NONE;
    show(s_d_qbox, q);
    if (q) {
        snprintf(buf, sizeof(buf), "%s  %s", s->q_kind == CC_Q_PERM ? "APPROVE" : "QUESTION", s->q_title);
        set_text(s_d_qtitle, buf);
        set_text(s_d_qbody, s->q_body);
    }
    // After the main turn the summary describes background work, not the main agent.
    set_text(s_d_now, s->state == CC_SESSION_BG ? "IN BACKGROUND" : "MAIN AGENT");
    set_text(s_d_activity, s->activity);
    bool note = s->note[0] != '\0';
    show(s_d_note_head, note);
    show(s_d_note, note);
    if (note) set_text(s_d_note, s->note);

    if (s->agents_total == 0) snprintf(buf, sizeof(buf), "NO SUBAGENTS OR TASKS");
    else snprintf(buf, sizeof(buf), "TASKS  %u/%u RUNNING", (unsigned)s->agents_running,
                  (unsigned)s->agents_total);
    set_text(s_d_agents_head, buf);
    for (int i = 0; i < CC_MAX_AGENTS; i++) {
        bool visible = i < s->agent_count;
        show(s_d_agent[i], visible);
        if (!visible) continue;
        const cc_agent_t *a = &s->agents[i];
        set_color(s_d_agent_dot[i], agent_color(a->state), false);
        set_text(s_d_agent_type[i], a->type);
        cc_format_age(buf, sizeof(buf), age_now(a->age_s, now));
        set_text(s_d_agent_age[i], buf);
        set_text(s_d_agent_act[i], a->activity);
    }

    // Apply the requested scroll, then clamp the view to what the content allows.
    lv_obj_update_layout(s_d_scroll);
    int32_t max = lv_obj_get_scroll_y(s_d_scroll) + lv_obj_get_scroll_bottom(s_d_scroll);
    int max_steps = max > 0 ? (int)((max + DETAIL_STEP - 1) / DETAIL_STEP) : 0;
    if (s_view.detail_scroll > max_steps) s_view.detail_scroll = max_steps;
    int32_t y = s_view.detail_scroll * DETAIL_STEP;
    if (y > max) y = max;
    if (lv_obj_get_scroll_y(s_d_scroll) != y) lv_obj_scroll_to_y(s_d_scroll, y, LV_ANIM_ON);
}

static void render_settings(uint32_t now)
{
    static const char *const BRIGHT[CC_BRIGHTNESS_LEVELS] = { "Low", "Medium", "High" };
    static const char *const TIMEOUT[CC_TIMEOUT_OPTIONS] = { "30 s", "1 min", "5 min", "Always on" };
    char buf[48];
    for (int i = 0; i < CC_SET_COUNT; i++) {
        bool sel = i == s_view.settings_index;
        set_color(s_set_row[i], sel ? C_SURFACE2 : C_BG, false);
        if (lv_obj_get_style_border_width(s_set_row[i], LV_PART_MAIN) != (sel ? 3 : 0)) {
            lv_obj_set_style_border_width(s_set_row[i], sel ? 3 : 0, 0);
        }
        switch ((cc_setting_item_t)i) {
        case CC_SET_SOUND:
            snprintf(buf, sizeof(buf), "%s", s_settings->sound ? "On" : "Off");
            break;
        case CC_SET_BRIGHTNESS:
            snprintf(buf, sizeof(buf), "%s", BRIGHT[s_settings->brightness % CC_BRIGHTNESS_LEVELS]);
            break;
        case CC_SET_TIMEOUT:
            snprintf(buf, sizeof(buf), "%s", TIMEOUT[s_settings->timeout % CC_TIMEOUT_OPTIONS]);
            break;
        case CC_SET_PAIR:
            if ((int32_t)(s_pair_until_ms - now) > 0) {
                snprintf(buf, sizeof(buf), "open %us", (unsigned)((s_pair_until_ms - now) / 1000u));
            } else {
                snprintf(buf, sizeof(buf), "%s", s_link.bonds == 0 ? "ready" : "");
            }
            break;
        case CC_SET_FORGET:
            if (s_view.forget_armed) snprintf(buf, sizeof(buf), "OK again");
            else snprintf(buf, sizeof(buf), "%u paired", (unsigned)s_link.bonds);
            break;
        case CC_SET_ABOUT:
        default:
            snprintf(buf, sizeof(buf), "v%s", s_fw);
            break;
        }
        set_color(s_set_value[i], i == CC_SET_FORGET && s_view.forget_armed ? C_RED : C_DIM, true);
        set_text(s_set_value[i], buf);
    }
    char name[24];
    char info[96];
    cc_store_device_name(name, sizeof(name));
    snprintf(info, sizeof(info), "%s\n%u KB free heap", name,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    set_text(s_set_info, info);
}

static void render_wait(void)
{
    const char *title = "Starting";
    const char *body = "Bringing up Bluetooth...";
    uint32_t spark = C_CORAL;
    switch (s_link_ui) {
    case CC_LINK_OFF:
        title = "Bluetooth off";
        body = "The radio failed to start. See the serial log for details.";
        spark = C_RED;
        break;
    case CC_LINK_ADVERTISING:
        title = "Waiting for your Mac";
        body = s_link.pairing_blocked
                   ? "A new Mac tried to pair.\nHold OK > Pair new Mac to allow it."
                   : "Start the companion bridge on your Mac. It connects on its own.";
        spark = s_link.pairing_blocked ? C_AMBER : C_FAINT;
        break;
    case CC_LINK_SECURING:
        title = "Securing link";
        body = "Encrypting the Bluetooth connection...";
        spark = C_AMBER;
        break;
    case CC_LINK_WAIT_BRIDGE:
        title = "Connected";
        body = "Waiting for Claude Code data from the bridge...";
        spark = C_BLUE;
        break;
    default:
        break;
    }
    set_text(s_wait_title, title);
    set_text(s_wait_body, body);
    spark_color(&s_wait_spark, spark);
    char name[24];
    cc_store_device_name(name, sizeof(name));
    set_text(s_wait_name, name);
}

static void render_alert(const cc_snapshot_t *snap)
{
    const cc_session_t *s = NULL;
    for (int i = 0; i < snap->count; i++) {
        if (strcmp(snap->sessions[i].sid, s_view.alert_sid) == 0) s = &snap->sessions[i];
    }
    bool ask = s ? s->q_kind == CC_Q_ASK || s->state == CC_SESSION_ASK : s_view.alert_kind == CC_EVT_ASK;
    set_text(s_alert_mark, ask ? "?" : "!");
    set_text(s_alert_kind, ask ? "QUESTION FOR YOU" : "NEEDS APPROVAL");
    set_text(s_alert_project, s ? s->project : "");
    set_text(s_alert_qtitle, s && s->q_title[0] ? s->q_title : (ask ? "Question" : "Permission"));
    set_text(s_alert_body, s && s->q_body[0] ? s->q_body : "Open the terminal to respond.");
}

static void render_toast(void)
{
    uint32_t color = s_view.toast_kind == CC_EVT_ERR ? C_RED : C_GREEN;
    lv_color_t c = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_border_color(s_toast, LV_PART_MAIN), c)) {
        lv_obj_set_style_border_color(s_toast, c, 0);
    }
    set_color(s_toast_dot, color, false);
    set_color(s_toast_head, color, true);
    set_text(s_toast_head, s_view.toast_kind == CC_EVT_ERR ? "STOPPED" : "FINISHED");
    set_text(s_toast_project, s_view.toast_project);
    set_text(s_toast_text, s_view.toast_text);
}

static void render(const cc_snapshot_t *snap, uint32_t now)
{
    render_top_bar(now);
    bool pairing = s_link_ui == CC_LINK_PAIRING;
    bool live = s_link_ui == CC_LINK_LIVE || s_link_ui == CC_LINK_STALE;
    bool settings = !pairing && s_view.page == CC_PAGE_SETTINGS;
    bool waiting = !pairing && !settings && !live;
    bool detail = !pairing && !settings && live && s_view.page == CC_PAGE_DETAIL;
    bool home = !pairing && !settings && live && !detail;

    show(s_pair, pairing);
    show(s_set_page, settings);
    show(s_wait, waiting);
    show(s_detail, detail);
    show(s_home, home);
    if (pairing) {
        char code[16];
        snprintf(code, sizeof(code), "%03u %03u", (unsigned)(s_link.passkey / 1000),
                 (unsigned)(s_link.passkey % 1000));
        set_text(s_pair_code, code);
        char name[24];
        cc_store_device_name(name, sizeof(name));
        set_text(s_pair_name, name);
    }
    if (settings) render_settings(now);
    if (waiting) render_wait();
    if (detail) render_detail(snap, now);
    if (home) render_home(snap, now);

    bool alert = s_view.alert && live && !pairing;
    show(s_alert, alert);
    if (alert) render_alert(snap);
    bool toast = s_view.toast && !alert && (home || detail);
    show(s_toast, toast);
    if (toast) render_toast();
}

static void apply_backlight(void)
{
    uint8_t pct = cc_view_backlight_percent(&s_view, s_settings);
    if (pct != s_applied_backlight) {
        bsp_display_backlight(pct);
        s_applied_backlight = pct;
    }
}

static cc_chime_t chime_for(cc_evt_kind_t kind)
{
    switch (kind) {
    case CC_EVT_PERM: return CC_CHIME_PERM;
    case CC_EVT_ASK: return CC_CHIME_ASK;
    case CC_EVT_ERR: return CC_CHIME_ERR;
    case CC_EVT_DONE:
    default: return CC_CHIME_DONE;
    }
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now = now_ms();
    unsigned act = CC_ACT_NONE;

    cc_event_t events[4];
    int n_events = 0;
    while (n_events < 4 && cc_store_pop_event(&events[n_events])) n_events++;

    uint32_t link_version = cc_store_link_version();
    bool link_changed = link_version != s_seen_link_version;
    if (link_changed) {
        s_seen_link_version = link_version;
        cc_store_link_get(&s_link);
    }
    cc_link_ui_t link_ui = cc_link_ui_state(&s_link, now);
    if (link_ui != s_link_ui) {
        if (link_ui == CC_LINK_PAIRING) act |= cc_view_wake(&s_view, now);
        s_link_ui = link_ui;
        link_changed = true;
    }

    uint32_t version = cc_store_version();
    bool second = now - s_last_second >= 1000u;
    if (second) s_last_second = now;

    const cc_snapshot_t *snap = cc_store_lock();
    if (version != s_seen_version) {
        s_seen_version = version;
        act |= cc_view_on_snapshot(&s_view, snap);
    }
    for (int i = 0; i < n_events; i++) {
        act |= cc_view_on_event(&s_view, snap, &events[i], now);
        if (s_sound_ok && s_settings->sound) cc_sound_play(chime_for(events[i].kind));
    }
    act |= cc_view_tick(&s_view, s_settings, now);
    act |= cc_view_follow(&s_view, snap, now);
    if ((act & CC_ACT_REDRAW) || link_changed || second) render(snap, now);
    cc_store_unlock();

    if (act & CC_ACT_BACKLIGHT) apply_backlight();
    if (s_view.backlight != CC_BL_OFF) {
        spark_step(&s_card_spark);
        spark_step(&s_wait_spark);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
esp_err_t cc_ui_init(cc_settings_t *settings, const char *fw_version, bool sound_ok)
{
    s_settings = settings;
    s_fw = fw_version;
    s_sound_ok = sound_ok;
    cc_view_init(&s_view, now_ms());

    cc_fonts_init();
    if (cc_fonts_self_test() != 0) ESP_LOGE(TAG, "font coverage incomplete; see log above");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_top_bar(scr);
    build_home(scr);
    build_detail(scr);
    build_settings(scr);
    build_wait_and_pair(scr);
    build_overlays(scr);
    show(s_home, false);
    show(s_detail, false);
    show(s_set_page, false);
    show(s_pair, false);
    show(s_alert, false);
    show(s_toast, false);
    lv_screen_load(scr);

    const cc_snapshot_t *snap = cc_store_lock();
    render(snap, now_ms());
    cc_store_unlock();
    apply_backlight();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool: %u/%u bytes used, biggest free %u", (unsigned)(mon.total_size - mon.free_size),
             (unsigned)mon.total_size, (unsigned)mon.free_biggest_size);

    return lv_timer_create(tick_cb, TICK_MS, NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

unsigned cc_ui_handle_key(cc_key_t key, cc_press_t press)
{
    const uint32_t now = now_ms();
    bool live = s_link_ui == CC_LINK_LIVE || s_link_ui == CC_LINK_STALE;
    if (!live && s_view.page == CC_PAGE_HOME && s_view.backlight == CC_BL_FULL &&
        !(key == CC_KEY_OK && press == CC_PRESS_LONG)) {
        // Nothing to navigate on the waiting screens; only settings is reachable.
        s_view.last_activity_ms = now;
        return CC_ACT_NONE;
    }
    const cc_snapshot_t *snap = cc_store_lock();
    unsigned act = cc_view_on_key(&s_view, snap, s_settings, key, press, now);
    if (act & (CC_ACT_REDRAW | CC_ACT_BACKLIGHT)) render(snap, now);
    cc_store_unlock();
    if (act & CC_ACT_BACKLIGHT) apply_backlight();
    return act;
}

void cc_ui_note_pair_window(uint32_t seconds)
{
    s_pair_until_ms = now_ms() + seconds * 1000u;
}
