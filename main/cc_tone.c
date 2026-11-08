// main/cc_tone.c — see cc_tone.h.
#include "cc_tone.h"

#define MAX_NOTES 6
#define ATTACK_SAMPLES (CC_TONE_RATE * 4 / 1000)

typedef struct {
    uint8_t count;
    cc_note_t notes[MAX_NOTES];
} chime_def_t;

// Note frequencies: E6 1319, A6 1760, C6 1047, G6 1568, G5 784, A4 440, F4 349.
static const chime_def_t CHIMES[CC_CHIME_COUNT] = {
    [CC_CHIME_PERM] = { 5, { { 1319, 90 }, { 1760, 140 }, { 0, 110 }, { 1319, 90 }, { 1760, 160 } } },
    [CC_CHIME_ASK] = { 3, { { 1047, 80 }, { 1319, 80 }, { 1568, 180 } } },
    [CC_CHIME_DONE] = { 2, { { 784, 120 }, { 1047, 240 } } },
    [CC_CHIME_ERR] = { 3, { { 440, 180 }, { 0, 60 }, { 349, 260 } } },
};

// One full sine cycle in 64 Q15 steps; enough for short alert tones.
static const int16_t SINE[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898,
    30273, 31356, 32137, 32609, 32767, 32609, 32137, 31356, 30273, 28898, 27245,
    25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212, 0, -3212, -6393,
    -9512, -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273,
    -31356, -32137, -32609, -32767, -32609, -32137, -31356, -30273, -28898, -27245,
    -25329, -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212,
};

static size_t note_samples(const cc_note_t *note)
{
    return (size_t)note->ms * CC_TONE_RATE / 1000u;
}

size_t cc_tone_length(cc_chime_t chime)
{
    if ((unsigned)chime >= CC_CHIME_COUNT) return 0;
    size_t total = 0;
    for (int i = 0; i < CHIMES[chime].count; i++) total += note_samples(&CHIMES[chime].notes[i]);
    return total;
}

static int16_t sample_at(const cc_note_t *note, size_t i, size_t n, int16_t amplitude)
{
    if (note->freq_hz == 0) return 0;
    // Phase in 1/65536 of a cycle; 64-entry table indexed by the top 6 bits.
    uint32_t phase = (uint32_t)(((uint64_t)i * note->freq_hz * 65536u) / CC_TONE_RATE);
    int32_t s = SINE[(phase >> 10) & 63];
    int32_t env = 1024;
    if (i < ATTACK_SAMPLES) {
        env = (int32_t)(i * 1024 / ATTACK_SAMPLES);
    }
    size_t release_start = n * 6 / 10;
    if (i >= release_start) {
        int32_t rel = (int32_t)((n - i) * 1024 / (n - release_start));
        if (rel < env) env = rel;
    }
    return (int16_t)((s * amplitude / 32767) * env / 1024);
}

size_t cc_tone_render(cc_chime_t chime, size_t offset, int16_t *out, size_t max,
                      int16_t amplitude)
{
    if ((unsigned)chime >= CC_CHIME_COUNT || amplitude < 0) return 0;
    const chime_def_t *def = &CHIMES[chime];
    size_t written = 0;
    size_t base = 0;
    for (int k = 0; k < def->count && written < max; k++) {
        const cc_note_t *note = &def->notes[k];
        size_t n = note_samples(note);
        size_t pos = offset + written;
        if (pos < base + n) {
            for (size_t i = pos - base; i < n && written < max; i++) {
                out[written++] = sample_at(note, i, n, amplitude);
            }
        }
        base += n;
    }
    return written;
}
