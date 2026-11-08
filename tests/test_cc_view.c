// Host tests for main/cc_view.c (navigation, alerts, backlight, link status).
#include "cc_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static cc_snapshot_t snap;

static void add_session(const char *sid, cc_session_state_t state)
{
    cc_session_t *s = &snap.sessions[snap.count++];
    memset(s, 0, sizeof(*s));
    strcpy(s->sid, sid);
    strcpy(s->project, sid);
    s->state = state;
}

static void reset_snapshot(void)
{
    memset(&snap, 0, sizeof(snap));
}

static void test_focus_follows_sid(void)
{
    cc_view_t v;
    cc_view_init(&v, 0);
    reset_snapshot();
    add_session("a", CC_SESSION_WORK);
    add_session("b", CC_SESSION_DONE);
    add_session("c", CC_SESSION_IDLE);
    cc_view_on_snapshot(&v, &snap);
    CHECK(v.focus == 0 && strcmp(v.focus_sid, "a") == 0);

    cc_settings_t set;
    cc_settings_defaults(&set);
    CHECK(cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 10) & CC_ACT_REDRAW);
    CHECK(v.focus == 1 && strcmp(v.focus_sid, "b") == 0);

    // Reordered snapshot: focus stays on "b".
    reset_snapshot();
    add_session("b", CC_SESSION_PERM);
    add_session("a", CC_SESSION_WORK);
    add_session("c", CC_SESSION_IDLE);
    cc_view_on_snapshot(&v, &snap);
    CHECK(v.focus == 0 && strcmp(v.focus_sid, "b") == 0);

    // The cards loop at both ends; a double click moves two steps.
    CHECK(cc_view_on_key(&v, &snap, &set, CC_KEY_UP, CC_PRESS_CLICK, 20) & CC_ACT_REDRAW);
    CHECK(v.focus == 2 && strcmp(v.focus_sid, "c") == 0);
    CHECK(cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 25) & CC_ACT_REDRAW);
    CHECK(v.focus == 0);
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_DOUBLE, 30);
    CHECK(v.focus == 2);
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_DOUBLE, 35);
    CHECK(v.focus == 1);   // 2 -> 0 -> 1
    cc_view_on_key(&v, &snap, &set, CC_KEY_UP, CC_PRESS_CLICK, 38);
    cc_view_on_key(&v, &snap, &set, CC_KEY_UP, CC_PRESS_CLICK, 39);
    CHECK(v.focus == 2);

    // With two sessions a double click still moves one card, and a single
    // session does not move at all.
    {
        cc_snapshot_t saved = snap;
        cc_view_t two;
        cc_view_init(&two, 0);
        reset_snapshot();
        add_session("x", CC_SESSION_WORK);
        add_session("y", CC_SESSION_DONE);
        cc_view_on_snapshot(&two, &snap);
        cc_view_on_key(&two, &snap, &set, CC_KEY_DOWN, CC_PRESS_DOUBLE, 40);
        CHECK(two.focus == 1);
        cc_view_on_key(&two, &snap, &set, CC_KEY_UP, CC_PRESS_DOUBLE, 41);
        CHECK(two.focus == 0);
        reset_snapshot();
        add_session("solo", CC_SESSION_WORK);
        cc_view_on_snapshot(&two, &snap);
        CHECK(cc_view_on_key(&two, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 42) == CC_ACT_NONE);
        snap = saved;
    }

    // Focused session disappears while on the details page: back home, nearest focus.
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 50);
    CHECK(v.page == CC_PAGE_DETAIL);
    reset_snapshot();
    add_session("b", CC_SESSION_PERM);
    add_session("a", CC_SESSION_WORK);
    cc_view_on_snapshot(&v, &snap);
    CHECK(v.page == CC_PAGE_HOME);
    CHECK(v.focus == 1 && strcmp(v.focus_sid, "a") == 0);

    // Empty snapshot.
    reset_snapshot();
    cc_view_on_snapshot(&v, &snap);
    CHECK(v.focus == -1 && v.focus_sid[0] == '\0');
    CHECK(cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 60) == CC_ACT_NONE);
    CHECK(v.page == CC_PAGE_HOME);
}

static void test_alerts(void)
{
    cc_view_t v;
    cc_settings_t set;
    cc_settings_defaults(&set);
    cc_view_init(&v, 0);
    reset_snapshot();
    add_session("a", CC_SESSION_WORK);
    add_session("b", CC_SESSION_PERM);
    add_session("c", CC_SESSION_ASK);
    cc_view_on_snapshot(&v, &snap);

    cc_event_t e = { .kind = CC_EVT_PERM };
    strcpy(e.sid, "b");
    strcpy(e.project, "b");
    cc_view_on_event(&v, &snap, &e, 100);
    CHECK(v.alert && strcmp(v.alert_sid, "b") == 0 && v.focus == 1);

    // DOWN jumps to the next session that needs the user.
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 110);
    CHECK(v.alert && strcmp(v.alert_sid, "c") == 0 && v.alert_kind == CC_EVT_ASK && v.focus == 2);
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 120);
    CHECK(v.alert && strcmp(v.alert_sid, "b") == 0);

    // UP opens the details page of that session.
    cc_view_on_key(&v, &snap, &set, CC_KEY_UP, CC_PRESS_CLICK, 130);
    CHECK(!v.alert && v.page == CC_PAGE_DETAIL && v.focus == 1);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 140);
    CHECK(v.page == CC_PAGE_HOME);

    // The alert clears itself once the session stops waiting.
    cc_view_on_event(&v, &snap, &e, 200);
    CHECK(v.alert);
    snap.sessions[1].state = CC_SESSION_WORK;
    cc_view_on_snapshot(&v, &snap);
    CHECK(!v.alert);

    // OK dismisses.
    snap.sessions[1].state = CC_SESSION_PERM;
    cc_view_on_event(&v, &snap, &e, 300);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 310);
    CHECK(!v.alert && v.focus == 1);

    // Done events show a toast that expires; a key press also clears it.
    cc_event_t done = { .kind = CC_EVT_DONE };
    strcpy(done.sid, "a");
    strcpy(done.project, "a");
    strcpy(done.text, "All tests pass");
    cc_view_on_event(&v, &snap, &done, 1000);
    CHECK(v.toast && !v.alert);
    CHECK(cc_view_tick(&v, &set, 1000 + CC_TOAST_MS - 1) == CC_ACT_NONE);
    CHECK(cc_view_tick(&v, &set, 1000 + CC_TOAST_MS) & CC_ACT_REDRAW);
    CHECK(!v.toast);
}

static void test_settings(void)
{
    cc_view_t v;
    cc_settings_t set;
    cc_settings_defaults(&set);
    cc_view_init(&v, 0);
    reset_snapshot();
    cc_view_on_snapshot(&v, &snap);

    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_LONG, 10);
    CHECK(v.page == CC_PAGE_SETTINGS && v.settings_index == CC_SET_SOUND);
    unsigned act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 20);
    CHECK(set.sound == 0 && (act & CC_ACT_SAVE_SETTINGS) && !(act & CC_ACT_SOUND_PREVIEW));
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 30);
    CHECK(set.sound == 1 && (act & CC_ACT_SOUND_PREVIEW));

    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 40);
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 50);
    CHECK(set.brightness == 2 && (act & CC_ACT_BACKLIGHT));
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 60);
    CHECK(set.brightness == 0);

    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 70);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 80);
    CHECK(set.timeout == 2);

    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 90);
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 100);
    CHECK(act & CC_ACT_PAIR_WINDOW);

    // Forget requires a second OK within the confirmation window.
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 110);
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 120);
    CHECK(!(act & CC_ACT_FORGET_BONDS) && v.forget_armed);
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 120 + CC_FORGET_CONFIRM_MS);
    CHECK(act & CC_ACT_FORGET_BONDS);
    CHECK(!v.forget_armed);
    // An expired confirmation does not forget.
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 10000);
    cc_view_tick(&v, &set, 10000 + CC_FORGET_CONFIRM_MS + 1);
    CHECK(!v.forget_armed);
    act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 10000 + CC_FORGET_CONFIRM_MS + 2);
    CHECK(!(act & CC_ACT_FORGET_BONDS));

    // Clamped at the last item; long OK returns home.
    for (int i = 0; i < 10; i++) cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 20000);
    CHECK(v.settings_index == CC_SET_COUNT - 1);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_LONG, 20010);
    CHECK(v.page == CC_PAGE_HOME);

    cc_settings_t bad = { .sound = 7, .brightness = 9, .timeout = 200 };
    cc_settings_sanitize(&bad);
    CHECK(bad.sound == 1 && bad.brightness < CC_BRIGHTNESS_LEVELS && bad.timeout < CC_TIMEOUT_OPTIONS);
}

static void test_backlight(void)
{
    cc_view_t v;
    cc_settings_t set;
    cc_settings_defaults(&set);   // 60 s timeout
    cc_view_init(&v, 0);
    reset_snapshot();
    add_session("a", CC_SESSION_WORK);
    cc_view_on_snapshot(&v, &snap);

    CHECK(cc_view_tick(&v, &set, 59999) == CC_ACT_NONE && v.backlight == CC_BL_FULL);
    CHECK(cc_view_tick(&v, &set, 60000) & CC_ACT_BACKLIGHT);
    CHECK(v.backlight == CC_BL_DIM && cc_view_backlight_percent(&v, &set) > 0);
    CHECK(cc_view_tick(&v, &set, 60000 + CC_DIM_TO_OFF_MS) & CC_ACT_BACKLIGHT);
    CHECK(v.backlight == CC_BL_OFF && cc_view_backlight_percent(&v, &set) == 0);

    // The first key on a dark screen only wakes it.
    unsigned act = cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 200000);
    CHECK((act & CC_ACT_BACKLIGHT) && v.backlight == CC_BL_FULL && v.page == CC_PAGE_HOME);

    // An approval alert keeps the screen lit for CC_ALERT_HOLD_MS.
    cc_event_t e = { .kind = CC_EVT_PERM };
    strcpy(e.sid, "a");
    snap.sessions[0].state = CC_SESSION_PERM;
    cc_view_on_event(&v, &snap, &e, 300000);
    cc_view_tick(&v, &set, 300000 + 59000);
    CHECK(v.backlight == CC_BL_FULL);
    cc_view_tick(&v, &set, 300000 + CC_ALERT_HOLD_MS + 1);
    CHECK(v.backlight == CC_BL_DIM);

    // "Never" keeps the screen on.
    set.timeout = 3;
    cc_view_tick(&v, &set, 900000000);
    CHECK(v.backlight == CC_BL_FULL);
    CHECK(cc_view_timeout_s(3) == 0 && cc_view_timeout_s(0) == 30);
}

static void test_follow_priority_when_idle(void)
{
    cc_view_t v;
    cc_settings_t set;
    cc_settings_defaults(&set);
    cc_view_init(&v, 0);
    reset_snapshot();
    add_session("err", CC_SESSION_ERR);
    add_session("work", CC_SESSION_WORK);
    add_session("done", CC_SESSION_DONE);
    cc_view_on_snapshot(&v, &snap);
    CHECK(cc_view_follow(&v, &snap, 0) == CC_ACT_NONE);   // already on the top session

    // A manual selection holds while the user is active...
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 1000);
    CHECK(v.focus == 1);
    CHECK(cc_view_follow(&v, &snap, 1000 + CC_FOLLOW_IDLE_MS - 1) == CC_ACT_NONE);
    CHECK(v.focus == 1);
    // ...and gives way to the top-priority session after the idle period.
    CHECK(cc_view_follow(&v, &snap, 1000 + CC_FOLLOW_IDLE_MS) & CC_ACT_REDRAW);
    CHECK(v.focus == 0 && strcmp(v.focus_sid, "err") == 0);

    // Not while reading details or when an alert is up.
    cc_view_on_key(&v, &snap, &set, CC_KEY_DOWN, CC_PRESS_CLICK, 100000);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 100001);
    CHECK(v.page == CC_PAGE_DETAIL);
    CHECK(cc_view_follow(&v, &snap, 100001 + CC_FOLLOW_IDLE_MS) == CC_ACT_NONE && v.focus == 1);
    cc_view_on_key(&v, &snap, &set, CC_KEY_OK, CC_PRESS_CLICK, 200000);
    snap.sessions[2].state = CC_SESSION_PERM;
    cc_event_t e = { .kind = CC_EVT_PERM };
    strcpy(e.sid, "done");
    cc_view_on_event(&v, &snap, &e, 200001);
    CHECK(cc_view_follow(&v, &snap, 200001 + CC_FOLLOW_IDLE_MS) == CC_ACT_NONE && v.focus == 2);
}

static void test_link_state(void)
{
    cc_link_info_t l = { 0 };
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_STARTING);
    l.ble_started = true;
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_ADVERTISING);
    l.connected = true;
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_SECURING);
    l.passkey_active = true;
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_PAIRING);
    l.passkey_active = false;
    l.secure = true;
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_WAIT_BRIDGE);
    l.have_data = true;
    l.last_rx_ms = 1000;
    CHECK(cc_link_ui_state(&l, 1000 + CC_STALE_MS) == CC_LINK_LIVE);
    CHECK(cc_link_ui_state(&l, 1001 + CC_STALE_MS) == CC_LINK_STALE);
    l.ble_failed = true;
    CHECK(cc_link_ui_state(&l, 0) == CC_LINK_OFF);
}

int main(void)
{
    test_focus_follows_sid();
    test_alerts();
    test_settings();
    test_backlight();
    test_follow_priority_when_idle();
    test_link_state();
    if (failures) {
        fprintf(stderr, "test_cc_view: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_cc_view: PASS\n");
    return EXIT_SUCCESS;
}
