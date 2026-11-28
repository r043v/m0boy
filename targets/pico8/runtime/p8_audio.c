/* PICO-8 sound, see p8_audio.h. */
#include "p8_audio.h"
#include "p8gfx.h"
#include "p8rt.h"
#include "m0_divide.h"
#include <string.h>

#define CHANNELS 4
#define SFX_BYTES 68
#define SFX_BASE 0x3200u
#define MUSIC_BASE 0x3100u
#define MUSIC_COUNT 64
#define MAX_VOLUME_STEP 512 /* amplitude per volume step (femto8's MAX_VOLUME / 8) */

enum { MODE_NONE, MODE_SOUND, MODE_MUSIC };
enum { W_TRIANGLE, W_TILTED_SAW, W_SAW, W_SQUARE, W_PULSE, W_ORGAN, W_NOISE, W_PHASER };
enum { E_NONE, E_SLIDE, E_VIBRATO, E_DROP, E_FADEIN, E_FADEOUT, E_ARP_FAST, E_ARP_SLOW };

typedef struct {
    uint8_t mode, sfx, note, end;
    uint32_t into;          /* samples played of the current note */
    uint32_t phase;         /* oscillator, one cycle = 2^32 */
    uint32_t samples_per_note;
    uint32_t inc;           /* phase step of the current note and effect */
    int16_t amplitude;
    uint8_t waveform;
    uint8_t held;           /* the (pitch, volume) the step and amplitude were computed for */
    uint16_t held_key;
} Chan;

static Chan chan[CHANNELS];
static int music_pattern = -1;
static unsigned sample_rate = 22050;
static uint32_t inc_base[12]; /* phase step of the 12 notes of the lowest octave */
static uint32_t noise_state = 0x1234567u;

/* the notes of the lowest octave in Hz * 100, as femto8's table halved */
static const uint16_t tone_hz100[12] = { 6540, 6930, 7342, 7778, 8240, 8730, 9250, 9800, 10382, 11000, 11654, 12347 };

void p8_audio_set_rate(unsigned rate) {
    sample_rate = rate ? rate : 22050;
    for (int i = 0; i < 12; i++) inc_base[i] = (uint32_t)(((uint64_t)tone_hz100[i] << 32) / ((uint64_t)sample_rate * 100u));
}

void p8_audio_init(unsigned rate) {
    memset(chan, 0, sizeof chan);
    music_pattern = -1;
    p8_audio_set_rate(rate);
}

static inline unsigned sfx_speed(int n) { return p8_rom_u8(&p8_cart[SFX_BASE + SFX_BYTES * n + 65]); }
static inline uint16_t sfx_note(int n, int i) {
    const uint8_t *p = &p8_cart[SFX_BASE + SFX_BYTES * n + 2 * i];
    return (uint16_t)(p8_rom_u8(&p[0]) | (p8_rom_u8(&p[1]) << 8));
}

static void channel_start(Chan *c, int n, int start, int end, int mode) {
    c->mode = (uint8_t)mode;
    c->sfx = (uint8_t)n;
    c->note = (uint8_t)start;
    c->end = (uint8_t)end;
    c->samples_per_note = (uint32_t)((uint64_t)sample_rate * (sfx_speed(n) + 1) / 128u);
    if (!c->samples_per_note) c->samples_per_note = 1;
    c->into = 0;
    c->held_key = 0xffff;
}

void p8_audio_sfx(int n, int channel, int start, int end) {
    if (n == -1) {
        if (channel >= 0 && channel < CHANNELS) chan[channel].mode = MODE_NONE;
        return;
    }
    if (n == -2) return;
    if (channel == -2) { /* every channel playing n */
        for (int i = 0; i < CHANNELS; i++) if (chan[i].mode != MODE_NONE && chan[i].sfx == n) chan[i].mode = MODE_NONE;
        return;
    }
    if (channel == -1) {
        for (int i = 0; i < CHANNELS; i++) if (chan[i].mode == MODE_NONE) { channel = i; break; }
        if (channel == -1) channel = 0; /* all busy: the first is taken */
    }
    if (channel < 0 || channel >= CHANNELS || n < 0 || n >= 64) return;
    if (start < 0) start = 0;
    if (end > 32) end = 32;
    if (start >= end) return;
    channel_start(&chan[channel], n, start, end, MODE_SOUND);
}

static void music_start_pattern(int pattern) {
    music_pattern = pattern;
    for (int i = 0; i < CHANNELS; i++) {
        const uint8_t data = p8_rom_u8(&p8_cart[MUSIC_BASE + 4 * pattern + i]);
        if (!(data & 0x40)) channel_start(&chan[i], data & 0x7f, 0, 31, MODE_MUSIC);
        else chan[i].mode = MODE_NONE;
    }
}

void p8_audio_music(int pattern, int fade_ms, int mask) {
    (void)fade_ms; (void)mask;
    if (pattern < 0) {
        for (int i = 0; i < CHANNELS; i++) chan[i].mode = MODE_NONE;
        music_pattern = -1;
    } else if (pattern < MUSIC_COUNT) music_start_pattern(pattern);
}

int p8_audio_stat(int index) {
    if (index >= 16 && index <= 19) return chan[index - 16].mode == MODE_NONE ? -1 : chan[index - 16].sfx;
    if (index >= 20 && index <= 23) return chan[index - 20].mode == MODE_NONE ? -1 : (int)chan[index - 20].note;
    if (index >= 46 && index <= 49) return chan[index - 46].mode == MODE_NONE ? -1 : chan[index - 46].sfx;
    if (index >= 50 && index <= 53) return chan[index - 50].mode == MODE_NONE ? -1 : (int)chan[index - 50].note;
    if (index == 24 || index == 25 || index == 54 || index == 55) {
        for (int i = 0; i < CHANNELS; i++) if (chan[i].mode == MODE_MUSIC) return music_pattern;
        return -1;
    }
    if (index == 57) {
        for (int i = 0; i < CHANNELS; i++) if (chan[i].mode == MODE_MUSIC) return 1;
    }
    return 0;
}

/* a channel reached the end of its sfx: the sound stops, the music goes on to
 * the next pattern (after the loop-back and stop flags) */
static void channel_ended(Chan *c) {
    if (c->mode == MODE_SOUND) { c->mode = MODE_NONE; return; }
    const uint8_t *m = &p8_cart[MUSIC_BASE + 4 * music_pattern];
    if (p8_rom_u8(&m[2]) & 0x80) return; /* stop flag: this channel idles on its last note */
    int next = music_pattern + 1;
    if ((p8_rom_u8(&m[1]) & 0x80) || next == MUSIC_COUNT) {
        int i = next - 1;
        while (i >= 0) {
            if ((p8_rom_u8(&p8_cart[MUSIC_BASE + 4 * i]) & 0x80) || i == 0) break;
            i--;
        }
        next = i;
    }
    music_start_pattern(next);
}

/* one cycle of the waveform at `ph` (16 bits of phase), for an amplitude a */
static inline int wave(const Chan *c, uint32_t ph16) {
    const int a = c->amplitude;
    switch (c->waveform) {
    case W_TRIANGLE: return ph16 < 32768 ? a - (int)((a * ph16) >> 14) : -a + (int)((a * (ph16 - 32768)) >> 14);
    case W_TILTED_SAW: { /* up for 85 % of the cycle, then down; the slopes of the 8 volumes */
#define UP(v) ((2u * MAX_VOLUME_STEP * (v) << 16) / 55705u)
#define DOWN(v) ((2u * MAX_VOLUME_STEP * (v) << 16) / 9831u)
        static const uint32_t up[8] = { UP(0), UP(1), UP(2), UP(3), UP(4), UP(5), UP(6), UP(7) };
        static const uint32_t down[8] = { DOWN(0), DOWN(1), DOWN(2), DOWN(3), DOWN(4), DOWN(5), DOWN(6), DOWN(7) };
#undef UP
#undef DOWN
        const unsigned v = (unsigned)a / MAX_VOLUME_STEP; /* a power of two */
        return ph16 < 55705 ? -a + (int)((up[v] * ph16) >> 16) : a - (int)((down[v] * (ph16 - 55705)) >> 16);
    }
    case W_SAW: return -a + (int)((2 * a * ph16) >> 16);
    case W_SQUARE: return ph16 < 32768 ? -a : a;
    case W_PULSE: return ph16 < 21845 ? a : -a;
    case W_ORGAN:
        if (ph16 < 16384) return a - (int)((a * ph16) >> 13);
        if (ph16 < 32768) return -a + (int)((3 * a * (ph16 - 16384)) >> 15);
        if (ph16 < 49152) return a / 2 - (int)((3 * a * (ph16 - 32768)) >> 15);
        return -a + (int)((a * (ph16 - 49152)) >> 13);
    case W_NOISE:
        noise_state ^= noise_state << 13;
        noise_state ^= noise_state >> 17;
        noise_state ^= noise_state << 5;
        return (int)(((noise_state >> 16) * (uint32_t)a) >> 16) - a / 2;
    default: return 0;
    }
}

/* pitch, volume and waveform of the note at hand with its effect; every 128
 * samples, so an effect moves smoothly inside a note */
static void update_note(Chan *c) {
    const uint16_t data = sfx_note(c->sfx, c->note);
    const int effect = (data & 0x7000) >> 12, volume = (data & 0x0e00) >> 9, pitch = data & 0x3f;
    int eff_pitch = pitch, eff_volume = volume;
    c->waveform = (uint8_t)((data & 0x01c0) >> 6);
    if (effect != E_NONE) {
        const int t = (int)m0_ratio256(c->into, c->samples_per_note); /* 0..255 */
        switch (effect) {
        case E_SLIDE: {
            int prev = pitch;
            if (c->note > 0) prev = sfx_note(c->sfx, c->note - 1) & 0x3f;
            eff_pitch = prev + (((pitch - prev) * t) >> 8);
            break;
        }
        case E_VIBRATO: { /* a pitch step either way, once per note */
            const int q = (t * 4) >> 8; /* quarter of the cycle */
            eff_pitch = pitch + (q == 1 ? 1 : q == 3 ? -1 : 0);
            break;
        }
        case E_DROP: eff_pitch = (pitch * (256 - t)) >> 8; break;
        case E_FADEIN: eff_volume = (volume * t) >> 8; break;
        case E_FADEOUT: eff_volume = (volume * (256 - t)) >> 8; break;
        case E_ARP_FAST: { const int q = (t * 4) >> 8, phase = q == 3 ? 0 : q; eff_pitch = pitch + (phase == 1 ? 4 : phase == 2 ? 7 : 0); break; }
        case E_ARP_SLOW: { const int phase = (t * 2) >> 8; eff_pitch = pitch + (phase == 1 ? 4 : phase == 2 ? 7 : 0); break; }
        }
        if (eff_pitch < 0) eff_pitch = 0;
        if (eff_pitch > 63) eff_pitch = 63;
        if (eff_volume < 0) eff_volume = 0;
        if (eff_volume > 7) eff_volume = 7;
    }
    const uint16_t key = (uint16_t)(eff_pitch | (eff_volume << 6) | (c->waveform << 9));
    if (key != c->held_key) { /* only when something changed */
        c->held_key = key;
        unsigned note = (unsigned)eff_pitch, octave = 0;
        while (note >= 12) { note -= 12; octave++; }
        c->inc = inc_base[note] << octave;
        c->amplitude = (int16_t)(MAX_VOLUME_STEP * eff_volume);
    }
}

void p8_audio_render(uint8_t *buffer, unsigned n) {
    int32_t sum[128];
    while (n) {
        const unsigned block = n > 128 ? 128 : n;
        memset(sum, 0, block * sizeof sum[0]);
        for (int ci = 0; ci < CHANNELS; ci++) {
            Chan *c = &chan[ci];
            if (c->mode == MODE_NONE) continue;
            unsigned done = 0;
            while (done < block && c->mode != MODE_NONE) {
                update_note(c);
                /* up to the end of the block, of the note, or 128 samples */
                unsigned run = block - done;
                const uint32_t left = c->samples_per_note - c->into;
                if (run > left) run = left;
                if (c->amplitude && c->waveform != W_PHASER) {
                    uint32_t ph = c->phase;
                    for (unsigned i = 0; i < run; i++, ph += c->inc) sum[done + i] += wave(c, ph >> 16);
                    c->phase = ph;
                } else c->phase += c->inc * run;
                done += run;
                c->into += run;
                if (c->into >= c->samples_per_note) { c->into = 0; c->note++; } /* a run stops at the note's end */
                if (c->note >= c->end) channel_ended(c); /* may restart the channel on the next pattern */
            }
        }
        for (unsigned i = 0; i < block; i++) {
            int v = 128 + (sum[i] >> 7);
            buffer[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        buffer += block;
        n -= block;
    }
}

/* The channels at this driver's rate: a state loads into the firmware that
 * wrote it, on the same console. */
void p8_audio_state_io(p8_state_fn io, void *ctx) {
    io(chan, sizeof chan, ctx);
    io(&music_pattern, sizeof music_pattern, ctx);
    io(&noise_state, sizeof noise_state, ctx);
}
