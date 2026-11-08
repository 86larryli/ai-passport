// main/cc_link.h — protocol task between the BLE transport and the store.
//
// BLE RX bytes (NimBLE host task) are queued in a FreeRTOS stream buffer; the
// link task assembles lines, parses them with ccp.c, commits snapshots and
// events to cc_store, answers HELLO/END, and samples the battery every 30 s.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// battery_ok: whether bsp_battery_init() succeeded (otherwise -1 is reported).
esp_err_t cc_link_start(const char *fw_version, bool battery_ok);

// cc_ble_rx_fn implementation: copy into the stream buffer without blocking.
bool cc_link_rx(const uint8_t *data, size_t len, void *ctx);
