// main/main.c — Claude Code Companion for the FoloToy AI Passport.
//
// Shows the live state of every Claude Code CLI session on a Mac (main agent,
// subagents, pending approvals and questions, finished turns). A bridge on the
// Mac collects Claude Code hook events and sends snapshots over BLE; see
// docs/claude-code-companion/README.md.
//
// Init order: NVS -> store/settings -> shared I2C -> display/LVGL (hard
// dependency) -> audio/battery (optional) -> UI -> buttons -> link task -> BLE.
// Button callbacks run on the shared esp_timer task and only enqueue; the
// input task handles keys under the LVGL lock and performs slow follow-up work
// (NVS writes, bond deletion, chimes) after releasing it.
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "cc_ble.h"
#include "cc_link.h"
#include "cc_settings.h"
#include "cc_sound.h"
#include "cc_store.h"
#include "cc_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define CC_FW_VERSION "0.1.0"
#define PAIR_WINDOW_S 60
#define INPUT_QUEUE_DEPTH 8

static const char *TAG = "cc_main";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_t;

static QueueHandle_t s_input;
static cc_settings_t s_settings;     // owned by the UI; read here under the LVGL lock
static bool s_sound_ok;

// Runs on the button component's esp_timer task: enqueue only.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Only clicks and long presses express intent; PRESS/RELEASE edges are ignored
    // (see docs/reference/shinku-chen/three-key-input-event-semantics.md).
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE && ev != BSP_BTN_LONG) return;
    if (!s_input) return;
    const input_t in = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input, &in, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    input_t in;
    for (;;) {
        if (xQueueReceive(s_input, &in, portMAX_DELAY) != pdTRUE) continue;
        const cc_key_t key = in.btn == BSP_BTN_UP ? CC_KEY_UP : (in.btn == BSP_BTN_DOWN ? CC_KEY_DOWN : CC_KEY_OK);
        const cc_press_t press = in.event == BSP_BTN_LONG ? CC_PRESS_LONG
                               : (in.event == BSP_BTN_DOUBLE ? CC_PRESS_DOUBLE : CC_PRESS_CLICK);
        if (!bsp_lvgl_lock(500)) continue;
        const unsigned act = cc_ui_handle_key(key, press);
        const cc_settings_t settings = s_settings;
        if (act & CC_ACT_PAIR_WINDOW) cc_ui_note_pair_window(PAIR_WINDOW_S);
        bsp_lvgl_unlock();

        if (act & CC_ACT_SAVE_SETTINGS) (void)cc_settings_save(&settings);
        if ((act & CC_ACT_SOUND_PREVIEW) && s_sound_ok) cc_sound_play(CC_CHIME_DONE);
        if (act & CC_ACT_PAIR_WINDOW) cc_ble_open_pair_window(PAIR_WINDOW_S);
        if (act & CC_ACT_FORGET_BONDS) (void)cc_ble_forget_all();
    }
}

static void mark_ble_failed(cc_link_info_t *link, void *arg)
{
    (void)arg;
    link->ble_failed = true;
}

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "%s: free heap %u, largest block %u, minimum ever %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Claude Code Companion %s", CC_FW_VERSION);

    // Never erase NVS automatically: it holds BLE bonds and settings.
    esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) ESP_LOGE(TAG, "NVS init failed (%s); BLE disabled, NVS not erased",
                                esp_err_to_name(nvs));
    if (cc_store_init() != ESP_OK) {
        ESP_LOGE(TAG, "out of memory creating the store");
        return;
    }
    if (nvs == ESP_OK) cc_settings_load(&s_settings);
    else cc_settings_defaults(&s_settings);

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    const bool audio_ok = bsp_audio_init() == ESP_OK;
    const bool battery_ok = bsp_battery_init() == ESP_OK;
    s_sound_ok = audio_ok && cc_sound_start() == ESP_OK;
    ESP_LOGI(TAG, "audio=%d battery=%d sound=%d", audio_ok, battery_ok, s_sound_ok);

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "LVGL lock timeout during UI init");
        return;
    }
    esp_err_t ui = cc_ui_init(&s_settings, CC_FW_VERSION, s_sound_ok);
    bsp_lvgl_unlock();
    if (ui != ESP_OK) ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(ui));

    s_input = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_t));
    if (!s_input || xTaskCreate(input_task, "cc_input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "input task creation failed");
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button init failed");
    }

    esp_err_t link = cc_link_start(CC_FW_VERSION, battery_ok);
    esp_err_t ble = ESP_ERR_INVALID_STATE;
    if (link == ESP_OK && nvs == ESP_OK) ble = cc_ble_start(cc_link_rx, NULL);
    if (ble != ESP_OK) {
        ESP_LOGE(TAG, "BLE unavailable: link=%s ble=%s", esp_err_to_name(link), esp_err_to_name(ble));
        cc_store_link_update(mark_ble_failed, NULL);
    }
    log_memory("ready");
}
