// main/cc_ui.h — LVGL screens of the Claude Code Companion.
//
// Pages (all created once at boot and shown/hidden, so LVGL pool use is fixed):
//   top bar   link icon (left), clock (center), battery (right, top-right rule)
//   home      focused session card + the other sessions + hints
//   details   main activity, pending approval/question, last message, subagents
//   settings  sound, brightness, screen timeout, pair new Mac, forget Macs, about
//   waiting   shown while no live bridge (advertising, securing, waiting, error)
//   pairing   six-digit passkey to type on the Mac
//   alert     full-screen card when a session needs approval or an answer
//   toast     short banner when a session finishes or fails
//
// Threading: everything runs in the LVGL task (an lv_timer polls cc_store every
// 100 ms). cc_ui_handle_key() must be called with bsp_lvgl_lock() held; it
// returns cc_action_t bits for work the caller does after unlocking (NVS
// writes, BLE bond changes, sound preview).
#pragma once

#include <stdbool.h>

#include "cc_view.h"
#include "esp_err.h"

esp_err_t cc_ui_init(cc_settings_t *settings, const char *fw_version, bool sound_ok);
unsigned cc_ui_handle_key(cc_key_t key, cc_press_t press);
// Record that the pairing window was opened (for the countdown in settings).
void cc_ui_note_pair_window(uint32_t seconds);
