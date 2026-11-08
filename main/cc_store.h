// main/cc_store.h — state shared between the BLE/link tasks and the UI.
//
// Ownership and threading:
//   - The link task commits complete snapshots and pushes one-shot events.
//   - NimBLE host callbacks update link status (connection, encryption, passkey).
//   - The UI (LVGL task) reads everything.
// One recursive FreeRTOS mutex guards all of it; every critical section is a
// bounded copy, except that the UI holds it for one render pass.
// Lock order is LVGL lock -> store lock; nothing here takes the LVGL lock.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cc_types.h"
#include "cc_view.h"
#include "esp_err.h"

esp_err_t cc_store_init(void);

// Snapshot. lock() returns the committed snapshot (valid until unlock).
void cc_store_commit(const cc_snapshot_t *snap, uint32_t now_ms);
const cc_snapshot_t *cc_store_lock(void);
void cc_store_unlock(void);
uint32_t cc_store_version(void);          // increments on every commit
uint32_t cc_store_commit_ms(void);        // when the current snapshot arrived

// One-shot events (bounded ring; the oldest is dropped when full).
void cc_store_push_event(const cc_event_t *event);
bool cc_store_pop_event(cc_event_t *out);

// Link status. update() applies fn under the lock and bumps the link version.
typedef void (*cc_link_update_fn)(cc_link_info_t *link, void *arg);
void cc_store_link_update(cc_link_update_fn fn, void *arg);
void cc_store_link_get(cc_link_info_t *out);
uint32_t cc_store_link_version(void);
void cc_store_note_rx(uint32_t now_ms);   // a line arrived from the bridge

// Wall clock from the bridge (CLOCK record). Returns false until known.
void cc_store_set_clock(int64_t unix_s, int32_t utc_offset_min, uint32_t now_ms);
bool cc_store_clock(int64_t *unix_s, int32_t *utc_offset_min, uint32_t now_ms);

// Battery level sampled by the link task (-1 = unavailable).
void cc_store_set_battery(int percent, int millivolts);
int cc_store_battery(void);

// Device name used for advertising ("CC-Companion-XXXX").
void cc_store_set_device_name(const char *name);
void cc_store_device_name(char *out, size_t cap);
