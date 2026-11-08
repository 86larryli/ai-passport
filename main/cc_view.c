// main/cc_view.c — see cc_view.h.
#include "cc_view.h"

#include <string.h>

static const uint32_t TIMEOUTS_S[CC_TIMEOUT_OPTIONS] = { 30, 60, 300, 0 };
static const uint8_t BRIGHTNESS_PCT[CC_BRIGHTNESS_LEVELS] = { 30, 60, 100 };
#define DIM_PCT 6

static void copy_sid(char *dst, const char *src)
{
    strncpy(dst, src, CC_SID_MAX - 1);
    dst[CC_SID_MAX - 1] = '\0';
}

static int find_sid(const cc_snapshot_t *snap, const char *sid)
{
    if (!snap || sid[0] == '\0') return -1;
    for (int i = 0; i < snap->count; i++) {
        if (strcmp(snap->sessions[i].sid, sid) == 0) return i;
    }
    return -1;
}

static void set_focus(cc_view_t *v, const cc_snapshot_t *snap, int index)
{
    if (!snap || snap->count == 0) {
        v->focus = -1;
        v->focus_sid[0] = '\0';
        return;
    }
    if (index < 0) index = 0;
    if (index >= snap->count) index = snap->count - 1;
    v->focus = index;
    copy_sid(v->focus_sid, snap->sessions[index].sid);
}

void cc_settings_defaults(cc_settings_t *s)
{
    s->sound = 1;
    s->brightness = 1;
    s->timeout = 1;
}

void cc_settings_sanitize(cc_settings_t *s)
{
    if (s->sound > 1) s->sound = 1;
    if (s->brightness >= CC_BRIGHTNESS_LEVELS) s->brightness = 1;
    if (s->timeout >= CC_TIMEOUT_OPTIONS) s->timeout = 1;
}

uint32_t cc_view_timeout_s(uint8_t index)
{
    return index < CC_TIMEOUT_OPTIONS ? TIMEOUTS_S[index] : TIMEOUTS_S[1];
}

void cc_view_init(cc_view_t *v, uint32_t now_ms)
{
    memset(v, 0, sizeof(*v));
    v->page = CC_PAGE_HOME;
    v->focus = -1;
    v->last_activity_ms = now_ms;
    v->backlight = CC_BL_FULL;
}

unsigned cc_view_on_snapshot(cc_view_t *v, const cc_snapshot_t *snap)
{
    int index = find_sid(snap, v->focus_sid);
    if (index < 0) {
        // Focused session ended (or first snapshot): stay near the old position.
        if (v->page == CC_PAGE_DETAIL) {
            v->page = CC_PAGE_HOME;
            v->detail_scroll = 0;
        }
        set_focus(v, snap, v->focus < 0 ? 0 : v->focus);
    } else {
        v->focus = index;
    }
    if (v->alert) {
        int a = find_sid(snap, v->alert_sid);
        // An approval/answer alert ends once that session no longer waits for the user.
        if (a < 0 || !cc_session_needs_user(snap->sessions[a].state)) v->alert = false;
    }
    return CC_ACT_REDRAW;
}

unsigned cc_view_follow(cc_view_t *v, const cc_snapshot_t *snap, uint32_t now_ms)
{
    if (v->page != CC_PAGE_HOME || v->alert || !snap || snap->count == 0) return CC_ACT_NONE;
    if (v->key_seen && now_ms - v->last_key_ms < CC_FOLLOW_IDLE_MS) return CC_ACT_NONE;
    if (v->focus == 0 && strcmp(v->focus_sid, snap->sessions[0].sid) == 0) return CC_ACT_NONE;
    set_focus(v, snap, 0);
    return CC_ACT_REDRAW;
}

unsigned cc_view_wake(cc_view_t *v, uint32_t now_ms)
{
    v->last_activity_ms = now_ms;
    if (v->backlight != CC_BL_FULL) {
        v->backlight = CC_BL_FULL;
        return CC_ACT_BACKLIGHT | CC_ACT_REDRAW;
    }
    return CC_ACT_NONE;
}

unsigned cc_view_on_event(cc_view_t *v, const cc_snapshot_t *snap, const cc_event_t *e,
                          uint32_t now_ms)
{
    unsigned act = CC_ACT_REDRAW | cc_view_wake(v, now_ms);
    if (e->kind == CC_EVT_PERM || e->kind == CC_EVT_ASK) {
        v->alert = true;
        v->alert_kind = e->kind;
        v->alert_ms = now_ms;
        copy_sid(v->alert_sid, e->sid);
        int index = find_sid(snap, e->sid);
        if (index >= 0) set_focus(v, snap, index);
        else copy_sid(v->focus_sid, e->sid);   // resolved by the next snapshot
        if (v->page == CC_PAGE_DETAIL && index < 0) v->page = CC_PAGE_HOME;
    } else {
        v->toast = true;
        v->toast_kind = e->kind;
        strncpy(v->toast_project, e->project, sizeof(v->toast_project) - 1);
        v->toast_project[sizeof(v->toast_project) - 1] = '\0';
        strncpy(v->toast_text, e->text, sizeof(v->toast_text) - 1);
        v->toast_text[sizeof(v->toast_text) - 1] = '\0';
        v->toast_until_ms = now_ms + CC_TOAST_MS;
    }
    return act;
}

static int next_needing_user(const cc_snapshot_t *snap, const char *after_sid)
{
    if (!snap || snap->count == 0) return -1;
    int start = find_sid(snap, after_sid);
    for (int step = 1; step <= snap->count; step++) {
        int i = (start + step) % snap->count;
        if (i == start) break;
        if (cc_session_needs_user(snap->sessions[i].state)) return i;
    }
    return -1;
}

static unsigned key_alert(cc_view_t *v, const cc_snapshot_t *snap, cc_key_t key, cc_press_t press)
{
    if (key == CC_KEY_OK) {
        v->alert = false;
        int index = find_sid(snap, v->alert_sid);
        if (index >= 0) set_focus(v, snap, index);
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_UP && press != CC_PRESS_LONG) {
        v->alert = false;
        int index = find_sid(snap, v->alert_sid);
        if (index >= 0) {
            set_focus(v, snap, index);
            v->page = CC_PAGE_DETAIL;
            v->detail_scroll = 0;
        }
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_DOWN && press != CC_PRESS_LONG) {
        int next = next_needing_user(snap, v->alert_sid);
        if (next >= 0) {
            copy_sid(v->alert_sid, snap->sessions[next].sid);
            v->alert_kind = snap->sessions[next].state == CC_SESSION_ASK ? CC_EVT_ASK : CC_EVT_PERM;
            set_focus(v, snap, next);
        } else {
            v->alert = false;
        }
        return CC_ACT_REDRAW;
    }
    return CC_ACT_NONE;
}

static unsigned key_home(cc_view_t *v, const cc_snapshot_t *snap, cc_key_t key, cc_press_t press)
{
    if ((key == CC_KEY_UP || key == CC_KEY_DOWN) && press != CC_PRESS_LONG) {
        // The session cards loop: the counter ("2 / 4") keeps the position clear,
        // and there are at most CC_MAX_SESSIONS of them.
        int n = snap ? snap->count : 0;
        if (n < 2 || v->focus < 0) return CC_ACT_NONE;
        // The driver merges quick taps into a double click: two steps, except
        // with two sessions, where two steps would land back on the same card.
        int steps = (press == CC_PRESS_DOUBLE && n > 2) ? 2 : 1;
        int next = key == CC_KEY_UP ? (v->focus - steps + n) % n : (v->focus + steps) % n;
        set_focus(v, snap, next);
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_OK && press == CC_PRESS_LONG) {
        v->page = CC_PAGE_SETTINGS;
        v->settings_index = 0;
        v->forget_armed = false;
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_OK && v->focus >= 0) {
        v->page = CC_PAGE_DETAIL;
        v->detail_scroll = 0;
        return CC_ACT_REDRAW;
    }
    return CC_ACT_NONE;
}

static unsigned key_detail(cc_view_t *v, cc_key_t key, cc_press_t press)
{
    int steps = press == CC_PRESS_DOUBLE ? 2 : 1;
    if (key == CC_KEY_OK) {
        v->page = CC_PAGE_HOME;
        v->detail_scroll = 0;
        return CC_ACT_REDRAW;
    }
    if (press == CC_PRESS_LONG) return CC_ACT_NONE;
    if (key == CC_KEY_UP && v->detail_scroll > 0) {
        v->detail_scroll -= steps;
        if (v->detail_scroll < 0) v->detail_scroll = 0;
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_DOWN && v->detail_scroll < CC_DETAIL_SCROLL_MAX) {
        v->detail_scroll += steps;
        if (v->detail_scroll > CC_DETAIL_SCROLL_MAX) v->detail_scroll = CC_DETAIL_SCROLL_MAX;
        return CC_ACT_REDRAW;
    }
    return CC_ACT_NONE;
}

static unsigned key_settings(cc_view_t *v, cc_settings_t *s, cc_key_t key, cc_press_t press,
                             uint32_t now_ms)
{
    if (key == CC_KEY_OK && press == CC_PRESS_LONG) {
        v->page = CC_PAGE_HOME;
        v->forget_armed = false;
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_UP && press != CC_PRESS_LONG) {
        if (v->settings_index > 0) v->settings_index--;
        v->forget_armed = false;
        return CC_ACT_REDRAW;
    }
    if (key == CC_KEY_DOWN && press != CC_PRESS_LONG) {
        if (v->settings_index < CC_SET_COUNT - 1) v->settings_index++;
        v->forget_armed = false;
        return CC_ACT_REDRAW;
    }
    if (key != CC_KEY_OK) return CC_ACT_NONE;
    switch ((cc_setting_item_t)v->settings_index) {
    case CC_SET_SOUND:
        s->sound = s->sound ? 0 : 1;
        return CC_ACT_REDRAW | CC_ACT_SAVE_SETTINGS | (s->sound ? CC_ACT_SOUND_PREVIEW : 0);
    case CC_SET_BRIGHTNESS:
        s->brightness = (uint8_t)((s->brightness + 1) % CC_BRIGHTNESS_LEVELS);
        return CC_ACT_REDRAW | CC_ACT_SAVE_SETTINGS | CC_ACT_BACKLIGHT;
    case CC_SET_TIMEOUT:
        s->timeout = (uint8_t)((s->timeout + 1) % CC_TIMEOUT_OPTIONS);
        return CC_ACT_REDRAW | CC_ACT_SAVE_SETTINGS;
    case CC_SET_PAIR:
        return CC_ACT_REDRAW | CC_ACT_PAIR_WINDOW;
    case CC_SET_FORGET:
        if (v->forget_armed && now_ms - v->forget_armed_ms <= CC_FORGET_CONFIRM_MS) {
            v->forget_armed = false;
            return CC_ACT_REDRAW | CC_ACT_FORGET_BONDS;
        }
        v->forget_armed = true;
        v->forget_armed_ms = now_ms;
        return CC_ACT_REDRAW;
    case CC_SET_ABOUT:
    default:
        return CC_ACT_NONE;
    }
}

unsigned cc_view_on_key(cc_view_t *v, const cc_snapshot_t *snap, cc_settings_t *settings,
                        cc_key_t key, cc_press_t press, uint32_t now_ms)
{
    v->last_key_ms = now_ms;
    v->key_seen = true;
    if (v->backlight != CC_BL_FULL) {
        // A dark screen gives no feedback; the first press only wakes it.
        return cc_view_wake(v, now_ms);
    }
    v->last_activity_ms = now_ms;
    unsigned act = CC_ACT_NONE;
    if (v->toast) {
        v->toast = false;
        act |= CC_ACT_REDRAW;
    }
    if (v->alert) return act | key_alert(v, snap, key, press);
    switch (v->page) {
    case CC_PAGE_HOME:
        return act | key_home(v, snap, key, press);
    case CC_PAGE_DETAIL:
        return act | key_detail(v, key, press);
    case CC_PAGE_SETTINGS:
        return act | key_settings(v, settings, key, press, now_ms);
    }
    return act;
}

unsigned cc_view_tick(cc_view_t *v, const cc_settings_t *settings, uint32_t now_ms)
{
    unsigned act = CC_ACT_NONE;
    if (v->toast && (int32_t)(now_ms - v->toast_until_ms) >= 0) {
        v->toast = false;
        act |= CC_ACT_REDRAW;
    }
    if (v->forget_armed && now_ms - v->forget_armed_ms > CC_FORGET_CONFIRM_MS) {
        v->forget_armed = false;
        act |= CC_ACT_REDRAW;
    }

    cc_backlight_t want = CC_BL_FULL;
    uint32_t timeout_ms = cc_view_timeout_s(settings->timeout) * 1000u;
    uint32_t idle = now_ms - v->last_activity_ms;
    bool alert_hold = v->alert && now_ms - v->alert_ms < CC_ALERT_HOLD_MS;
    if (timeout_ms != 0 && !alert_hold) {
        if (idle >= timeout_ms + CC_DIM_TO_OFF_MS) want = CC_BL_OFF;
        else if (idle >= timeout_ms) want = CC_BL_DIM;
    }
    if (want != v->backlight) {
        v->backlight = want;
        act |= CC_ACT_BACKLIGHT;
    }
    return act;
}

uint8_t cc_view_backlight_percent(const cc_view_t *v, const cc_settings_t *settings)
{
    switch (v->backlight) {
    case CC_BL_OFF:
        return 0;
    case CC_BL_DIM:
        return DIM_PCT;
    case CC_BL_FULL:
    default:
        return BRIGHTNESS_PCT[settings->brightness < CC_BRIGHTNESS_LEVELS ? settings->brightness : 1];
    }
}

cc_link_ui_t cc_link_ui_state(const cc_link_info_t *l, uint32_t now_ms)
{
    if (l->ble_failed) return CC_LINK_OFF;
    if (!l->ble_started) return CC_LINK_STARTING;
    if (!l->connected) return CC_LINK_ADVERTISING;
    if (l->passkey_active) return CC_LINK_PAIRING;
    if (!l->secure) return CC_LINK_SECURING;
    if (!l->have_data) return CC_LINK_WAIT_BRIDGE;
    if (now_ms - l->last_rx_ms > CC_STALE_MS) return CC_LINK_STALE;
    return CC_LINK_LIVE;
}
