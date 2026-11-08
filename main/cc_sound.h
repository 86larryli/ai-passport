// main/cc_sound.h — alert chime worker on top of the ES8311 BSP.
//
// cc_sound_play() only queues a request; a dedicated task performs the
// blocking PCM writes. Between chimes the codec is suspended with
// bsp_audio_sleep() to save power and resumed with bsp_audio_wake().
#pragma once

#include "cc_tone.h"
#include "esp_err.h"

// Call after bsp_audio_init() succeeded.
esp_err_t cc_sound_start(void);
// Non-blocking; drops the request when one is already waiting.
void cc_sound_play(cc_chime_t chime);
