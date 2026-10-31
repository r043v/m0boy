#include "psg.h"

#include <string.h>

/* the waveform clock runs off the 3.58 MHz half of the master clock: a
 * sample lasts (freq) of its cycles (mednafen's freq_cache = freq << 1
 * master clocks), so a channel advances 3579545 / freq waveform samples
 * a second */
#define PSG_CLOCK 3579545u

void psg_reset(psg_t *p) {
    memset(p, 0, sizeof *p);
    p->lfsr = 0x15555; /* not all zeroes, so the noise breathes */
}

void psg_write(psg_t *p, unsigned at, uint8_t value) {
    at &= 0x0F;
    if (at == 0x00) {
        p->select = value & 0x07;
        return;
    }
    if (at == 0x01) {
        p->balance = value;
        return;
    }
    if (p->select > 5) return;
    switch (at) {
    case 0x02:
        p->ch[p->select].frequency = (uint16_t)((p->ch[p->select].frequency & 0x0F00) | value);
        break;
    case 0x03:
        p->ch[p->select].frequency = (uint16_t)((p->ch[p->select].frequency & 0x00FF) | ((value & 0x0F) << 8));
        break;
    case 0x04: /* enable, DDA, volume */
        if ((p->ch[p->select].control & 0x40) && !(value & 0x40))
            p->ch[p->select].waveform_index = 0;
        p->ch[p->select].control = value;
        break;
    case 0x05:
        p->ch[p->select].balance = value;
        break;
    case 0x06: /* the waveform, or the DDA stream */
        value &= 0x1F;
        if (!(p->ch[p->select].control & 0x40))
            p->ch[p->select].waveform[p->ch[p->select].waveform_index] = value;
        if ((p->ch[p->select].control & 0xC0) == 0x00)
            p->ch[p->select].waveform_index = (p->ch[p->select].waveform_index + 1) & 0x1F;
        if (p->ch[p->select].control & 0x80)
            p->ch[p->select].dda = value;
        break;
    case 0x07: /* the noise enable and rate */
        p->ch[p->select].noise = value;
        break;
    default: /* the LFO (0x08) and beyond: not modeled */
        break;
    }
}

/* the channels' sample clock, 12 fractional bits, per output sample */
static uint32_t wave_step(unsigned freq, unsigned rate) {
    if (!freq) freq = 4096;
    return (uint32_t)(((uint64_t)PSG_CLOCK << 12) / ((uint64_t)freq * rate));
}

static int channel_sample(psg_t *p, unsigned c, unsigned rate) {
    const uint8_t control = p->ch[c].control;
    if (!(control & 0x80)) return 0;
    if (c >= 4 && (p->ch[c].noise & 0x80)) {
        /* the noise: an 18-bit LFSR (the tap pair is an approximation,
         * the polynomial of the chip is not published) */
        const unsigned nf = (p->ch[c].noise & 0x1F) + 1;
        p->ch[c].acc += (uint32_t)(((uint64_t)PSG_CLOCK << 12) /
                                    ((uint64_t)nf * rate));
        while (p->ch[c].acc >> 12) {
            p->ch[c].acc -= 1u << 12;
            const unsigned fb = ((p->lfsr >> 17) ^ (p->lfsr >> 12)) & 1;
            p->lfsr = ((p->lfsr << 1) | fb) & 0x3FFFF;
        }
        return (p->lfsr >> 17) & 1 ? 15 : -16;
    }
    if (control & 0x40) /* DDA: the last written sample, held */
        return (int)p->ch[c].dda - 16;
    p->ch[c].acc += wave_step(p->ch[c].frequency, rate);
    if (p->ch[c].acc >> 12) {
        p->ch[c].waveform_index = (p->ch[c].waveform_index + (p->ch[c].acc >> 12)) & 0x1F;
        p->ch[c].acc &= 0x0FFF;
    }
    return (int)p->ch[c].waveform[p->ch[c].waveform_index] - 16;
}

static uint8_t clamp_sample(int mix) {
    mix += 128;
    return (uint8_t)(mix < 0 ? 0 : mix > 255 ? 255 : mix);
}

static void render_pair(psg_t *p, uint8_t *left, uint8_t *right,
                        unsigned rate) {
    int l = 0, r = 0;
    for (unsigned c = 0; c < 6; c++) {
        const int s = channel_sample(p, c, rate); /* the phase advances even at volume 0 */
        const unsigned vol = p->ch[c].control & 0x1F;
        if (!vol) continue; /* s * 0 drops out of the mix exactly */
        l += s * (int)vol * (p->ch[c].balance >> 4);
        r += s * (int)vol * (p->ch[c].balance & 0x0F);
    }
    /* Full balance on both registers retains the former /24 mix level. */
    l = l * (p->balance >> 4) / (24 * 15 * 15);
    r = r * (p->balance & 0x0F) / (24 * 15 * 15);
    *left = clamp_sample(l);
    *right = clamp_sample(r);
}

void psg_render_stereo(psg_t *p, uint8_t *dst, unsigned n, unsigned rate) {
    if (!rate) return;
    for (unsigned i = 0; i < n; i++)
        render_pair(p, &dst[2 * i], &dst[2 * i + 1], rate);
}

void psg_render(psg_t *p, uint8_t *dst, unsigned n, unsigned rate) {
    if (!rate) return;
    for (unsigned i = 0; i < n; i++) {
        uint8_t l, r;
        render_pair(p, &l, &r, rate);
        dst[i] = (uint8_t)(((unsigned)l + r) / 2);
    }
}
