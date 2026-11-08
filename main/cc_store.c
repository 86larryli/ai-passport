// main/cc_store.c — see cc_store.h.
#include "cc_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define EVENT_RING 4

static SemaphoreHandle_t s_mutex;
static cc_snapshot_t s_snapshot;          // ~9 KB, static: no heap churn
static uint32_t s_version;
static uint32_t s_commit_ms;
static cc_event_t s_events[EVENT_RING];
static uint8_t s_event_head;
static uint8_t s_event_count;
static cc_link_info_t s_link;
static uint32_t s_link_version;
static bool s_clock_known;
static int64_t s_clock_unix;
static int32_t s_clock_offset;
static uint32_t s_clock_ms;
static int s_battery = -1;
static char s_name[24] = "CC-Companion";

// Recursive: the UI holds the lock for a whole render and calls the getters below.
static void lock(void)
{
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

esp_err_t cc_store_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateRecursiveMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void cc_store_commit(const cc_snapshot_t *snap, uint32_t now_ms)
{
    lock();
    memcpy(&s_snapshot, snap, sizeof(s_snapshot));
    s_version++;
    s_commit_ms = now_ms;
    unlock();
}

const cc_snapshot_t *cc_store_lock(void)
{
    lock();
    return &s_snapshot;
}

void cc_store_unlock(void)
{
    unlock();
}

uint32_t cc_store_version(void)
{
    lock();
    uint32_t v = s_version;
    unlock();
    return v;
}

uint32_t cc_store_commit_ms(void)
{
    lock();
    uint32_t v = s_commit_ms;
    unlock();
    return v;
}

void cc_store_push_event(const cc_event_t *event)
{
    lock();
    if (s_event_count == EVENT_RING) {
        s_event_head = (uint8_t)((s_event_head + 1) % EVENT_RING);
        s_event_count--;
    }
    s_events[(s_event_head + s_event_count) % EVENT_RING] = *event;
    s_event_count++;
    unlock();
}

bool cc_store_pop_event(cc_event_t *out)
{
    lock();
    bool have = s_event_count > 0;
    if (have) {
        *out = s_events[s_event_head];
        s_event_head = (uint8_t)((s_event_head + 1) % EVENT_RING);
        s_event_count--;
    }
    unlock();
    return have;
}

void cc_store_link_update(cc_link_update_fn fn, void *arg)
{
    lock();
    fn(&s_link, arg);
    s_link_version++;
    unlock();
}

void cc_store_link_get(cc_link_info_t *out)
{
    lock();
    *out = s_link;
    unlock();
}

uint32_t cc_store_link_version(void)
{
    lock();
    uint32_t v = s_link_version;
    unlock();
    return v;
}

void cc_store_note_rx(uint32_t now_ms)
{
    lock();
    if (!s_link.have_data) s_link_version++;
    s_link.have_data = true;
    s_link.last_rx_ms = now_ms;
    unlock();
}

void cc_store_set_clock(int64_t unix_s, int32_t utc_offset_min, uint32_t now_ms)
{
    lock();
    s_clock_known = true;
    s_clock_unix = unix_s;
    s_clock_offset = utc_offset_min;
    s_clock_ms = now_ms;
    unlock();
}

bool cc_store_clock(int64_t *unix_s, int32_t *utc_offset_min, uint32_t now_ms)
{
    lock();
    bool known = s_clock_known;
    if (known) {
        *unix_s = s_clock_unix + (int64_t)((now_ms - s_clock_ms) / 1000u);
        *utc_offset_min = s_clock_offset;
    }
    unlock();
    return known;
}

void cc_store_set_battery(int percent, int millivolts)
{
    (void)millivolts;
    lock();
    s_battery = percent;
    unlock();
}

int cc_store_battery(void)
{
    lock();
    int v = s_battery;
    unlock();
    return v;
}

void cc_store_set_device_name(const char *name)
{
    lock();
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    unlock();
}

void cc_store_device_name(char *out, size_t cap)
{
    lock();
    strncpy(out, s_name, cap - 1);
    out[cap - 1] = '\0';
    unlock();
}
