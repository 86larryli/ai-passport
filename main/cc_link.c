// main/cc_link.c — see cc_link.h.
#include "cc_link.h"

#include <string.h>

#include "bsp_battery.h"
#include "cc_ble.h"
#include "cc_store.h"
#include "ccp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "cc_link";

// A full snapshot is a few KB; the bridge waits for each write response, so
// 4 KB of slack absorbs bursts while the link task is busy.
#define STREAM_BYTES 4096
#define BATTERY_PERIOD_MS 30000u
#define ERR_REPORT_PERIOD_MS 1000u

static StreamBufferHandle_t s_stream;
static ccp_line_asm_t s_asm;
static ccp_builder_t s_builder;
static cc_snapshot_t s_pending;     // filled between SNAP and END (~9 KB, static)
static const char *s_fw_version;
static bool s_battery_ok;
static uint32_t s_last_err_ms;
static uint32_t s_errors;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void send_line(const char *line, int len)
{
    if (len > 0) (void)cc_ble_send(line, (size_t)len);
}

static void on_line(char *line, size_t len, void *ctx)
{
    (void)ctx;
    const uint32_t now = now_ms();
    cc_store_note_rx(now);

    ccp_result_t r;
    ccp_handle_line(&s_builder, line, len, &r);
    char out[64];
    switch (r.kind) {
    case CCP_HELLO:
        ESP_LOGI(TAG, "bridge %s (protocol %u)", r.version, (unsigned)r.proto);
        cc_ble_note_bridge_subscribed();
        send_line(out, ccp_format_hi(out, sizeof(out), s_fw_version, cc_store_battery()));
        break;
    case CCP_CLOCK:
        cc_store_set_clock(r.unix_s, r.utc_offset_min, now);
        break;
    case CCP_COMMIT:
        cc_store_commit(&s_pending, now);
        send_line(out, ccp_format_ack(out, sizeof(out), r.seq));
        break;
    case CCP_EVENT:
        cc_store_push_event(&r.event);
        break;
    case CCP_ERROR:
        s_errors++;
        if (now - s_last_err_ms >= ERR_REPORT_PERIOD_MS) {
            s_last_err_ms = now;
            ESP_LOGW(TAG, "protocol error: %s (%u total)", r.error, (unsigned)s_errors);
            send_line(out, ccp_format_err(out, sizeof(out), r.error));
        }
        break;
    case CCP_PING:
    case CCP_UNKNOWN:
    case CCP_NONE:
    default:
        break;
    }
}

static void sample_battery(void)
{
    if (!s_battery_ok) {
        cc_store_set_battery(-1, -1);
        return;
    }
    cc_store_set_battery(bsp_battery_soc(), bsp_battery_mv());
}

static void link_task(void *arg)
{
    (void)arg;
    uint8_t chunk[256];
    uint32_t generation = cc_ble_generation();
    uint32_t last_battery = now_ms();
    sample_battery();
    for (;;) {
        size_t n = xStreamBufferReceive(s_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(1000));
        uint32_t current = cc_ble_generation();
        if (current != generation) {
            // New connection or disconnect: never let a half-received snapshot
            // or line continue across links.
            generation = current;
            ccp_line_asm_reset(&s_asm);
            ccp_builder_reset(&s_builder);
        }
        if (n > 0) ccp_line_asm_feed(&s_asm, chunk, n, on_line, NULL);
        if (now_ms() - last_battery >= BATTERY_PERIOD_MS) {
            last_battery = now_ms();
            sample_battery();
        }
    }
}

bool cc_link_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (!s_stream || xStreamBufferSpacesAvailable(s_stream) < len) return false;
    return xStreamBufferSend(s_stream, data, len, 0) == len;
}

esp_err_t cc_link_start(const char *fw_version, bool battery_ok)
{
    s_fw_version = fw_version;
    s_battery_ok = battery_ok;
    ccp_line_asm_reset(&s_asm);
    ccp_builder_init(&s_builder, &s_pending);
    s_stream = xStreamBufferCreate(STREAM_BYTES, 1);
    if (!s_stream) return ESP_ERR_NO_MEM;
    if (xTaskCreate(link_task, "cc_link", 4096, NULL, 5, NULL) != pdPASS) {
        vStreamBufferDelete(s_stream);
        s_stream = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
