// Host tests for main/cc_tone.c (chime synthesis).
#include "cc_tone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

int main(void)
{
    const int16_t amp = 12000;
    for (int c = 0; c < CC_CHIME_COUNT; c++) {
        size_t len = cc_tone_length((cc_chime_t)c);
        CHECK(len > CC_TONE_RATE / 10 && len < CC_TONE_RATE);   // 0.1 .. 1 s
        int16_t *whole = calloc(len + 8, sizeof(int16_t));
        int16_t *chunked = calloc(len + 8, sizeof(int16_t));
        CHECK(cc_tone_render((cc_chime_t)c, 0, whole, len + 8, amp) == len);

        // Chunked rendering (as the audio worker does) matches one-shot output.
        size_t off = 0;
        size_t n;
        while ((n = cc_tone_render((cc_chime_t)c, off, chunked + off, 317, amp)) > 0) off += n;
        CHECK(off == len);
        CHECK(memcmp(whole, chunked, len * sizeof(int16_t)) == 0);

        int peak = 0;
        for (size_t i = 0; i < len; i++) {
            int v = abs(whole[i]);
            if (v > peak) peak = v;
        }
        CHECK(peak <= amp && peak > amp / 2);
        // Notes start and end near silence (no clicks).
        CHECK(abs(whole[0]) < 200);
        CHECK(abs(whole[len - 1]) < 400);
        free(whole);
        free(chunked);
    }
    int16_t buf[4];
    CHECK(cc_tone_render(CC_CHIME_COUNT, 0, buf, 4, amp) == 0);
    CHECK(cc_tone_length(CC_CHIME_COUNT) == 0);
    CHECK(cc_tone_render(CC_CHIME_DONE, cc_tone_length(CC_CHIME_DONE), buf, 4, amp) == 0);
    if (failures) {
        fprintf(stderr, "test_cc_tone: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("test_cc_tone: PASS\n");
    return EXIT_SUCCESS;
}
