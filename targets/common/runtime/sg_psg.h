/* SN76489 sample generation, shared by the SG-1000's SN76489AN and the PSG
 * integrated in the Sega VDP (Game Gear, Master System).
 *
 * This is an implementation header, included by sg_audio.c and gg_psg.c,
 * which include their console header and set SG_SYSTEM_GG first, as sg_gen.h
 * does for the generated code.  Three square waves, 2 dB attenuation; the
 * two chips differ in
 *   - the noise LFSR: TI, 15 stages plus output delay; Sega, 16 bits with
 *     the white-noise taps on bits 0 and 3;
 *   - a tone divider of 0 or 1: TI (0 acts as 0x400) counts it out, Sega
 *     holds the output high (samples are played back through the volume);
 *   - the noise divider floor: TI 2048 when the tone divider is 0, Sega 2.
 * Samples are produced up to machine.cyc before every PSG write and at frame
 * end, so short notes and volume changes keep their position inside a frame. */
#ifndef SG_PSG_H
#define SG_PSG_H
#if SG_SYSTEM_GG
#include "gg.h"
typedef gg_t sg_psg_t;
#define SG_PSG(name) gg_##name
#else
#include "sg1000.h"
typedef sg1000_t sg_psg_t;
#define SG_PSG(name) sg1000_##name
#endif

#include "m0_divide.h"

#define SG_CLOCK 3579545u
static const uint8_t level[16] = { 30, 24, 19, 15, 12, 10, 8, 6, 5, 4, 3, 2, 2, 1, 1, 0 };

void SG_PSG(audio_rate)(unsigned rate) {
    sg_psg_t *m = &machine;
    /* 1000-48000 Hz (0: off): at most 223 PSG ticks per sample (uint8_t) */
    m->audio_rate = !rate ? 0 : rate > 48000 ? 48000 : rate < 1000 ? 1000 : rate;
    /* PSG clock divider 16: whole ticks per sample plus a remainder */
    m->audio_div = m->audio_rate * 16;
    m->audio_tick_int = m->audio_div ? SG_CLOCK / m->audio_div : 0;
    m->audio_tick_frac = m->audio_div ? SG_CLOCK % m->audio_div : 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = m->cyc;
    m->audio_head = m->audio_tail = 0;
}

/* Samples are produced in blocks, one channel at a time over the block (few
 * live values: the M0+ keeps them in registers), then mixed.  The PSG ticks
 * (clock / 16) of each sample come from one Bresenham pass per block, shared
 * by the four channels; a silent channel only advances its counter (one
 * division, in steps of the quotient's bits without a divider).  The result equals producing the channels sample by sample. */
#define SG_AUDIO_BLOCK 128

/* count after `total` ticks of a channel of this period, and the number of
 * times it reloaded */
static inline uint32_t skip_ticks(int32_t *count, int32_t period, uint32_t total) {
    int32_t c = *count - (int32_t)total;
    if (c > 0) { *count = c; return 0; }
    uint32_t rest;
    const uint32_t k = m0_udivmod((uint32_t)(-c), (uint32_t)period, &rest) + 1;
    *count = (int32_t)((uint32_t)period - rest);
    return k;
}

/* first: store into mix instead of adding (the first channel of a block) */
static void tone_block(sg_psg_t *s, unsigned ch, int16_t *mix, const uint8_t *ticks, unsigned n, uint32_t total, int first) {
#if SG_SYSTEM_GG
    const int32_t period = s->psg_tone[ch];
#else
    /* A zero tone divider acts as 0x400 on the TI part. */
    const int32_t period = s->psg_tone[ch] ? s->psg_tone[ch] : 1024;
#endif
    const int amp = level[s->psg_volume[ch]];
    int32_t count = s->audio_count[ch];
    unsigned out = s->audio_output[ch];
#if SG_SYSTEM_GG
    if (period <= 1) { /* held high */
        out = 1;
        for (unsigned i = 0; i < n; i++) mix[i] = (int16_t)(first ? 128 + amp : mix[i] + amp);
    } else
#endif
    if (!amp) {
        out ^= skip_ticks(&count, period, total) & 1;
        if (first) for (unsigned i = 0; i < n; i += 2) *(uint32_t *)(mix + i) = 0x00800080u; /* n even or mix padded */
    } else {
        int v = out ? amp : -amp;
        for (unsigned i = 0; i < n; i++) {
            count -= ticks[i];
            if (count <= 0) {
                do { count += period; out ^= 1; } while (count <= 0);
                v = out ? amp : -amp;
            }
            mix[i] = (int16_t)(first ? 128 + v : mix[i] + v);
        }
    }
    s->audio_count[ch] = count;
    s->audio_output[ch] = (uint8_t)out;
}

static inline uint32_t lfsr_step(uint32_t lfsr, unsigned white) {
#if SG_SYSTEM_GG
    const unsigned feedback = white ? (lfsr ^ (lfsr >> 3)) & 1 : lfsr & 1;
    return (lfsr >> 1) | (feedback << 15);
#else
    const unsigned feedback = white ? ((lfsr >> 2) ^ (lfsr >> 3)) & 1 : (lfsr >> 2) & 1;
    return (lfsr >> 1) | (feedback << 16);
#endif
}

static void noise_block(sg_psg_t *s, int16_t *mix, const uint8_t *ticks, unsigned n, uint32_t total) {
    int32_t period = (s->psg_noise & 3) == 3 ? s->psg_tone[2] * 2 : 32 << (s->psg_noise & 3);
#if SG_SYSTEM_GG
    if (period < 2) period = 2;
#else
    if (!period) period = 2048;
#endif
    const int amp = level[s->psg_volume[3]];
    const unsigned white = s->psg_noise & 4;
    int32_t count = s->audio_count[3];
    uint32_t lfsr = s->audio_lfsr;
    if (!amp) {
        for (uint32_t k = skip_ticks(&count, period, total); k; k--) lfsr = lfsr_step(lfsr, white);
    } else {
        int v = (lfsr & 1) ? amp : -amp;
        for (unsigned i = 0; i < n; i++) {
            count -= ticks[i];
            if (count <= 0) {
                do { count += period; lfsr = lfsr_step(lfsr, white); } while (count <= 0);
                v = (lfsr & 1) ? amp : -amp;
            }
            mix[i] = (int16_t)(mix[i] + v);
        }
    }
    s->audio_count[3] = count;
    s->audio_lfsr = lfsr;
    s->audio_output[3] = (uint8_t)(lfsr & 1);
}

static void produce(sg_psg_t *s, unsigned n) {
    int16_t mix[SG_AUDIO_BLOCK] __attribute__((aligned(4)));
    uint8_t ticks[SG_AUDIO_BLOCK];
    const uint32_t tint = s->audio_tick_int, tfrac = s->audio_tick_frac, div = s->audio_div;
    while (n) {
        unsigned k = n > SG_AUDIO_BLOCK ? SG_AUDIO_BLOCK : n;
        n -= k;
        uint32_t phase = s->audio_tick_phase, total = 0;
        for (unsigned i = 0; i < k; i++) {
            uint32_t t = tint;
            phase += tfrac;
            if (phase >= div) { phase -= div; t++; }
            ticks[i] = (uint8_t)t;
            total += t;
        }
        s->audio_tick_phase = phase;
        for (unsigned ch = 0; ch < 3; ch++) tone_block(s, ch, mix, ticks, k, total, ch == 0);
        noise_block(s, mix, ticks, k, total);
        unsigned head = s->audio_head, tail = s->audio_tail;
        for (unsigned i = 0; i < k; i++) {
            unsigned next = (head + 1) & 511;
            if (next == tail) break;
            s->audio_buffer[head] = (uint8_t)mix[i];
            head = next;
        }
        s->audio_head = (uint16_t)head;
    }
}

void SG_PSG(audio_catchup)(void) {
    sg_psg_t *s = &machine;
    if (!s->audio_rate) return;
    uint32_t elapsed = s->cyc - s->audio_cyc;
    s->audio_cyc = s->cyc;
    unsigned n = 0;
    while (elapsed) { /* chunks keep phase + elapsed * rate below 2^32 */
        uint32_t chunk = elapsed > 65536 ? 65536 : elapsed;
        elapsed -= chunk;
        const uint32_t phase = s->audio_phase + chunk * s->audio_rate;
#if M0_DIVIDE32
        n += phase / SG_CLOCK;
        s->audio_phase = phase % SG_CLOCK;
#else
        uint32_t rest = phase; /* one turn per sample the block then produces */
        while (rest >= SG_CLOCK) { rest -= SG_CLOCK; n++; }
        s->audio_phase = rest;
#endif
    }
    if (n) produce(s, n);
}

void SG_PSG(audio_render)(uint8_t *buffer, unsigned n) {
    sg_psg_t *s = &machine;
    while (n--) {
        if (s->audio_tail == s->audio_head) *buffer++ = 128;
        else { *buffer++ = s->audio_buffer[s->audio_tail]; s->audio_tail = (uint16_t)((s->audio_tail + 1) & 511); }
    }
}

#if SG_SYSTEM_GG
/* PSG port: latch/data bytes */
void gg_psg_write(uint8_t value) {
    gg_t *m = &machine;
    gg_audio_catchup();
    if (value & 128) m->psg_latch = (value >> 4) & 7;
    const unsigned r = m->psg_latch, ch = r >> 1;
    if (r & 1) m->psg_volume[ch] = value & 15;
    else if (ch == 3) { m->psg_noise = value & 7; m->audio_lfsr = 0x8000; }
    else if (value & 128) m->psg_tone[ch] = (uint16_t)((m->psg_tone[ch] & 0x3f0) | (value & 15));
    else m->psg_tone[ch] = (uint16_t)((m->psg_tone[ch] & 15) | ((value & 63) << 4));
}
#endif

#undef SG_PSG
#undef SG_CLOCK
#endif
