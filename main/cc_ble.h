// main/cc_ble.h — NimBLE GATT peripheral that carries CCP lines from the Mac.
//
// One primary service with two characteristics (UUIDs in cc_ble.c, mirrored in
// tools/cc_companion/ccp.py):
//   RX  write / write-without-response   bridge -> device CCP bytes
//   TX  notify                           device -> bridge CCP lines
// Both require an encrypted, MITM-authenticated, bonded link (LE Secure
// Connections, passkey shown on the device). Once a Mac is bonded, a different
// central can only pair during the window opened by cc_ble_open_pair_window()
// (Settings > Pair new Mac), so a nearby stranger cannot attach silently.
//
// Link status is published through cc_store_link_update(). RX bytes are passed
// to the callback on the NimBLE host task; it must only copy them and return.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Return false when the bytes could not be queued; the write is then rejected
// with an ATT error so the bridge sees the failure and resends.
typedef bool (*cc_ble_rx_fn)(const uint8_t *data, size_t len, void *ctx);

// Requires nvs_flash_init() to have succeeded (bonds and PHY data live in NVS).
esp_err_t cc_ble_start(cc_ble_rx_fn rx, void *ctx);

// Notify a line to the bridge. Fails with ESP_ERR_INVALID_STATE unless the link
// is secure and the bridge subscribed. Safe to call from any task.
esp_err_t cc_ble_send(const char *data, size_t len);

// The bridge sends HELLO only after its TX subscription succeeded, so a HELLO
// proves the subscription even when NimBLE did not report it (bonded reconnects
// restore the CCCD as part of encryption, which can race our security request).
void cc_ble_note_bridge_subscribed(void);

// Incremented on every connect and disconnect; the link task resets its parser
// when this changes so a half-received snapshot never spans two connections.
uint32_t cc_ble_generation(void);

// Allow a new Mac to pair for the next `seconds` (bonded Macs reconnect anytime).
void cc_ble_open_pair_window(uint32_t seconds);

// Disconnect and delete every stored bond. Blocks up to ~1.5 s; call it from a
// worker task (not the NimBLE host task, not with the LVGL lock held).
esp_err_t cc_ble_forget_all(void);
