// main/cc_sound.c — see cc_sound.h.
#include "cc_sound.h"

#include <stdbool.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "cc_sound";

#define CHUNK_SAMPLES 320          // 20 ms at 16 kHz
#define TAIL_SILENCE_CHUNKS 3      // flush the DMA ring so the last note is not cut
#define IDLE_BEFORE_SLEEP_MS 1500
#define VOLUME_PCT 70
#define AMPLITUDE 12000            // about -9 dBFS: audible without clipping the amp

static QueueHandle_t s_queue;

static void play(cc_chime_t chime, bool *awake)
{
    if (!*awake) {
        esp_err_t err = bsp_audio_wake();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "codec wake failed: %s", esp_err_to_name(err));
            return;
        }
        *awake = true;
    }
    if (bsp_audio_set_format(CC_TONE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "codec format failed");
        return;
    }
    int16_t buf[CHUNK_SAMPLES];
    size_t offset = 0;
    for (;;) {
        size_t n = cc_tone_render(chime, offset, buf, CHUNK_SAMPLES, AMPLITUDE);
        if (n == 0) break;
        offset += n;
        if (bsp_audio_write(buf, n * sizeof(int16_t)) != ESP_OK) return;
    }
    for (size_t i = 0; i < CHUNK_SAMPLES; i++) buf[i] = 0;
    for (int i = 0; i < TAIL_SILENCE_CHUNKS; i++) {
        if (bsp_audio_write(buf, sizeof(buf)) != ESP_OK) return;
    }
}

static void sound_task(void *arg)
{
    (void)arg;
    bool awake = true;   // bsp_audio_init() leaves the codec running
    // Suspend right away; the first chime wakes it.
    if (bsp_audio_sleep() == ESP_OK) awake = false;
    for (;;) {
        cc_chime_t chime;
        TickType_t wait = awake ? pdMS_TO_TICKS(IDLE_BEFORE_SLEEP_MS) : portMAX_DELAY;
        if (xQueueReceive(s_queue, &chime, wait) == pdTRUE) {
            play(chime, &awake);
        } else if (awake) {
            esp_err_t err = bsp_audio_sleep();
            if (err == ESP_OK) awake = false;
            else ESP_LOGW(TAG, "codec sleep failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t cc_sound_start(void)
{
    bsp_audio_set_volume(VOLUME_PCT);
    s_queue = xQueueCreate(1, sizeof(cc_chime_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(sound_task, "cc_sound", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void cc_sound_play(cc_chime_t chime)
{
    if (s_queue) (void)xQueueSend(s_queue, &chime, 0);
}
