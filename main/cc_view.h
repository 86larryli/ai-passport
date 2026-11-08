// main/cc_view.h — UI state machine of the Claude Code Companion (pure C).
//
// cc_ui.c renders whatever this state says; all navigation, focus, alert,
// backlight, and link-status decisions live here so tests/test_cc_view.c can
// cover them without LVGL. Functions return a bit set of cc_action_t telling
// the caller what to do next (redraw, persist settings, talk to BLE, ...).
//
// Controls (three ADC keys):
//   Home      UP/DOWN click: previous/next session (loops around)
//             OK click: open session details; OK long: settings
//             After CC_FOLLOW_IDLE_MS without a key press the card follows the
//             most important session again (the bridge sends them in priority order).
//   Details   UP/DOWN click: scroll; OK click or long: back to home
//   Alert     OK click/long: dismiss; UP: open that session's details;
//             DOWN: next session that needs you (or dismiss)
//   Settings  UP/DOWN: select; OK click: change; OK long: back
// Any key while the backlight is dimmed or off only wakes the screen.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cc_types.h"

typedef enum {
    CC_PAGE_HOME = 0,
    CC_PAGE_DETAIL,
    CC_PAGE_SETTINGS,
} cc_page_t;

typedef enum { CC_KEY_UP = 0, CC_KEY_DOWN, CC_KEY_OK } cc_key_t;
typedef enum { CC_PRESS_CLICK = 0, CC_PRESS_DOUBLE, CC_PRESS_LONG } cc_press_t;

typedef enum { CC_BL_FULL = 0, CC_BL_DIM, CC_BL_OFF } cc_backlight_t;

typedef enum {
    CC_SET_SOUND = 0,
    CC_SET_BRIGHTNESS,
    CC_SET_TIMEOUT,
    CC_SET_PAIR,
    CC_SET_FORGET,
    CC_SET_ABOUT,
    CC_SET_COUNT
} cc_setting_item_t;

#define CC_BRIGHTNESS_LEVELS 3
#define CC_TIMEOUT_OPTIONS 4
#define CC_DETAIL_SCROLL_MAX 40
#define CC_TOAST_MS 15000u
#define CC_FOLLOW_IDLE_MS 30000u   // manual selection holds this long after the last key
#define CC_ALERT_HOLD_MS 60000u     // keep the screen fully lit this long after an alert
#define CC_DIM_TO_OFF_MS 60000u     // dimmed this long before switching off
#define CC_FORGET_CONFIRM_MS 4000u
#define CC_STALE_MS 20000u          // no line from the bridge for this long: "bridge quiet"

typedef struct {
    uint8_t sound;        // 0 off, 1 on
    uint8_t brightness;   // 0..CC_BRIGHTNESS_LEVELS-1
    uint8_t timeout;      // index into cc_view_timeout_s()
} cc_settings_t;

typedef enum {
    CC_ACT_NONE = 0,
    CC_ACT_REDRAW = 1u << 0,
    CC_ACT_SAVE_SETTINGS = 1u << 1,
    CC_ACT_PAIR_WINDOW = 1u << 2,
    CC_ACT_FORGET_BONDS = 1u << 3,
    CC_ACT_SOUND_PREVIEW = 1u << 4,
    CC_ACT_BACKLIGHT = 1u << 5,
} cc_action_t;

typedef struct {
    cc_page_t page;
    char focus_sid[CC_SID_MAX];
    int focus;                    // index in the current snapshot, -1 when empty
    int detail_scroll;            // scroll steps requested on the details page
    int settings_index;
    bool forget_armed;
    uint32_t forget_armed_ms;

    bool alert;                   // full-screen attention card
    cc_evt_kind_t alert_kind;
    char alert_sid[CC_SID_MAX];
    uint32_t alert_ms;

    bool toast;                   // short banner for done/error events
    cc_evt_kind_t toast_kind;
    char toast_project[CC_PROJECT_MAX];
    char toast_text[CC_EVT_TEXT_MAX];
    uint32_t toast_until_ms;

    uint32_t last_activity_ms;    // last key press or wake-worthy event
    uint32_t last_key_ms;         // last key press (manual focus holds for CC_FOLLOW_IDLE_MS)
    bool key_seen;                // false until the first key press
    cc_backlight_t backlight;
} cc_view_t;

void cc_view_init(cc_view_t *v, uint32_t now_ms);
void cc_settings_defaults(cc_settings_t *s);
// Clamp out-of-range persisted values (e.g. from an older firmware).
void cc_settings_sanitize(cc_settings_t *s);

unsigned cc_view_on_snapshot(cc_view_t *v, const cc_snapshot_t *snap);
unsigned cc_view_on_event(cc_view_t *v, const cc_snapshot_t *snap, const cc_event_t *e,
                          uint32_t now_ms);
unsigned cc_view_on_key(cc_view_t *v, const cc_snapshot_t *snap, cc_settings_t *settings,
                        cc_key_t key, cc_press_t press, uint32_t now_ms);
// On the home page with no alert, move focus back to the top-priority session
// (index 0) once the user has not pressed a key for CC_FOLLOW_IDLE_MS.
unsigned cc_view_follow(cc_view_t *v, const cc_snapshot_t *snap, uint32_t now_ms);
// Wake the screen without a key (pairing prompt, connection change).
unsigned cc_view_wake(cc_view_t *v, uint32_t now_ms);
// Periodic housekeeping: toast expiry, confirmation timeout, backlight policy.
unsigned cc_view_tick(cc_view_t *v, const cc_settings_t *settings, uint32_t now_ms);

// Backlight percentage for the current state and settings.
uint8_t cc_view_backlight_percent(const cc_view_t *v, const cc_settings_t *settings);
// Screen timeout in seconds for a settings index (0 = never).
uint32_t cc_view_timeout_s(uint8_t index);

// ---------------------------------------------------------------------------
// Link status shown in the top bar and on the waiting screens.
// ---------------------------------------------------------------------------
typedef enum {
    CC_LINK_STARTING = 0,   // BLE stack not ready yet
    CC_LINK_OFF,            // BLE failed to start (e.g. NVS error)
    CC_LINK_ADVERTISING,    // waiting for the Mac to connect
    CC_LINK_SECURING,       // connected, encryption/pairing not finished
    CC_LINK_PAIRING,        // passkey on screen
    CC_LINK_WAIT_BRIDGE,    // secure link, no data from the bridge yet
    CC_LINK_LIVE,           // receiving snapshots
    CC_LINK_STALE,          // secure link but the bridge went quiet
} cc_link_ui_t;

typedef struct {
    bool ble_started;
    bool ble_failed;
    bool connected;
    bool secure;              // encrypted + authenticated (MITM-protected)
    bool passkey_active;
    uint32_t passkey;
    bool have_data;           // at least one line since the link became secure
    uint32_t last_rx_ms;
    bool pairing_blocked;     // a new Mac tried to pair without the pairing window
    bool pair_window;         // "Pair new Mac" window open
    uint8_t bonds;
} cc_link_info_t;

cc_link_ui_t cc_link_ui_state(const cc_link_info_t *link, uint32_t now_ms);
