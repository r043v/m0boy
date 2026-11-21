/* Audio output (META: DAC on PA02, meta_audio.cpp; Pokitto: R2R DAC, pokitto_audio.c). */
#ifndef META_AUDIO_H
#define META_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void m0_audio_init(void);
#ifdef META_AUDIO_PUSH_RATE
/* Queue one unsigned PCM sample, 128 = silence. */
void m0_audio_push(uint8_t sample);
#endif
void m0_audio_frame(void);  /* once per emulated frame */
void m0_audio_idle(void);   /* called while waiting for frame pacing */
void m0_audio_set_volume(uint8_t v); /* 0..8, 4 = default */
uint8_t m0_audio_volume(void);
/* The output path is META_AUDIO_DMA, chosen at build time (0 the TC4
 * interrupt writes each ring entry to the DAC, 1 a DMAC beat copies it; the
 * META only, the Pokitto and ESPboy have their own drivers).  No run-time
 * switching since 0.10.29. */
void m0_audio_stop(void);   /* timer and interrupt off (before the loader) */
/* The option menu: the DAC to mid-scale and the sample source off, so the
 * game's sound actually stops while the menu is open (m0_audio_stop's
 * cheaper sibling: one call to pause, one to resume). */
void m0_audio_pause(int on);

/* Samples of one DMG frame (70224 cycles at 4194304 Hz) at `rate` Hz, the
 * remainder carried over in *frac (shared by the consoles' drivers).  The
 * quotient and the remainder are computed once per rate: the Cortex-M0+
 * divides in software, and this runs once a frame. */
static inline unsigned m0_audio_frame_samples(uint32_t rate, uint32_t *frac) {
    static uint32_t cached_rate, whole, part;
    if (rate != cached_rate) {
        cached_rate = rate;
        whole = rate * 70224u / 4194304u;
        part = rate * 70224u % 4194304u;
    }
    *frac += part;
    unsigned n = whole;
    if (*frac >= 4194304u) { *frac -= 4194304u; n++; }
    return n;
}

#ifdef __cplusplus
}
#endif

#endif
