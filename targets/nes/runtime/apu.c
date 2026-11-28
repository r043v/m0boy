#include "apu.h"
#ifndef M0_TL
#define M0_TL   /* deepeel builds with -DM0_TL=__thread */
#endif

/* The sampled APU: every output sample advances each channel by the CPU
 * cycles it spans (fractional cycles carried), and the envelope, length
 * and sweep counters at their own 240/120 Hz marks.  Mixing is the plain
 * sum the console's DACs approximate.  The frame-counter IRQ stays masked:
 * no game in scope waits on it. */
static const uint8_t duty_table[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 0, 0},
};
static const uint8_t triangle_steps[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};
static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

#define NES_APU_CPU_HZ 1789773u
/* 128 + mix * 120 / 42: the sum's 43 values, no division per sample */
static const uint8_t mix_level[43] = {
#define L(m) (uint8_t)(128 + (m) * 120 / 42)
    L(0), L(1), L(2), L(3), L(4), L(5), L(6), L(7), L(8), L(9), L(10), L(11), L(12), L(13), L(14),
    L(15), L(16), L(17), L(18), L(19), L(20), L(21), L(22), L(23), L(24), L(25), L(26), L(27), L(28),
    L(29), L(30), L(31), L(32), L(33), L(34), L(35), L(36), L(37), L(38), L(39), L(40), L(41), L(42),
#undef L
};

void nes_apu_reset(nes_apu_t *apu) {
    for (unsigned i = 0; i < 4; i++) {
        apu->timer[i] = apu->counter[i] = 0;
        apu->length[i] = apu->seq[i] = 0;
    }
    apu->envelope[0] = apu->envelope[1] = 0;
    apu->duty[0] = apu->duty[1] = 0;
    apu->enable = 0;
    apu->lfsr = 1;
    apu->env_div = apu->len_div = apu->sweep_div[0] = apu->sweep_div[1] = 0;
    apu->triangle_reload = 0;
    apu->sample_frac = 0;
}

void nes_apu_write(nes_apu_t *apu, unsigned reg, uint8_t value) {
    const unsigned voice = reg >> 2;
    if (voice > 3) {
        if (reg == 0x15) {
            apu->enable = value & 0x1F;
            for (unsigned i = 0; i < 4; i++)
                if (!(value & (1u << i))) apu->length[i] = 0;
        }
        return; /* $4017: the IRQ stays masked whatever the game asks */
    }
    switch (reg & 3) {
    case 0:
        if (voice < 2) {
            apu->duty[voice] = value >> 6;
            apu->envelope_halt[voice] = value & 0x20;
            apu->envelope_const[voice] = value & 0x10;
            apu->envelope_vol[voice] = value & 0x0F;
        } else if (voice == 2) apu->triangle_halt = value & 0x80;
        else {
            apu->envelope3_halt = value & 0x20;
            apu->envelope3_const = value & 0x10;
            apu->envelope3_vol = value & 0x0F;
        }
        break;
    case 1:
        if (voice < 2) {
            apu->sweep_on[voice] = value & 0x80;
            apu->sweep_period[voice] = (value >> 4) & 7;
            apu->sweep_negate[voice] = value & 0x08;
            apu->sweep_shift[voice] = value & 7;
            apu->sweep_reload[voice] = 1;
        }
        break;
    case 2:
        apu->timer[voice] = (uint16_t)((apu->timer[voice] & 0x0700) | value);
        if (voice == 3) apu->timer[3] = noise_period[value & 0x0F];
        break;
    case 3:
        apu->timer[voice] = (uint16_t)((apu->timer[voice] & 0x00FF) | ((value & 7) << 8));
        if (voice == 3) apu->timer[3] = noise_period[value & 0x0F];
        if (apu->enable & (1u << voice)) {
            apu->length[voice] = length_table[(value >> 3) & 0x1F];
            if (voice < 2) apu->seq[voice] = 0;
        }
        if (voice == 2) apu->triangle_reload = 1;
        break;
    }
}

uint8_t nes_apu_read(nes_apu_t *apu) {
    uint8_t out = 0;
    for (unsigned i = 0; i < 4; i++)
        if (apu->length[i]) out |= 1u << i;
    return out;
}

/* the frame sequencer's clocks, voice by voice: 240 Hz envelopes, 120 Hz
 * lengths and sweeps */
static void pulse_envelope(nes_apu_t *apu, unsigned i) {
    if (apu->envelope_const[i]) return;
    if (apu->envelope[i]) apu->envelope[i]--;
    else if (apu->envelope_halt[i]) apu->envelope[i] = 15;
}
static void noise_envelope(nes_apu_t *apu) {
    if (apu->envelope3_const) return;
    if (apu->envelope3) apu->envelope3--;
    else if (apu->envelope3_halt) apu->envelope3 = 15;
}
static void length_clock(nes_apu_t *apu, unsigned i) {
    const int halted = i < 2 ? apu->envelope_halt[i] : i == 2 ? apu->triangle_halt : 0;
    if (apu->length[i] && !halted) apu->length[i]--;
}
static void sweep_clock(nes_apu_t *apu, unsigned i) {
    if (apu->sweep_reload[i]) { apu->sweep_reload[i] = 0; apu->sweep_div[i] = apu->sweep_period[i]; return; }
    if (apu->sweep_div[i]) apu->sweep_div[i]--;
    else {
        apu->sweep_div[i] = apu->sweep_period[i];
        if (apu->sweep_on[i] && apu->sweep_shift[i] && apu->timer[i] >= 8) {
            const unsigned delta = apu->timer[i] >> apu->sweep_shift[i];
            const unsigned target = apu->sweep_negate[i]
                                        ? apu->timer[i] - delta - (i ? 1u : 0u)
                                        : apu->timer[i] + delta;
            if (target < 0x800) apu->timer[i] = (uint16_t)target;
        }
    }
}

/* Reciprocals of 1..32, rounded up, 18 fractional bits: with them a
 * division by any short period is a multiply and a shift, exact for the
 * runs the render loop passes (under 8192 cycles; the correction keeps
 * longer runs right, the Cortex-M0+ has no hardware division and its
 * __udivsi3 was the APU's first cost). */
#define APU_RECIP_SHIFT 18
#define APU_SHORT_RECIP(d) (((1u << APU_RECIP_SHIFT) + (d) - 1u) / (d))
static const uint32_t apu_recip[33] = {
    0,
    APU_SHORT_RECIP(1), APU_SHORT_RECIP(2), APU_SHORT_RECIP(3), APU_SHORT_RECIP(4),
    APU_SHORT_RECIP(5), APU_SHORT_RECIP(6), APU_SHORT_RECIP(7), APU_SHORT_RECIP(8),
    APU_SHORT_RECIP(9), APU_SHORT_RECIP(10), APU_SHORT_RECIP(11), APU_SHORT_RECIP(12),
    APU_SHORT_RECIP(13), APU_SHORT_RECIP(14), APU_SHORT_RECIP(15), APU_SHORT_RECIP(16),
    APU_SHORT_RECIP(17), APU_SHORT_RECIP(18), APU_SHORT_RECIP(19), APU_SHORT_RECIP(20),
    APU_SHORT_RECIP(21), APU_SHORT_RECIP(22), APU_SHORT_RECIP(23), APU_SHORT_RECIP(24),
    APU_SHORT_RECIP(25), APU_SHORT_RECIP(26), APU_SHORT_RECIP(27), APU_SHORT_RECIP(28),
    APU_SHORT_RECIP(29), APU_SHORT_RECIP(30), APU_SHORT_RECIP(31), APU_SHORT_RECIP(32),
};

/* A channel's timer over `run` cycles, in closed form: the per-cycle rule
 * is "at 0, reload the period and step, else count down", so the first
 * step comes after counter + 1 cycles and the next ones every timer + 1.
 * Returns the steps taken; the counter is left where the cycles leave it. */
static inline unsigned timer_run(unsigned *counter, unsigned timer, unsigned run) {
    unsigned c = *counter;
    if (run <= c) { *counter = c - run; return 0; }
    run -= c + 1;
    unsigned steps = 1;
    if (timer < 32) { /* short periods (or none: a silent voice's 0) */
        unsigned d = timer + 1, q = (run * apu_recip[d]) >> APU_RECIP_SHIFT;
        while (q * d > run) q--; /* rounded-up reciprocal: never short of the quotient */
        steps += q;
        run -= q * d;
    } else {
        while (run > timer) { run -= timer + 1; steps++; }
    }
    *counter = timer - run;
    return steps;
}

/* The noise's 15-bit LFSR, k steps at once.  A step shifts right and puts
 * bit 0 ^ bit 1 in bit 14; the bits a step reads are still the original
 * ones for the first 14 steps, so k <= 14 steps insert (lfsr ^ lfsr >> 1)'s
 * low k bits, landing from bit 15 - k up.  A short period steps it some
 * forty times a sample: one at a time it was the APU's main cost. */
static inline uint16_t lfsr_run(uint32_t r, unsigned k) {
    for (; k >= 14; k -= 14) r = (r >> 14) | (((r ^ (r >> 1)) & 0x3FFFu) << 1);
    if (k) r = (r >> k) | (((r ^ (r >> 1)) & ((1u << k) - 1)) << (15 - k));
    return (uint16_t)r;
}

/* The render runs a chunk of samples at a time: first the CPU cycles each
 * sample spans and the frame-sequencer marks among them (a mark applies
 * after the steps of its cycle), then each voice over the whole chunk with
 * its state in locals and its output levels redone only at the marks, the
 * levels summed per sample.  Per voice the order of steps, marks and
 * reads is the one of clocking every voice cycle by cycle (1.8 million
 * iterations a second, more than the SAMD21 has). */
#define APU_CHUNK 128u
#define APU_ENV_CYCLES (NES_APU_CPU_HZ / 240)
#define APU_LEN_CYCLES (NES_APU_CPU_HZ / 120)
/* kind: 1 envelopes, 2 lengths; pre: cycles since the sample's start or
 * the previous mark in it; at: cycles since the chunk's start */
typedef struct { uint8_t sample, kind; uint16_t pre, at; } apu_mark;
typedef struct {
    uint16_t cycles[APU_CHUNK]; /* each sample's */
    apu_mark mark[24];
    unsigned marks, n, total;
    uint8_t mix[APU_CHUNK];
} apu_chunk;

static void pulse_levels(const nes_apu_t *apu, unsigned i, uint8_t *level) {
    const unsigned on = apu->length[i] && apu->timer[i] >= 8;
    const unsigned volume = !on ? 0 : apu->envelope_const[i] ? apu->envelope_vol[i] : apu->envelope[i];
    for (unsigned k = 0; k < 8; k++) level[k] = (uint8_t)(duty_table[apu->duty[i]][k] * volume);
}

/* A voice that is silent at the chunk's start stays so through it (its
 * length only counts down; a period under 8 is one the sweep leaves): it
 * only steps, from mark to mark (under 7457 cycles: timer_run's exact
 * range).  mark(apu, i, kind) applies a mark to voice i. */
static void pulse_mark(nes_apu_t *apu, unsigned i, unsigned kind) {
    if (kind & 1) pulse_envelope(apu, i);
    if (kind & 2) { length_clock(apu, i); sweep_clock(apu, i); }
}

/* A pulse's timer counts APU cycles, two CPU cycles each: its sequencer
 * steps every 2 * (timer + 1) CPU cycles, 1789773 / (16 * (timer + 1)) Hz
 * for the eight steps. */
static void pulse_pass(nes_apu_t *apu, unsigned i, apu_chunk *ch) {
    unsigned c = apu->counter[i], seq = apu->seq[i];
    if (!apu->length[i] || apu->timer[i] < 8) {
        unsigned at = 0;
        for (const apu_mark *mk = ch->mark; mk != ch->mark + ch->marks; mk++) {
            seq += timer_run(&c, 2u * apu->timer[i] + 1u, mk->at - at);
            at = mk->at;
            pulse_mark(apu, i, mk->kind);
        }
        seq += timer_run(&c, 2u * apu->timer[i] + 1u, ch->total - at);
        apu->counter[i] = (uint16_t)c;
        apu->seq[i] = (uint8_t)(seq & 7);
        return;
    }
    uint8_t level[8];
    pulse_levels(apu, i, level);
    const apu_mark *mk = ch->mark, *const mk_end = ch->mark + ch->marks;
    for (unsigned s = 0; s < ch->n; s++) {
        unsigned run = ch->cycles[s];
        for (; mk != mk_end && mk->sample == s; mk++) {
            seq += timer_run(&c, 2u * apu->timer[i] + 1u, mk->pre);
            run -= mk->pre;
            pulse_mark(apu, i, mk->kind);
            pulse_levels(apu, i, level);
        }
        seq += timer_run(&c, 2u * apu->timer[i] + 1u, run);
        ch->mix[s] += level[seq & 7];
    }
    apu->counter[i] = (uint16_t)c;
    apu->seq[i] = (uint8_t)(seq & 7);
}

/* the triangle steps while its length and linear counters let it: the
 * condition holds for a whole run (only the else branch changes it, and
 * only to keep it false) */
static inline unsigned triangle_run(nes_apu_t *apu, unsigned *c, unsigned seq, unsigned run) {
    const unsigned k = timer_run(c, apu->timer[2], run);
    if (k) {
        if (apu->length[2] && (apu->triangle_linear || apu->triangle_reload)) seq += k;
        else if (!apu->triangle_halt) apu->triangle_reload = 0;
    }
    return seq;
}

static void triangle_pass(nes_apu_t *apu, apu_chunk *ch) {
    unsigned c = apu->counter[2], seq = apu->seq[2];
    const apu_mark *mk = ch->mark, *const mk_end = ch->mark + ch->marks;
    if (!apu->length[2]) {
        unsigned at = 0;
        for (; mk != mk_end; mk++) {
            seq = triangle_run(apu, &c, seq, mk->at - at);
            at = mk->at;
            if (mk->kind & 2) length_clock(apu, 2);
        }
        seq = triangle_run(apu, &c, seq, ch->total - at);
        apu->counter[2] = (uint16_t)c;
        apu->seq[2] = (uint8_t)(seq & 31);
        return;
    }
    for (unsigned s = 0; s < ch->n; s++) {
        unsigned run = ch->cycles[s];
        for (; mk != mk_end && mk->sample == s; mk++) {
            seq = triangle_run(apu, &c, seq, mk->pre);
            run -= mk->pre;
            if (mk->kind & 2) length_clock(apu, 2);
        }
        seq = triangle_run(apu, &c, seq, run) & 31;
        if (apu->length[2]) ch->mix[s] += triangle_steps[seq] >> 1;
    }
    apu->counter[2] = (uint16_t)c;
    apu->seq[2] = (uint8_t)seq;
}

static void noise_pass(nes_apu_t *apu, apu_chunk *ch) {
    unsigned c = apu->counter[3], lfsr = apu->lfsr, steps;
    const apu_mark *mk = ch->mark, *const mk_end = ch->mark + ch->marks;
    if (!apu->length[3]) {
        unsigned at = 0;
        for (; mk != mk_end; mk++) {
            if ((steps = timer_run(&c, apu->timer[3], mk->at - at))) lfsr = lfsr_run(lfsr, steps);
            at = mk->at;
            if (mk->kind & 1) noise_envelope(apu);
            if (mk->kind & 2) length_clock(apu, 3);
        }
        if ((steps = timer_run(&c, apu->timer[3], ch->total - at))) lfsr = lfsr_run(lfsr, steps);
        apu->counter[3] = (uint16_t)c;
        apu->lfsr = (uint16_t)lfsr;
        return;
    }
    unsigned volume = !apu->length[3] ? 0 : apu->envelope3_const ? apu->envelope3_vol : apu->envelope3;
    for (unsigned s = 0; s < ch->n; s++) {
        unsigned run = ch->cycles[s];
        for (; mk != mk_end && mk->sample == s; mk++) {
            if ((steps = timer_run(&c, apu->timer[3], mk->pre))) lfsr = lfsr_run(lfsr, steps);
            run -= mk->pre;
            if (mk->kind & 1) noise_envelope(apu);
            if (mk->kind & 2) length_clock(apu, 3);
            volume = !apu->length[3] ? 0 : apu->envelope3_const ? apu->envelope3_vol : apu->envelope3;
        }
        if ((steps = timer_run(&c, apu->timer[3], run))) lfsr = lfsr_run(lfsr, steps);
        if (!(lfsr & 1)) ch->mix[s] += volume;
    }
    apu->counter[3] = (uint16_t)c;
    apu->lfsr = (uint16_t)lfsr;
}

void nes_apu_render(nes_apu_t *apu, uint8_t *dst, unsigned n, unsigned rate) {
    /* whole CPU cycles per sample, the remainder carried: the quotient and
     * the remainder once per rate (the frontends keep one rate), an add and
     * a compare per sample (sample_frac stays below rate); chunks short
     * enough for the marks array (eight envelope periods) */
    static M0_TL unsigned cached_rate, whole, part, chunk;
    if (rate != cached_rate) {
        cached_rate = rate;
        whole = NES_APU_CPU_HZ / rate;
        part = NES_APU_CPU_HZ % rate;
        chunk = 8u * APU_ENV_CYCLES / (whole + 1);
        if (chunk > APU_CHUNK) chunk = APU_CHUNK;
        if (!chunk) chunk = 1;
    }
    static M0_TL apu_chunk ch;
    while (n) {
        ch.n = n < chunk ? n : chunk;
        ch.marks = 0;
        ch.total = 0;
        for (unsigned s = 0; s < ch.n; s++) {
            apu->sample_frac += part;
            unsigned left = whole, pre = 0;
            if (apu->sample_frac >= rate) { apu->sample_frac -= rate; left++; }
            ch.cycles[s] = (uint16_t)left;
            ch.mix[s] = 0;
            ch.total += left;
            while (left) {
                unsigned run = left, kind = 0;
                const unsigned to_env = APU_ENV_CYCLES - apu->env_div;
                const unsigned to_len = APU_LEN_CYCLES - apu->len_div;
                if (run > to_env) run = to_env;
                if (run > to_len) run = to_len;
                left -= run;
                pre += run;
                apu->env_div += run;
                if (apu->env_div >= APU_ENV_CYCLES) { apu->env_div = 0; kind |= 1; }
                apu->len_div += run;
                if (apu->len_div >= APU_LEN_CYCLES) { apu->len_div = 0; kind |= 2; }
                if (kind) {
                    ch.mark[ch.marks++] = (apu_mark){ (uint8_t)s, (uint8_t)kind, (uint16_t)pre,
                                                      (uint16_t)(ch.total - left) };
                    pre = 0;
                }
            }
        }
        pulse_pass(apu, 0, &ch);
        pulse_pass(apu, 1, &ch);
        triangle_pass(apu, &ch);
        noise_pass(apu, &ch);
        /* two pulses up to 15, the triangle up to 7, the noise up to 15 */
        for (unsigned s = 0; s < ch.n; s++) dst[s] = mix_level[ch.mix[s] > 42 ? 42 : ch.mix[s]];
        dst += ch.n;
        n -= ch.n;
    }
}
