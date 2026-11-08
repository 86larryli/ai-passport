// main/cc_settings.h — persist user settings (sound, brightness, screen timeout) in NVS.
#pragma once

#include "cc_view.h"
#include "esp_err.h"

// Loads saved settings or defaults. Never erases NVS.
void cc_settings_load(cc_settings_t *out);
// Writes only when the value changed since the last load/save.
esp_err_t cc_settings_save(const cc_settings_t *settings);
