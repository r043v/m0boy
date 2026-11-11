#ifndef M0_TL
#define M0_TL   /* deepeel builds a thread-local flavour with -DM0_TL=__thread */
#endif
#include "sg1000.h"
static inline uint8_t sg_native_read(z80_t *c, uint16_t a) { (void)c; return sg1000_read(a); }
static inline void sg_native_write(z80_t *c, uint16_t a, uint8_t v) { (void)c; sg1000_write(a, v); }
#define Z80_NATIVE_READ sg_native_read
#define Z80_NATIVE_WRITE sg_native_write
#include "z80_ops.h"
/* Opcode fetch as the generic decoder sees it: mapped, never a data read. */
#if defined(SG_MSX_TRACE)
extern M0_TL int msx_in_fetch;
static inline uint8_t sg_fetch(uint16_t a) { msx_in_fetch = 1; const uint8_t v = sg1000_read(a); msx_in_fetch = 0; return v; }
#define Z80_FAST_FETCH(a) sg_fetch((a))
#else
#define Z80_FAST_FETCH(a) sg1000_read((a))
#endif
#include "z80_fast.h"
#include <string.h>
#if SG_MSX
#include "m0_msx_layout.h"
#endif

M0_TL sg1000_t machine;

#if SG_MSX
/* Profiling location of a pc: banked cartridges count page << 16 | pc (the
 * recompiler's Loc), the others the address; ~0 outside the cartridge. */
static uint32_t msx_location(uint16_t pc) {
    if (!machine.msx_mapper) return pc;
    const unsigned w = pc >> 13;
    return pc >= 0x4000 && pc < 0xc000 && (machine.cart_vis >> w & 1) ? (uint32_t)machine.page[w] << 16 | pc : ~0u;
}
#endif

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons the game receives (player 1, pause),
 * so that a session played in gm0 replays in the host harness (--input).
 * sg1000_reset references it: --gc-sections would drop it otherwise. */
#ifndef M0_NO_INPUT_PROBE
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    sg_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.buttons[0], &machine.pause };
#endif

M0_TL uintptr_t sg_stack_limit;

const uint32_t sg1000_palette[16] = {
    0x000000, 0x000000, 0x21c842, 0x5edc78, 0x5455ed, 0x7d76fc, 0xd4524d, 0x42ebf5,
    0xfc5554, 0xff7978, 0xd4c154, 0xe6ce80, 0x21b03b, 0xc95bba, 0xcccccc, 0xffffff
};

/* ---- bus ------------------------------------------------------------------ */

static uint8_t bus_read(void *u, uint16_t a) { (void)u; return sg1000_read(a); }
static void bus_write(void *u, uint16_t a, uint8_t v) { (void)u; sg1000_write(a, v); }
static uint8_t bus_in(void *u, uint16_t p) { (void)u; return sg1000_in((uint8_t)p); }
static void bus_out(void *u, uint16_t p, uint8_t v) { (void)u; sg1000_out((uint8_t)p, v); }

#if !SG_MSX
void sg1000_write_cart(uint16_t a, uint8_t v) {
    machine.cart_ram_used = 1;
    if (machine.cart_ram) machine.cart_ram[a & (SG_CART_RAM_SIZE - 1)] = v;
}
#endif

/* Next device event; an acceptable interrupt makes it now. */
static void schedule(void) {
    sg1000_t *m = &machine;
    z80_t *c = &m->cpu;
    m->next_evt = m->frame_start + (m->vblank_done ? SG_FRAME_CYCLES : SG_VBLANK_CYCLES);
    if (c->nmi || (c->irq && c->iff1 && !c->ei_delay)) m->next_evt = m->cyc;
}

void sg_irq_update(void) {
    sg1000_t *m = &machine;
    m->cpu.irq = (m->vdp_status & 128) && (m->vdp[1] & 32);
    if (m->cpu.irq && m->cpu.iff1) m->next_evt = m->cyc;
}

#if SG_MSX
/* The MSX decodes its own ports (msx.c); the VDP below is this file's,
 * reached through these names. */
#define sg1000_in msx_vdp_in
#define sg1000_out msx_vdp_out
#endif
/* A write to the VDP's data port: the lines that ended are drawn first (the
 * call only when one has), then the byte goes in. */
static inline void vdp_data_write(uint8_t value) {
    sg1000_t *m = &machine;
    m->vdp_pending = 0;
    if (m->render_line < SG_HEIGHT && (int32_t)(m->cyc - m->render_at) >= 0) sg1000_vdp_catchup();
    const unsigned address = m->vdp_address, entry = (uint16_t)(address - m->sat_base);
    if (entry < 128 && !(entry & 3)) sg1000_sprite_y(entry >> 2, m->vram[address], value);
    m->vram[address] = value;
    m->vdp_buffer = value;
    m->vdp_address = (uint16_t)((address + 1) & 0x3fff);
}
#if SG_MSX
void msx_vdp_data(uint8_t value) { vdp_data_write(value); } /* msx.c: port 98 without the decoding */
#endif

uint8_t sg1000_in(uint8_t port) {
    sg1000_t *m = &machine;
    if (port >= 0x80 && port < 0xc0) {
        m->vdp_pending = 0;
        if (port & 1) { /* status: flags of every line that already ended */
            sg1000_vdp_catchup();
            uint8_t v = m->vdp_status;
            m->vdp_status &= 31;
            sg_irq_update();
            return v;
        }
        uint8_t v = m->vdp_buffer;
        m->vdp_buffer = m->vram[m->vdp_address];
        m->vdp_address = (m->vdp_address + 1) & 0x3fff;
        return v;
    }
    if (port >= 0xc0) {
        if (!(port & 1)) return (uint8_t)~(m->buttons[0] | ((m->buttons[1] & 3) << 6));
        return (uint8_t)(0xf0 | (~(m->buttons[1] >> 2) & 15));
    }
    return 0xff;
}

void sg1000_out(uint8_t port, uint8_t value) {
    sg1000_t *m = &machine;
    if (port >= 0x40 && port < 0x80) {
        sg1000_audio_catchup();
        if (value & 128) m->psg_latch = (value >> 4) & 7;
        unsigned r = m->psg_latch, ch = r >> 1;
        if (r & 1) m->psg_volume[ch] = value & 15;
        else if (ch == 3) { m->psg_noise = value & 7; m->audio_lfsr = 0x10000; m->audio_count[3] = 0; m->audio_output[3] = 0; }
        else if (value & 128) m->psg_tone[ch] = (uint16_t)((m->psg_tone[ch] & 0x3f0) | (value & 15));
        else m->psg_tone[ch] = (uint16_t)((m->psg_tone[ch] & 15) | ((value & 63) << 4));
    } else if (port >= 0x80 && port < 0xc0) {
        if (!(port & 1)) {
            vdp_data_write(value);
        } else if (!m->vdp_pending) {
            m->vdp_latch = value;
            m->vdp_pending = 1;
        } else {
            m->vdp_pending = 0;
            if (value & 128) {
                sg1000_vdp_catchup();
                const unsigned r = value & 7, before = m->vdp[r];
                m->vdp[r] = m->vdp_latch;
                m->sat_base = (uint16_t)((m->vdp[5] & 127u) << 7);
                /* the sprite lines depend on the size (register 1) and the table */
                if ((r == 1 && ((before ^ m->vdp_latch) & 3)) || (r == 5 && before != m->vdp_latch)) m->sprites_dirty = 1;
                sg_irq_update();
            } else {
                m->vdp_address = (uint16_t)(((value & 63) << 8) | m->vdp_latch);
                if (!(value & 64)) { /* read setup: prefetch */
                    m->vdp_buffer = m->vram[m->vdp_address];
                    m->vdp_address = (m->vdp_address + 1) & 0x3fff;
                }
            }
        }
    }
}

#if SG_MSX
#undef sg1000_in
#undef sg1000_out
#endif

/* ---- machine -------------------------------------------------------------- */

void sg1000_init(const uint8_t *rom, size_t size, uint8_t *cart_ram) {
    sg1000_t *m = &machine;
    sg_fn (*lookup)(uint16_t) = m->lookup;
    memset(m, 0, sizeof *m);
    m->lookup = lookup;
    m->rom = rom;
    m->rom_size = (uint32_t)size;
    /* Power-of-two boards mirror ROM through the cartridge window; irregular
     * 40/48 KiB dumps map directly and read FF past their end. */
    m->rom_mask = size && !(size & (size - 1)) ? (uint32_t)size - 1 : 0;
#if SG_MSX
    (void)cart_ram;
    m->cart_ram = 0;
    {
#ifdef SG_MSX_MAPPER /* generated projects: fixed at generation (the ROM may not be in memory) */
        m->msx_base = SG_MSX_BASE;
        m->msx_init = SG_MSX_INIT;
        m->msx_mapper = SG_MSX_MAPPER;
#else
        const m0_msx_layout_t layout = m0_msx_layout(rom, size);
        m->msx_base = layout.base;
        m->msx_init = layout.init;
        m->msx_mapper = (uint8_t)layout.mapper;
#endif
        if (msx_mapper_forced() >= 0) m->msx_mapper = (uint8_t)msx_mapper_forced();
        m->rom_units = (uint16_t)((size + M0_MSX_PAGE - 1) / M0_MSX_PAGE);
    }
#else
    /* Cartridge RAM sits at 8000-BFFF only on boards of up to 32 KiB of
     * ROM; 40/48 KiB cartridges have ROM there. */
    m->cart_ram = size <= 0x8000 ? cart_ram : 0;
#endif
    for (unsigned i = 0; i < 16; i++) {
        uint32_t rgb = sg1000_palette[i];
        m->palette[i] = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
    }
    sg1000_palette_changed();
    m->cpu.user = m;
    m->cpu.read = bus_read; m->cpu.write = bus_write;
    m->cpu.in = bus_in; m->cpu.out = bus_out;
    sg1000_reset();
}

void sg1000_reset(void) {
#ifndef M0_NO_INPUT_PROBE
    __asm__ volatile("" : : "r"(&sg_input_probe));
#endif
    sg1000_t *m = &machine;
    z80_t keep = m->cpu;
    z80_reset(&m->cpu);
    m->cpu.user = keep.user; m->cpu.read = keep.read; m->cpu.write = keep.write;
    m->cpu.in = keep.in; m->cpu.out = keep.out;
    memset(m->ram, 0, sizeof m->ram);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->vdp, 0, sizeof m->vdp);
    if (m->cart_ram) memset(m->cart_ram, 0, SG_CART_RAM_SIZE);
    m->cyc = m->frame_start = 0;
    m->render_line = 0;
    m->render_at = SG_LINE_CYCLES;
    m->vblank_done = 0;
    m->frames = 0;
    m->vdp_address = 0;
    m->vdp_pending = m->vdp_latch = m->vdp_buffer = 0;
    m->vdp_status = 31;
    m->sat_base = 0;
    m->sprites_dirty = 1;
    m->pause = 0;
    memset(m->buttons, 0, sizeof m->buttons);
    memset(m->psg_tone, 0, sizeof m->psg_tone);
    memset(m->psg_volume, 15, sizeof m->psg_volume);
    m->psg_noise = m->psg_latch = 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = 0;
    m->audio_head = m->audio_tail = 0;
    m->audio_lfsr = 0x10000;
    memset(m->audio_count, 0, sizeof m->audio_count);
    memset(m->audio_output, 0, sizeof m->audio_output);
    m->interpreted = m->compiled = 0;
#if SG_MSX
    memset(m->ay_reg, 0, sizeof m->ay_reg);
    msx_boot();
#endif
    schedule();
}

void sg1000_buttons(unsigned player, uint8_t pressed) {
    if (player < 2) machine.buttons[player] = pressed & 63;
}

void sg1000_pause(int pressed) {
#if SG_MSX
    machine.pause = !!pressed; /* RETURN on the keyboard matrix: the MSX has no NMI */
    return;
#endif
    if (pressed && !machine.pause) { machine.cpu.nmi = 1; machine.next_evt = machine.cyc; }
    machine.pause = !!pressed;
}

/* ---- events and interrupts ------------------------------------------------ */

void sg_sync(void) {
    sg1000_t *m = &machine;
    for (;;) {
        if (!m->vblank_done) {
            if ((int32_t)(m->cyc - (m->frame_start + SG_VBLANK_CYCLES)) < 0) break;
            sg1000_vdp_catchup(); /* the 192 visible lines have ended */
            m->vblank_done = 1;
            m->vdp_status |= 128;
            m->cpu.irq = !!(m->vdp[1] & 32);
        } else {
            if ((int32_t)(m->cyc - (m->frame_start + SG_FRAME_CYCLES)) < 0) break;
            sg1000_audio_catchup();
            m->frame_start += SG_FRAME_CYCLES;
            m->vblank_done = 0;
            m->render_line = 0;
            m->render_at = m->frame_start + SG_LINE_CYCLES;
            m->frames++;
            if (m->frame) m->frame(m->platform);
        }
    }
    schedule();
}

/* Take an NMI or an acceptable IRQ: push pc, jump to the vector. */
static int irq_enter(void) {
    sg1000_t *m = &machine;
    z80_t *c = &m->cpu;
    unsigned cycles;
    if (c->nmi) {
        c->nmi = 0;
        c->iff2 = c->iff1; c->iff1 = 0;
        z80_push(c, c->pc);
        if (m->entries && m->irq_depth < 8) m->irq_sp[m->irq_depth++] = c->sp;
        c->pc = 0x66;
        cycles = 11;
    } else if (c->irq && c->iff1 && !c->ei_delay) {
        c->iff1 = c->iff2 = 0;
        if (c->im == 0 && (c->irq_vector & 0xc7) != 0xc7) { c->fault = 1; m->stop = 1; return 0; }
        z80_push(c, c->pc);
        if (m->entries && m->irq_depth < 8) m->irq_sp[m->irq_depth++] = c->sp;
        c->pc = c->im == 2 ? z80_word(c, (uint16_t)((c->i << 8) | c->irq_vector))
              : c->im == 0 ? (uint16_t)(c->irq_vector & 0x38) : 0x38;
        cycles = c->im == 2 ? 19 : 13;
    } else return 0;
    c->halted = 0;
    c->r = (uint8_t)((c->r & 128) | ((c->r + 1) & 127));
    m->cyc += cycles;
    schedule();
    return 1;
}

int sg_event(uint16_t resume_pc) {
    sg1000_t *m = &machine;
    for (;;) {
        sg_sync();
        m->cpu.pc = resume_pc;
        if (m->stop) return 1;
        if (!irq_enter()) return 0;
        sg_exec();
        if (m->cpu.pc != resume_pc || m->stop) return 1;
    }
}

/* HALT: whole 4-cycle refresh periods up to each device event, until an
 * interrupt can be taken (never, with interrupts off: the CPU stays here). */
void sg_halt(void) {
    sg1000_t *m = &machine;
    z80_t *c = &m->cpu;
    for (;;) {
        sg_sync();
        if (m->stop || c->nmi || (c->irq && c->iff1 && !c->ei_delay)) return;
        uint32_t ticks = (m->next_evt - m->cyc + 3) / 4;
        m->cyc += ticks * 4;
        c->r = (uint8_t)((c->r & 128) | ((c->r + ticks) & 127));
    }
}

/* ---- dispatch ------------------------------------------------------------- */

/* Nested loops need this much C stack: regions they call must pass their
 * entry guard, or no progress is made. */
#define SG_EXEC_HEADROOM 768u

static void dispatch_one(int compiled) {
    sg1000_t *m = &machine;
    if (m->cpu.halted) { /* resumed in HALT (after a stop) */
        sg_halt();
        if (!m->stop) (void)sg_event(m->cpu.pc);
        return;
    }
    if (m->cpu.ei_delay) { /* resumed inside an EI delay: the interpreter ends it */
        sg_step(m->cpu.pc);
        return;
    }
    sg_fn fn = compiled && m->lookup ? m->lookup(m->cpu.pc) : 0;
    if (fn) { m->compiled++; fn(); }
    else sg_interp_block();
}

void sg_exec(void) {
    char probe;
    if ((uintptr_t)&probe < sg_stack_limit + SG_EXEC_HEADROOM) return; /* callers unwind */
    uint16_t sp0 = machine.cpu.sp;
    do {
        if (machine.stop) return;
        dispatch_one(1);
    } while ((int16_t)(machine.cpu.sp - sp0) <= 0);
}

void sg1000_run(void) {
    char probe;
    if (!sg_stack_limit) sg_stack_limit = (uintptr_t)&probe - 4096u;
    int room = (uintptr_t)&probe >= sg_stack_limit + SG_EXEC_HEADROOM;
    while (!machine.stop) dispatch_one(room);
}

/* One interpreted instruction, after a device sync; 0: stop the block
 * (control transfer, HALT, stop or fault). */
/* Profiling: a RET, RETI/RETN or JP (HL/IX/IY) went to pc; returns from
 * interrupts (arbitrary points) are left out.  The recompiler makes these
 * region entries (pushed return addresses, jump tables). */
static void note_dynamic(const z80_decoded_t *d, uint16_t sp, uint16_t pc) {
    sg1000_t *m = &machine;
    const unsigned op = d->opcode;
    const int ret = d->group == 0 ? op == 0xc9 || (op & 0xc7) == 0xc0 : d->group == 2 && (op & 0xc7) == 0x45;
    if (!ret && !(d->group == 0 && op == 0xe9)) return;
    while (m->irq_depth && sp > m->irq_sp[m->irq_depth - 1]) m->irq_depth--; /* unwound */
    if (ret && m->irq_depth && sp == m->irq_sp[m->irq_depth - 1]) { m->irq_depth--; return; }
#if SG_MSX
    {
        const uint32_t loc = msx_location(pc);
        if (loc < m->coverage_size) m->entries[loc] = 1;
    }
#else
    m->entries[pc] = 1;
#endif
}

/* The instructions the fast path declines: decoded and executed generically. */
static __attribute__((noinline)) int interp_generic(uint16_t pc) {
    sg1000_t *m = &machine;
    z80_t *c = &m->cpu;
    z80_decoded_t d;
#ifdef SG_MSX_TRACE
    msx_in_fetch = 1;
#endif
    const int decoded = z80_decode(c, &d);
#ifdef SG_MSX_TRACE
    msx_in_fetch = 0;
#endif
    if (!decoded) { c->fault = 1; m->stop = 1; return 0; }
    const uint16_t sp = c->sp;
    m->cyc += z80_execute(c, &d);
    m->interpreted++;
    if (m->entries && c->pc != (uint16_t)(pc + d.length)) note_dynamic(&d, sp, c->pc);
    if (c->ei_delay && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
    if (c->halted) {
        sg_halt();
        if (!m->stop) (void)sg_event(c->pc);
        return 0;
    }
    return c->pc == (uint16_t)(pc + d.length);
}

/* One instruction: 0 ends the block (a control transfer, HALT, an event that
 * moved pc, a stop).  Inlined into the block loop: the registers it works
 * with stay in registers from one instruction to the next. */
static inline __attribute__((always_inline)) int interp_step(void) {
    sg1000_t *m = &machine;
    z80_t *c = &m->cpu;
    if (__builtin_expect((int32_t)(m->cyc - m->next_evt) >= 0, 0) && sg_event(c->pc)) return 0;
    const uint16_t pc = c->pc;
    if (m->coverage) {
#if SG_MSX
        const uint32_t loc = msx_location(pc);
        if (loc < m->coverage_size) m->coverage[loc]++;
#else
        m->coverage[pc]++;
#endif
    }
    int jumped;
    const int fast = z80_fast(c, &jumped); /* common opcodes without decoding */
    if (fast >= 0) {
        c->r = (uint8_t)((c->r & 128) | ((c->r + 1) & 127));
        m->cyc += (uint32_t)fast;
        m->interpreted++;
        if (__builtin_expect(c->ei_delay != 0, 0) && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
        return !jumped;
    }
    return interp_generic(pc);
}

/* The block loop is the one place the step is inlined: a second copy would double the fast path's flash. */
static __attribute__((noinline)) unsigned interp_run(unsigned limit) {
    unsigned n = 0;
    while (n < limit) { n++; if (!interp_step()) break; }
    return n;
}
static void interp_one(void) { (void)interp_run(1); }

/* Interpreter: until a control transfer, or 32 instructions. */
void sg_interp_block(void) {
    (void)interp_run(32);
}

/* The instruction after EI, in the interpreter; again while it was itself
 * an EI (the delay restarts), so generated code resumes with no delay. */
int sg_step(uint16_t after) {
    do interp_one();
    while (machine.cpu.ei_delay && !machine.stop && !machine.cpu.fault);
    return machine.cpu.pc == after && !machine.stop;
}

uint32_t sg_state_bytes(void) {
    return (uint32_t)sizeof(sg1000_t) + (machine.cart_ram ? SG_CART_RAM_SIZE : 0u);
}

void sg_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load) {
    if (!load) {
        io(&machine, (uint32_t)sizeof(sg1000_t), ctx);
        if (machine.cart_ram) io(machine.cart_ram, SG_CART_RAM_SIZE, ctx);
        return;
    }
    const uint8_t *rom = machine.rom;
    uint8_t *cart = machine.cart_ram;
    uint16_t *row_out = machine.row_out;
    void *platform = machine.platform;
    int (*draw_line)(void *, unsigned y) = machine.draw_line;
    void (*row)(void *, unsigned y, uint16_t *pixels) = machine.row;
    void (*ref_row)(void *, unsigned y, const uint8_t *pixels) = machine.ref_row;
    void (*frame)(void *) = machine.frame;
    sg_fn (*lookup)(uint16_t pc) = machine.lookup;
    uint32_t *coverage = machine.coverage;
    uint8_t *entries = machine.entries;
    z80_read_fn read = machine.cpu.read, in = machine.cpu.in;
    z80_write_fn write = machine.cpu.write, out = machine.cpu.out;
    io(&machine, (uint32_t)sizeof(sg1000_t), ctx);
    if (cart) io(cart, SG_CART_RAM_SIZE, ctx);
    machine.rom = rom;
    machine.cart_ram = cart;
    machine.row_out = row_out;
    machine.platform = platform;
    machine.draw_line = draw_line;
    machine.row = row;
    machine.ref_row = ref_row;
    machine.frame = frame;
    machine.lookup = lookup;
    machine.coverage = coverage;
    machine.entries = entries;
    machine.cpu.user = &machine;
    machine.cpu.read = read;
    machine.cpu.in = in;
    machine.cpu.write = write;
    machine.cpu.out = out;
    machine.stop = 0; /* the saved run's end must not end this one */
    machine.sprites_dirty = 1; /* the renderer's caches follow the loaded VRAM */
#if SG_MSX
    msx_state_loaded();
#endif
    sg1000_palette_changed();
}

uint32_t sg_state_frame(void) { return machine.frames; }
