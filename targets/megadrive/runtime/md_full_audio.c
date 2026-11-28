#include <stdio.h>
#include <stdlib.h>
#include "md_full.h"
#include "m0_divide.h"
#include <string.h>

/* One clock for every sound part: the NTSC master clock. The 68000 and the
 * YM2612 run at master/7, the Z80 and the PSG at master/15. The YM2612 makes
 * one sample every 144 of its clocks, the PSG ticks every 16 of its own. */
enum {
    MASTER_HZ = 53693175, AUDIO_RATE = 44100,
    FRAME_MASTER = 262 * 3420, VINT_MASTER = 0, VINT_LENGTH = 228 * 15,
    FM_MASTER = 144 * 7, PSG_MASTER = 16 * 15, PSG_AMP = 1800,
};

static uint64_t m68k_time(const MdFull *m) {
    return (uint64_t)m->frames * FRAME_MASTER + (uint64_t)m->frame_cycles * 7u;
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

/* last + (now - last) * f / (FM_MASTER * AUDIO_RATE), truncated toward zero,
 * in 32-bit divisions.  f and the span are multiples of 75: f / 75 is an exact
 * product by the inverse of 75, and the rest of the span is 64 * 9261. */
_Static_assert(MASTER_HZ % 75 == 0 && FM_MASTER * AUDIO_RATE == 75 * 64 * 9261, "audio span");
static int between(int last, int now, uint32_t f) {
    const uint32_t part = f * 0x962fc963u; /* f / 75 */
    const int delta = now - last;
    const uint32_t size = delta < 0 ? 0u - (uint32_t)delta : (uint32_t)delta;
    const int step = (int)((uint32_t)((uint64_t)size * part >> 6) / 9261u);
    return last + (delta < 0 ? -step : step);
}

/* Produces every FM sample that ends at or before master clock `until`.
 * Output frame k sits at k * MASTER_HZ (scaled by AUDIO_RATE); that
 * position is tracked in audio_next, so a sample emits without any
 * division or 64-bit product — the FM step adds a constant span. */
static void audio_run(MdFull *m, uint64_t until) {
#if defined(MD_PERF)
    if (m->no_audio) return;
#endif
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
            const uint32_t f = (uint32_t)(m->audio_next - start);
            audio_emit(m, between(m->audio_last[0], now[0], f), between(m->audio_last[1], now[1], f));
        }
        m->audio_last[0] = now[0];
        m->audio_last[1] = now[1];
        m->audio_time += FM_MASTER;
    }
}

/* ---- Z80 and its bus --------------------------------------------------- */

static unsigned z80tab_from = ~0u;   /* frame from which DP_MD_GENS_Z80TAB=<frame> applies (seamless: both timelines agree at a frame start) */
static uint64_t z80tab_now;
static int z80tab_on(void) { static int on = -1; if (on < 0) { on = getenv("DP_MD_GENS_Z80TAB") != NULL; if (on) z80tab_from = (unsigned)atoi(getenv("DP_MD_GENS_Z80TAB")); } return on && z80tab_now >= z80tab_from; }
/* DP_MD_GENS_Z80TAB: Gens' Z80 timeline, 228 Z80 cycles per 488-cycle 68000 line, the Z80 position being the line end less
 * (int)(remaining 68000 cycles * 7 / 15) (Z80_M68K_Cycle_Tab). */
/* DP_MD_GENS_Z80PHASE=<n>: the frame (V-int) starts n 68000 cycles into a Gens line (Gens: 128, the V-int line runs to
 * Cycles_M68K - 360); the Z80 count is rebased so that the frame start stays Z80 cycle 0. */
static uint32_t z80tab_phase(void) { static int ph = -1; if (ph < 0) ph = getenv("DP_MD_GENS_Z80PHASE") ? atoi(getenv("DP_MD_GENS_Z80PHASE")) : 0; return (uint32_t)ph; }
static uint32_t z80tab_base(void) {
    static int b = -1;
    if (b < 0) { const uint32_t ph = z80tab_phase(); b = ph ? (int)(228u - (488u - ph) * 7u / 15u) : 0; }
    return (uint32_t)b;
}
static uint64_t z80_target(uint64_t until) {
    if (!z80tab_on()) { static int rn = -1; if (rn < 0) rn = getenv("DP_MD_GENS_ZRND") ? atoi(getenv("DP_MD_GENS_ZRND")) : 0; return (until + (uint64_t)rn) / 15u; }   /* ZRND: rounding experiment */
    const uint64_t f = until / FRAME_MASTER, r = until % FRAME_MASTER;
    if (r == 0u) return f * 59736u;
    if (r % 7u) return f * 59736u + r / 15u;
    const uint32_t ph = z80tab_phase(), fc = (uint32_t)(r / 7u) + ph, L = (fc - 1u) / 488u, rem = 488u * (L + 1u) - fc;
    return f * 59736u + 228u * (L + 1u) - (uint32_t)((uint64_t)rem * 7u / 15u) - z80tab_base();
}
static uint64_t z80_master(uint64_t c) {
    if (!z80tab_on()) return c * 15u;
    const uint64_t f = c / 59736u; const uint32_t k = (uint32_t)(c % 59736u);
    if (!k) return f * FRAME_MASTER;
    const uint32_t ph = z80tab_phase(), kk = k + z80tab_base(), L = (kk - 1u) / 228u, kz = kk - 228u * L, q = 228u - kz, rem = (q * 15u + 6u) / 7u, fc = 488u * (L + 1u) - rem - ph;
    return f * FRAME_MASTER + (uint64_t)fc * 7u;
}
static uint64_t z80_time(const MdFull *m) { return z80_master(m->sound_cpu.cycles); }

/* Gens (mdZ80) instruction lengths that differ from the Z80: the TAS
 * compatibility profile (DP_MD_GENS_Z80TIME) charges them as Gens does, the
 * hardware path keeps the real ones. DJNZ not taken 10 (8), RET cc taken
 * 17 (11), INC/DEC (IX+d) 26 (23), DDCB rotates and shifts 27 (23),
 * undocumented DDCB BIT on a register 12 (20), CPIR/CPDR leaving 18 (16),
 * undefined ED opcodes 4 (8). */
static int gens_z80time(void) { static int on = -1; if (on < 0) on = getenv("DP_MD_GENS_Z80TIME") != NULL; return on; }
static int ed_undefined(uint8_t op) {
    if (op < 0x40u || op >= 0xc0u) return 1;
    if (op < 0x80u) return op == 0x77u || op == 0x7fu;
    return op < 0xa0u || (op & 0x04u);
}
typedef struct { uint16_t pc, len; uint8_t op, b, ix; } GensStep;
static int gens_pre(const z80_t *z, GensStep *g) {
    if (!gens_z80time() || z->nmi || z->halted || (z->irq && z->iff1 && !z->ei_delay)) return 0;
    g->pc = z->pc; g->b = z->b; g->op = z->read(z->user, g->pc); g->len = 1u;
    g->ix = g->op == 0xddu || g->op == 0xfdu;
    if (g->ix) { g->op = z->read(z->user, (uint16_t)(g->pc + 1u)); g->len = 2u; }
    return 1;
}
static int ld_re;   /* diagnostic: chunk re-entries inside the running LDIR */
/* Clocks Gens charges beyond (or short of) the step that just ran. */
static int gens_post(z80_t *z, const GensStep *g) {
    int extra = 0;
    if (z->fault) return 0;
    if (g->op == 0x10u) { if (g->b == 1u) extra = 2; }
    else if ((g->op & 0xc7u) == 0xc0u) { if (z->pc != (uint16_t)(g->pc + g->len)) extra = 6; }
    else if (g->ix && (g->op == 0x34u || g->op == 0x35u)) extra = 3;
    else if (g->ix && g->op == 0xcbu) {
        const uint8_t o4 = z->read(z->user, (uint16_t)(g->pc + 3u));
        if (o4 < 0x40u) extra = 4; else if (o4 < 0x80u && (o4 & 7u) != 6u) extra = -8;
    } else if (!g->ix && g->op == 0xedu) {
        const uint8_t o2 = z->read(z->user, (uint16_t)(g->pc + 1u));
        { static int mn = -2, dl = 0;   /* DP_MD_GENS_Z80LDIR: an LDIR that spans two chunk re-entries finishes one iteration (21 clocks) early in Gens; DP_MD_LDRE=min:delta overrides (experiments) */
          if (mn == -2) { const char *e = getenv("DP_MD_LDRE"); mn = e ? atoi(e) : getenv("DP_MD_GENS_Z80LDIR") ? 2 : -1; const char *c = e ? strchr(e, ':') : NULL; dl = c ? atoi(c + 1) : -21; }
          if (mn >= 0 && o2 == 0xb0u && z->pc != g->pc && ld_re >= mn) extra += dl; }
        if ((o2 == 0xb1u || o2 == 0xb9u) && z->pc != g->pc) extra = 2;
        else if (ed_undefined(o2)) extra = -4;
    }
    { static int on = -1, nt = 0; static unsigned tk[8]; static int td[8], tf[8];   /* diagnostic: DP_MD_ZADJ=xxxx:delta[:f],... (xxxx = first two opcode bytes, f = only when the instruction completes) */
      if (on < 0) { on = 0; const char *e = getenv("DP_MD_ZADJ"); while (e && *e && nt < 8) { unsigned k; int d, f = 0, n = 0; if (sscanf(e, "%x:%d%n", &k, &d, &n) < 2) break; e += n; if (*e == ':') { f = 1; ++e; if (*e == 'f') ++e; } tk[nt] = k; td[nt] = d; tf[nt] = f; ++nt; on = 1; if (*e == ',') ++e; else break; } }
      if (on) { const unsigned k2 = ((unsigned)z->read(z->user, g->pc) << 8) | z->read(z->user, (uint16_t)(g->pc + 1u));
        static int fr = -2, fr2 = -3; if (fr == -2) { fr = getenv("DP_MD_ZADJ_FR") ? atoi(getenv("DP_MD_ZADJ_FR")) : -1; fr2 = getenv("DP_MD_ZADJ_FR2") ? atoi(getenv("DP_MD_ZADJ_FR2")) : -3; }
        for (int i = 0; i < nt; ++i) if ((tk[i] >= 0x10000u ? tk[i] == (0x10000u | g->pc) : tk[i] == k2) && (!tf[i] || z->pc != g->pc) && (fr < 0 || (int)z80tab_now == fr || (int)z80tab_now == fr2)) { extra += td[i]; if (getenv("DP_MD_ZADJ_V")) fprintf(stderr, "ZADJ fr=%u pc=%04x d=%d\n", (unsigned)z80tab_now, g->pc, td[i]); } } }
    z->cycles = (uint64_t)((int64_t)z->cycles + extra);
    return extra * 15;
}

/* DP_MD_GENS_Z80LATCH: mdZ80 latches /INT (IntLine) when Gens raises it, 60 Z80 cycles into the V-int line, and keeps it
 * until the Z80 takes it: through DI, a held bus or a stopped Z80; the Z80 itself only drops a one-line level. */
static int gens_latch(void) { static int on = -1; if (on < 0) on = getenv("DP_MD_GENS_Z80LATCH") != NULL; return on; }
static void int_latch(MdFull *m, uint64_t zc) {
    if (!gens_latch()) return;
    if (m->z80_int_at && zc < m->z80_int_at) return;
    if (m->z80_int_at) m->z80_int_latch = 1;
    m->z80_int_at = zc / 59736u * 59736u + 60u;
    if (m->z80_int_at <= zc) m->z80_int_at += 59736u;
}

static void z80_run(MdFull *m, uint64_t until) {
    z80_t *z = &m->sound_cpu;
#if defined(MD_PERF)
    if (m->no_z80) return;
#endif
    const uint64_t target = z80_target(until);
    if (!m->z80_reset || m->z80_bus || z->fault) {
        if (z->cycles < target) z->cycles = target;
        int_latch(m, z->cycles);
        return;
    }
    /* /INT is held for about one line from the start of vertical blank;
     * the frame phase advances by each step's own clocks (a step is far
     * below one frame, so one conditional subtract keeps the modulo). */
    uint32_t at = (uint32_t)(z80_time(m) % FRAME_MASTER);
    static int irq_off = -1;   /* DP_MD_GENS_Z80IRQ: Gens raises /INT 168 Z80 cycles before the end of its V-int line (60 cycles in), not at its start */
    if (irq_off < 0) irq_off = getenv("DP_MD_GENS_Z80IRQ") ? 60 * 15 : 0;
    static int ldadj = -1, ldpend = 0; if (ldadj < 0) ldadj = getenv("DP_MD_LDIRADJ") ? atoi(getenv("DP_MD_LDIRADJ")) : 0;   /* diagnostic */
    if (ldpend) { ldpend = 0; z->cycles = (uint64_t)((int64_t)z->cycles + ldadj); }
    { const int nowld = z->read(z->user, z->pc) == 0xedu && z->read(z->user, (uint16_t)(z->pc + 1u)) == 0xb0u; if (nowld) ++ld_re; else ld_re = 0; }
    while (z->cycles < target && !z->fault) {
        int taken = 0;
        if (gens_latch()) { int_latch(m, z->cycles); z->irq = m->z80_int_latch; taken = z->irq && z->iff1 && !z->ei_delay && !z->nmi; }
        else z->irq = at >= VINT_MASTER + irq_off && at < VINT_MASTER + VINT_LENGTH;
        GensStep gs; const int gens = gens_pre(z, &gs);
        { static int zt = -1; if (zt < 0) zt = getenv("DP_MD_ZTRACE") ? atoi(getenv("DP_MD_ZTRACE")) : 0;   /* diagnostic: Z80 instruction trace of one frame */
          if (zt && m->frames == (unsigned)zt) fprintf(stderr, "ZT %llu %04x hl=%04x de=%04x bc=%04x\n", (unsigned long long)(z->cycles - (uint64_t)m->frames * 59736u), (unsigned)z->pc, (unsigned)z->hl, (unsigned)z->de, (unsigned)z->bc); }
        at += z80_step(z) * 15u;
        if (taken) m->z80_int_latch = 0;
        if (gens) { const int32_t t = (int32_t)at + gens_post(z, &gs); at = (uint32_t)(t < 0 ? t + (int32_t)FRAME_MASTER : t); }
        MD_PERF_ADD(m, p_z80);
        if (at >= FRAME_MASTER) at -= FRAME_MASTER;
    }
    if (ldadj && z->read(z->user, z->pc) == 0xedu && (z->read(z->user, (uint16_t)(z->pc + 1u)) & 0xf7u) == 0xb0u) ldpend = 1;
    z->irq = 0;
}

static void sound_store(MdFull *m, uint16_t addr, uint8_t value, uint64_t at) {
    if (addr < 0x4000u) {
        { static int lg = -1; if (lg < 0) lg = getenv("DP_MD_W2ELOG") ? atoi(getenv("DP_MD_W2ELOG")) : 0;   /* diagnostic: Z80 writes of $2E/$1FFD */
          if (lg && (addr == 0x2eu || addr == 0x1ffdu) && m->frames >= (unsigned)lg && m->frames < (unsigned)lg + 3u)
            fprintf(stderr, "W2E f=%u zc=%llu a=%04x v=%02x\n", (unsigned)m->frames, (unsigned long long)m->sound_cpu.cycles, addr, value); }
        m->zram[addr & 0x1fffu] = value; return; }
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
    if (addr < 0x6000u) { audio_run(m, at); return (uint8_t)(m->ym.status | (!m->gens_dma && at < m->ym.busy_until ? 0x80u : 0u)); }
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
    { static int lz = -1; if (lz < 0) lz = getenv("DP_MD_GENS_ZLAZYW") != NULL;   /* Gens does not run the Z80 before a 68000 write to Z80 RAM */
      if (!(lz && (addr & 0xffffu) < 0x4000u)) z80_run(m, now); }
    sound_store(m, (uint16_t)addr, value, now);
}
uint8_t md_full_sound_read(MdFull *m, uint32_t addr) {
    const uint64_t now = m68k_time(m);
    z80_run(m, now);
    { static FILE *fa = (FILE *)1; static unsigned lf = ~0u;   /* diagnostic: DP_MD_FFDALL=<file>, flag and $2E at each 68000 read of $A01FFD, one line per V-int */
      if (fa == (FILE *)1) fa = getenv("DP_MD_FFDALL") ? fopen(getenv("DP_MD_FFDALL"), "w") : NULL;
      if (fa && (addr & 0xffffu) == 0x1ffdu) { if (m->frames != lf) { fprintf(fa, "%s%u", lf == ~0u ? "" : "\n", (unsigned)m->frames - 294u); lf = m->frames; } fprintf(fa, " %02x%02x:%x", m->zram[0x1ffd], m->zram[0x2e], (unsigned)m->pc); } }
    { static int nf = -1, k = 0; if (nf < 0) nf = getenv("DP_MD_FFDN") ? atoi(getenv("DP_MD_FFDN")) : 0;   /* diagnostic: first N reads of $A01FFD */
      if (nf && (addr & 0xffffu) == 0x1ffdu && k < nf) { fprintf(stderr, "FN %d fr=%u line=%u fc=%u zpc=%04x zc=%llu fl=%02x 2e=%02x sp=%06x r0=%08x r1=%08x ", k++, (unsigned)m->frames, (unsigned)m->line, (unsigned)m->frame_cycles, (unsigned)m->sound_cpu.pc,
          (unsigned long long)m->sound_cpu.cycles, m->zram[0x1ffd], m->zram[0x2e], (unsigned)m->a[7], (unsigned)md_full_read32(m, m->a[7]), (unsigned)md_full_read32(m, m->a[7] + 4u)); for (unsigned a = 0x1ff0u; a < 0x2000u; a++) fprintf(stderr, "%02x", m->zram[a]); fprintf(stderr, "\n"); } }
    { static int zd = -1; if (zd < 0) zd = getenv("DP_MD_ZDUMP") ? atoi(getenv("DP_MD_ZDUMP")) : 0;   /* diagnostic: Z80 RAM at the first $A01FFD read of frame N */
      if (zd > 0 && m->frames == (unsigned)zd) { FILE *f = fopen("zram.bin", "wb"); if (f) { fwrite(m->zram, 1, 0x2000, f); fclose(f); } zd = 0; } }
    { static int lg = -1; if (lg < 0) lg = getenv("DP_MD_FFDLOG") ? atoi(getenv("DP_MD_FFDLOG")) : 0;   /* diagnostic */
      if (lg && (addr & 0xffffu) == 0x1ffdu && m->frames >= (unsigned)lg && m->frames < (unsigned)lg + 3u)
        fprintf(stderr, "FFD f=%u line=%u fc=%u zc=%llu zpc=%04x v=%02x 2e=%02x\n", (unsigned)m->frames, (unsigned)m->line, (unsigned)m->frame_cycles,
                (unsigned long long)(m->sound_cpu.cycles - (uint64_t)m->frames * 59736u), (unsigned)m->sound_cpu.pc, m->zram[0x1ffd], m->zram[0x2e]); }
    return sound_load(m, (uint16_t)addr, now);
}

/* reset_line 0: bus request (A11100), 1: reset (A11200, 1 = running). */
void md_full_sound_control(MdFull *m, unsigned reset_line, uint8_t value) {
    uint64_t now = m68k_time(m);
    static int gstart = -1;   /* DP_MD_GENS_Z80START: Starscream's odometer in a write is the instruction's start; a skip sets the Z80 odometer exactly (may go back) */
    if (gstart < 0) { const char *e = getenv("DP_MD_GENS_Z80START"); gstart = !e || strcmp(e, "0") == 0 ? 0 : *e == 's' ? 2 : *e == 'e' ? 3 : 1; }
    if (gstart) {   /* diagnostic modes: 's' only the skip (Set_Odo) uses the instruction start, 'e' only the catch-up (Exec) */
        const uint32_t spent = (uint32_t)(m->cycles - m->insn_cycles);
        const int skip = !m->z80_reset || m->z80_bus || m->sound_cpu.fault;
        if (gstart == 1 || (gstart == 2 && skip) || (gstart == 3 && !skip)) now -= (uint64_t)spent * 7u;
        { static int rb = -1; if (rb < 0) rb = getenv("DP_MD_RELBACK") ? atoi(getenv("DP_MD_RELBACK")) : 0;   /* diagnostic: a release sets the Z80 clock N 68000 cycles earlier */
          if (rb && skip && !reset_line && !value && !(getenv("DP_MD_RELBACK_FR") && m->frames != (unsigned)atoi(getenv("DP_MD_RELBACK_FR")))) now -= (uint64_t)rb * 7u; }
        { static int qb = 0, qi = 0; if (!qi) { qi = 1; qb = getenv("DP_MD_REQBACK") ? atoi(getenv("DP_MD_REQBACK")) : 0; }   /* diagnostic: a bus request catches the Z80 up to a clock N 68000 cycles earlier (N may be negative) */
          if (qb && !skip && !reset_line && value) now -= (uint64_t)((int64_t)qb * 7); }
        if (skip) { m->sound_cpu.cycles = z80_target(now); int_latch(m, m->sound_cpu.cycles); }
        else z80_run(m, now);
    } else { static int rb = -1; if (rb < 0) rb = getenv("DP_MD_RELBACK") ? atoi(getenv("DP_MD_RELBACK")) : 0;   /* diagnostic: release sets the Z80 clock N 68000 cycles earlier (may go back) */
        const int rel = !reset_line && !value && rb && !(getenv("DP_MD_RELBACK_FR") && m->frames != (unsigned)atoi(getenv("DP_MD_RELBACK_FR")));
        z80_run(m, now);
        if (getenv("DP_MD_RELBACK_V") && m->frames >= 12760u && m->frames < 12770u) fprintf(stderr, "CTL rl=%u v=%u rb=%d rel=%d\n", reset_line, (unsigned)value, rb, rel);
        if (rel && getenv("DP_MD_RELBACK_V")) fprintf(stderr, "REL f=%u zc=%llu\n", (unsigned)m->frames, (unsigned long long)m->sound_cpu.cycles);
        if (rel && !m->sound_cpu.fault) { m->sound_cpu.cycles = z80_target(now - (uint64_t)rb * 7u); int_latch(m, m->sound_cpu.cycles); } }
    { static int zd2 = -1, zn = 0, zk = 0; if (zd2 < 0) { zd2 = getenv("DP_MD_ZDUMPREQ") ? atoi(getenv("DP_MD_ZDUMPREQ")) : 0; zn = getenv("DP_MD_ZDUMPN") ? atoi(getenv("DP_MD_ZDUMPN")) : 0; }   /* diagnostic: Z80 RAM at the Nth bus request of frame */
      if (zd2 > 0 && !reset_line && value && m->frames == (unsigned)zd2 && zk++ == zn) { FILE *f = fopen("zramreq.bin", "wb"); if (f) { fwrite(m->zram, 1, 0x2000, f); fclose(f); } } }
    { static int lg = -1; if (lg < 0) lg = getenv("DP_MD_FFDLOG") ? atoi(getenv("DP_MD_FFDLOG")) : 0;   /* diagnostic */
      static unsigned ln = 0; if (!ln) ln = getenv("DP_MD_FFDLOG_N") ? (unsigned)atoi(getenv("DP_MD_FFDLOG_N")) : 2u;
      if (lg && m->frames >= (unsigned)lg && m->frames < (unsigned)lg + ln)
        fprintf(stderr, "BUS f=%u line=%u fc=%u ins=%u sp=%u r=%u v=%u zc=%llu zpc=%04x pc=%06x\n", (unsigned)m->frames, (unsigned)m->line, (unsigned)m->frame_cycles, (unsigned)m->insn_cycles, (unsigned)(m->cycles - m->insn_cycles), reset_line, value,
                (unsigned long long)(m->sound_cpu.cycles - (uint64_t)m->frames * 59736u), (unsigned)m->sound_cpu.pc, (unsigned)m->pc); }
    if (!reset_line) {
        if (value) { m->busreq_st = !m->z80_bus; m->busreq_cnt = m->insn_cycles; }   /* Gens: bus granted CYCLE_FOR_TAKE_Z80_BUS_GENESIS (16) cycles after the request */
        m->z80_bus = value;
        return;
    }
    if (value && !m->z80_reset) {
        const uint64_t cycles = m->sound_cpu.cycles;
        z80_reset(&m->sound_cpu);
        m->sound_cpu.cycles = cycles;
        m->z80_int_latch = 0;
    } else if (!value && m->z80_reset) {
        /* The YM2612 shares the Z80 reset line. */
        audio_run(m, now);
        md_ym_reset(&m->ym);
    }
    m->z80_reset = value;
}

/* Gens runs the Z80 up to the end of every line (when running), whatever the 68000 touched.  DP_MD_GENS_Z80LINE=1. */
void md_full_sound_line(MdFull *m, unsigned line) {
    static int on = -1;
    if (on < 0) on = getenv("DP_MD_GENS_Z80LINE") != NULL;
    if (on) z80_run(m, (uint64_t)m->frames * FRAME_MASTER + (uint64_t)(line + 1u) * 3420u);
    { static int zl = -1; if (zl < 0) zl = getenv("DP_MD_ZLL") ? atoi(getenv("DP_MD_ZLL")) : 0;   /* diagnostic: Z80 state after each line run */
      if (zl && m->frames == (unsigned)zl) fprintf(stderr, "ZLL line=%u zc=%llu pc=%04x\n", line, (unsigned long long)(m->sound_cpu.cycles - (uint64_t)m->frames * 59736u), (unsigned)m->sound_cpu.pc); }
}

/* Gens starts its frame at active line 0 (fc `at` here): Cycles_Z80 = 0, z80_Clear_Odo and Last_BUS_REQ_Cnt = -1000, so the Z80's
 * overshoot past its last line end is dropped (the 68000's is dropped by the caller, main68k_tripOdometer). */
void md_full_gens_trip(MdFull *m, uint32_t at) {
    const uint64_t now = (uint64_t)m->frames * FRAME_MASTER + (uint64_t)at * 7u;
    z80_run(m, now);
    if (!m->sound_cpu.fault) m->sound_cpu.cycles = z80_target(now);
    m->busreq_cnt = m->insn_cycles - 1000u;
}

void md_full_sound_frame(MdFull *m) {
    z80tab_now = m->frames;
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
