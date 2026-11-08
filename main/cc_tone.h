// main/cc_tone.h — synthesized alert chimes (pure C, host-tested).
//
// Each chime is a short list of notes rendered as 16-bit mono PCM with a sine
// table, a 4 ms attack, and a linear release so notes start and stop without
// clicks. Rendering is chunked so the audio worker can stream it into
// bsp_audio_write() with a small stack buffer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CC_TONE_RATE 16000u

typedef enum {
    CC_CHIME_PERM = 0,   // rising two-note call, twice: "needs your approval"
    CC_CHIME_ASK,        // three-note arpeggio: "question for you"
    CC_CHIME_DONE,       // soft two-note ding: "finished"
    CC_CHIME_ERR,        // low falling pair: "stopped on an error"
    CC_CHIME_COUNT
} cc_chime_t;

typedef struct {
    uint16_t freq_hz;    // 0 = rest
    uint16_t ms;
} cc_note_t;

// Total length of a chime in samples.
size_t cc_tone_length(cc_chime_t chime);

// Render samples [offset, offset + max) of the chime into out. Returns the
// number written (0 at the end). amplitude is the peak (0..32767).
size_t cc_tone_render(cc_chime_t chime, size_t offset, int16_t *out, size_t max,
                      int16_t amplitude);
