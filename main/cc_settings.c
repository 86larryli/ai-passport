// main/cc_settings.c — see cc_settings.h.
#include "cc_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "cc_settings";
static const char *NS = "cc_comp";
static const char *KEY = "settings";
#define SETTINGS_VERSION 1

typedef struct {
    uint8_t version;
    cc_settings_t value;
} stored_t;

static cc_settings_t s_saved;
static bool s_loaded;

void cc_settings_load(cc_settings_t *out)
{
    cc_settings_defaults(out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        stored_t stored;
        size_t size = sizeof(stored);
        if (nvs_get_blob(h, KEY, &stored, &size) == ESP_OK && size == sizeof(stored) &&
            stored.version == SETTINGS_VERSION) {
            *out = stored.value;
        }
        nvs_close(h);
    }
    cc_settings_sanitize(out);
    s_saved = *out;
    s_loaded = true;
}

esp_err_t cc_settings_save(const cc_settings_t *settings)
{
    if (s_loaded && memcmp(&s_saved, settings, sizeof(s_saved)) == 0) return ESP_OK;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }
    stored_t stored = { .version = SETTINGS_VERSION, .value = *settings };
    err = nvs_set_blob(h, KEY, &stored, sizeof(stored));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_saved = *settings;
        s_loaded = true;
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}
