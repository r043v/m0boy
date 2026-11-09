#include "md_full.h"
#include <string.h>

/* One clock for every sound part: the NTSC master clock. The 68000 and the
 * YM2612 run at master/7, the Z80 and the PSG at master/15. The YM2612 makes
 * one sample every 144 of its clocks, the PSG ticks every 16 of its own. */
enum {
    MASTER_HZ = 53693175, AUDIO_RATE = 44100,
    FRAME_MASTER = 127856 * 7, VINT_MASTER = 109312 * 7, VINT_LENGTH = 228 * 15,
    FM_MASTER = 144 * 7, PSG_MASTER = 16 * 15, PSG_AMP = 1800,
};

static uint64_t m68k_time(const MdFull *m) {
    return ((uint64_t)m->frames * 127856u + m->frame_cycles) * 7u;
}


/* ---- SN76489 (Sega variant) ------------------------------------------- */

static void psg_write(MdPsg *p, uint8_t value) {
    if (value & 0x80u) p->latch = (value >> 4) & 7u;
    const unsigned r = p->latch, ch = r >> 1;
    if (r & 1u) p->volume[ch] = value & 15u;
    else if (ch == 3u) { p->noise = value & 7u; p->lfsr = 0x8000u; }
    else if (value & 0x80u) p->tone[ch] = (uint16_t)((p->tone[ch] & 0x3f0u) | (value & 15u));
    else p->tone[ch] = (uint16_t)((p->tone[ch] & 15u) | ((value & 63u) << 4));
}

static void psg_tick(MdPsg *p) {
    for (unsigned ch = 0; ch < 3; ++ch) {
        if (p->tone[ch] <= 1u) { p->high[ch] = 1; continue; }   /* sample playback level */
        if (p->counter[ch] <= 1u) { p->counter[ch] = p->tone[ch]; p->high[ch] ^= 1u; }
        else --p->counter[ch];
    }
    if (p->counter[3] <= 1u) {
        const unsigned rate = p->noise & 3u;
        p->counter[3] = rate == 3u ? (p->tone[2] ? p->tone[2] : 1u) : 0x10u << rate;
        p->high[3] ^= 1u;
        if (p->high[3]) {
            const unsigned bit = (p->noise & 4u) ? ((p->lfsr ^ (p->lfsr >> 3)) & 1u) : (p->lfsr & 1u);
            p->lfsr = (uint16_t)((p->lfsr >> 1) | (bit << 15));
        }
    } else --p->counter[3];
}

static int psg_output(const MdPsg *p) {
    /* lround(1800 * 10^(-i/10)); fixed constants, no floating point. */
    static const int amp[16] = {
        1800, 1430, 1136, 902, 717, 569, 452, 359, 285, 227, 180, 143, 114, 90, 72, 57,
    };
    int mix = 0;
    for (unsigned ch = 0; ch < 3; ++ch) mix += p->high[ch] ? amp[p->volume[ch]] : -amp[p->volume[ch]];
    return mix + ((p->lfsr & 1u) ? amp[p->volume[3]] : -amp[p->volume[3]]);
}

/* ---- Mixing and resampling -------------------------------------------- */

static void audio_emit(MdFull *m, int left, int right) {
    const int in[2] = {left, right};
    int16_t s[2];
    for (unsigned c = 0; c < 2; ++c) {
        /* Remove DC (about 3 Hz), like the console's output capacitors. */
        m->audio_dc[c] += ((in[c] * 256) - m->audio_dc[c]) / 2048;
        const int32_t y = in[c] - m->audio_dc[c] / 256;
        s[c] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        m->audio_hash = (m->audio_hash ^ (uint8_t)s[c]) * 16777619u;
        m->audio_hash = (m->audio_hash ^ (uint8_t)((uint16_t)s[c] >> 8)) * 16777619u;
    }
    if (m->audio_frames < sizeof m->audio_out / sizeof m->audio_out[0] / 2u) {
        m->audio_out[m->audio_frames * 2u] = s[0];
        m->audio_out[m->audio_frames * 2u + 1u] = s[1];
        ++m->audio_frames;
    }
}

/* Produces every FM sample that ends at or before master clock `until`.
 * Output frame k sits at k * MASTER_HZ (scaled by AUDIO_RATE); that
 * position is tracked in audio_next, so a sample emits without any
 * division or 64-bit product — the FM step adds a constant span. */
static void audio_run(MdFull *m, uint64_t until) {
    if (m->no_audio) return;
    while (m->audio_time + FM_MASTER <= until) {
        int fm[2];
        MD_PERF_ADD(m, p_ym);
        md_ym_sample(&m->ym, &fm[0], &fm[1]);
        int psg = 0, ticks = 0;
        for (m->psg_rest += FM_MASTER; m->psg_rest >= PSG_MASTER; m->psg_rest -= PSG_MASTER, ++ticks) {
            psg_tick(&m->psg);
            MD_PERF_ADD(m, p_psg);
            psg += psg_output(&m->psg);
        }
        if (ticks) psg /= ticks;
        const int now[2] = {fm[0] * 3 / 4 + psg, fm[1] * 3 / 4 + psg};
        const uint64_t start = m->audio_time * AUDIO_RATE;
        for (; m->audio_next <= start + (uint64_t)FM_MASTER * AUDIO_RATE; m->audio_next += MASTER_HZ) {
            const int64_t f = (int64_t)(m->audio_next - start), span = (int64_t)FM_MASTER * AUDIO_RATE;
            audio_emit(m, (int)(m->audio_last[0] + (now[0] - m->audio_last[0]) * f / span),
                          (int)(m->audio_last[1] + (now[1] - m->audio_last[1]) * f / span));
        }
        m->audio_last[0] = now[0];
        m->audio_last[1] = now[1];
        m->audio_time += FM_MASTER;
    }
}

/* ---- Z80 and its bus --------------------------------------------------- */

static uint64_t z80_time(const MdFull *m) { return m->sound_cpu.cycles * 15u; }

static void z80_run(MdFull *m, uint64_t until) {
    z80_t *z = &m->sound_cpu;
    if (m->no_z80) return;
    const uint64_t target = until / 15u;
    if (!m->z80_reset || m->z80_bus || z->fault) {
        if (z->cycles < target) z->cycles = target;
        return;
    }
    /* /INT is held for about one line from the start of vertical blank;
     * the frame phase advances by each step's own clocks (a step is far
     * below one frame, so one conditional subtract keeps the modulo). */
    uint32_t at = (uint32_t)(z80_time(m) % FRAME_MASTER);
    while (z->cycles < target && !z->fault) {
        z->irq = at >= VINT_MASTER && at < VINT_MASTER + VINT_LENGTH;
        at += z80_step(z) * 15u;
        MD_PERF_ADD(m, p_z80);
        if (at >= FRAME_MASTER) at -= FRAME_MASTER;
    }
    z->irq = 0;
}

static void sound_store(MdFull *m, uint16_t addr, uint8_t value, uint64_t at) {
    if (addr < 0x4000u) { m->zram[addr & 0x1fffu] = value; return; }
    if (addr < 0x6000u) {
        audio_run(m, at);
        const unsigned bank = (addr >> 1) & 1u;
        if (!(addr & 1u)) m->ym.addr[bank] = value;
        else {
            if (m->ym_log) m->ym_log(m->ym_log_user, m->audio_time / FM_MASTER, bank, m->ym.addr[bank], value);
            md_ym_write(&m->ym, bank, m->ym.addr[bank], value);
            m->ym.busy_until = at + 32u * 6u * 7u;   /* 32 internal clocks of fM/6 */
        }
        return;
    }
    if (addr < 0x6100u) {
        m->z80_bank = ((m->z80_bank >> 1) | ((uint32_t)(value & 1u) << 23)) & 0xff8000u;
        return;
    }
    if ((addr & 0xfff9u) == 0x7f11u) { audio_run(m, at); psg_write(&m->psg, value); }
}

static uint8_t sound_load(MdFull *m, uint16_t addr, uint64_t at) {
    if (addr < 0x4000u) return m->zram[addr & 0x1fffu];
    if (addr < 0x6000u) { audio_run(m, at); return (uint8_t)(m->ym.status | (at < m->ym.busy_until ? 0x80u : 0u)); }
    return 0xff;
}

static uint8_t zread(void *user, uint16_t addr) {
    MdFull *m = (MdFull *)user;
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a < md_rom_size) return md_rom[a];
        if (a >= 0xff0000u) return m->ram[a & 0xffffu];
        return 0xff;
    }
    return sound_load(m, addr, z80_time(m));
}
static void zwrite(void *user, uint16_t addr, uint8_t value) {
    MdFull *m = (MdFull *)user;
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a >= 0xff0000u) m->ram[a & 0xffffu] = value;
        else if ((a & 0xffffe0u) == 0xc00000u && (a & 0x19u) == 0x11u) sound_store(m, 0x7f11u, value, z80_time(m));
        return;
    }
    sound_store(m, addr, value, z80_time(m));
}
static uint8_t zin(void *user, uint16_t port) { (void)user; (void)port; return 0xff; }
static void zout(void *user, uint16_t port, uint8_t value) { (void)user; (void)port; (void)value; }

void md_full_audio_reset(MdFull *m) {
    m->sound_cpu.user = m;
    m->sound_cpu.read = zread;
    m->sound_cpu.write = zwrite;
    m->sound_cpu.in = zin;
    m->sound_cpu.out = zout;
    z80_reset(&m->sound_cpu);
    m->sound_cpu.cycles = 0;
    m->z80_reset = 0;
    md_ym_reset(&m->ym);
    memset(&m->psg, 0, sizeof m->psg);
    m->psg.lfsr = 0x8000u;
    for (unsigned i = 0; i < 4; ++i) m->psg.volume[i] = 15;
    m->audio_time = 0;
    m->audio_next = MASTER_HZ;   /* the first output frame k=1 */
    m->audio_frames = 0;
    m->audio_hash = 2166136261u;
}

/* 68000 side: the Z80 and the sound chips first catch up with its clock. */
void md_full_sound_write(MdFull *m, uint32_t addr, uint8_t value) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    sound_store(m, (uint16_t)addr, value, now);
}
uint8_t md_full_sound_read(MdFull *m, uint32_t addr) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    return sound_load(m, (uint16_t)addr, now);
}

/* reset_line 0: bus request (A11100), 1: reset (A11200, 1 = running). */
void md_full_sound_control(MdFull *m, unsigned reset_line, uint8_t value) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    if (!reset_line) { m->z80_bus = value; return; }
    if (value && !m->z80_reset) {
        const uint64_t cycles = m->sound_cpu.cycles;
        z80_reset(&m->sound_cpu);
        m->sound_cpu.cycles = cycles;
    } else if (!value && m->z80_reset) {
        /* The YM2612 shares the Z80 reset line. */
        audio_run(m, now);
        md_ym_reset(&m->ym);
    }
    m->z80_reset = value;
}

void md_full_sound_frame(MdFull *m) {
    const uint64_t end = (uint64_t)m->frames * FRAME_MASTER;
    z80_run(m, end);
    audio_run(m, end);
}

unsigned md_full_audio_read(MdFull *m, int16_t *samples, unsigned max) {
    const unsigned n = m->audio_frames < max ? m->audio_frames : max;
    memcpy(samples, m->audio_out, n * 2u * sizeof *samples);
    memmove(m->audio_out, m->audio_out + n * 2u, (m->audio_frames - n) * 2u * sizeof *samples);
    m->audio_frames -= n;
    return n;
}
