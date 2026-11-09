#include "gg.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef GG_ROM_TRACE /* host analysis: ROM bytes read as data, interpreted instruction bytes, cartridge RAM */
void gg_rom_trace(uint32_t offset);
void gg_rom_trace_fetch(uint32_t offset);
void gg_rom_trace_insn(uint32_t offset); /* an instruction starts: its bytes are traced unless compiled */
void gg_cart_trace(uint32_t offset, unsigned write);
static inline int gg_rom_offset(uint16_t a, uint32_t *offset) {
    if (a < 0x400 && GG_FIXED_KIB) *offset = a;
    else if (a < 0xc000 && (GG_KOREAN150 || !(a >= 0x8000 && (machine.ram_ctl & 8))))
        *offset = ((uint32_t)machine.page[a >> GG_BANK_SHIFT] << GG_BANK_SHIFT) + (a & GG_BANK_MASK);
    else return 0;
    return 1;
}
/* gg_read and RD trace each read (gg_trace_read): an instruction byte while
 * the interpreter fetches, a data read otherwise. */
static unsigned trace_fetching;
void gg_trace_read(uint16_t a) {
    if (!machine.coverage) return;
    uint32_t offset;
    if (gg_rom_offset(a, &offset)) {
        if (trace_fetching) gg_rom_trace_fetch(offset);
        else gg_rom_trace(offset);
    } else if (a >= 0x8000) gg_cart_trace(gg_cart_offset(a, machine.ram_ctl), 0);
}
/* Each opcode and operand byte the interpreter fetches: one firmware read
 * each (gg_read). */
static inline uint8_t gg_fetch(uint16_t a) {
    trace_fetching = 1;
    const uint8_t v = gg_read(a);
    trace_fetching = 0;
    return v;
}
#else
#define gg_fetch gg_read
#endif
static inline uint8_t gg_native_read(z80_t *c, uint16_t a) { (void)c; return gg_read(a); }
static inline void gg_native_write(z80_t *c, uint16_t a, uint8_t v) { (void)c; gg_write(a, v); }
#define Z80_NATIVE_READ gg_native_read
#define Z80_NATIVE_WRITE gg_native_write
#include "z80_ops.h"
/* Opcode fetch as the generic decoder sees it (bus_read): mapped by the
 * mapper, never traced as a data read (gg_rom_trace). */
#define Z80_FAST_FETCH(a) gg_fetch((a))
#include "z80_fast.h"
#include <string.h>
#ifdef GG_POOL_PACKED
#include "m0_rom_codec.h"
#endif

gg_t machine;

/* bench watch: log every write to this work-RAM address (GG_WATCH_ADDR=hex) */
int gg_watch_addr = -1;
static void gg_watch_addr_init(void) {
    const char *w = getenv("GG_WATCH_ADDR");
    if (w) { gg_watch_addr = (int)strtoul(w, 0, 16); fprintf(stderr, "watch enabled: %04x\n", gg_watch_addr); }
}
int gg_watch_dump(unsigned a, uint8_t v, unsigned pc, uint32_t cyc, unsigned fr) {
    return fprintf(stderr, "watch %04x <- %02x pc=%04x cyc=%u fr=%u\n", a, v, pc, cyc, fr);
}

/* Read by gm0 --record-input, which finds it by its tag in the firmware:
 * the frame counter and the buttons the game receives, so that a session
 * played in gm0 replays in the host harness (--input).  gg_reset
 * references it: --gc-sections would drop it otherwise. */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    gg_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.buttons, 0 };

uintptr_t sg_stack_limit;

/* ---- bus ------------------------------------------------------------------ */

static uint8_t bus_read(void *u, uint16_t a) { (void)u; return gg_fetch(a); }
static void bus_write(void *u, uint16_t a, uint8_t v) { (void)u; gg_write(a, v); }
static uint8_t bus_in(void *u, uint16_t p) {
    (void)u;
    static int btrace = -1;
    if (btrace < 0) btrace = getenv("GG_IN_TRACE") ? 1 : 0;
    if (btrace && (p & 0xc0) == 0xc0)
        fprintf(stderr, "bus_in %04x buttons=%02x ctl=%02x pc=%04x\n", p, machine.buttons, machine.io_ctl, machine.cpu.pc);
    return gg_in((uint8_t)p);
}
static void bus_out(void *u, uint16_t p, uint8_t v) { (void)u; gg_out((uint8_t)p, v); }

static void map_slot(unsigned s, uint8_t value) {
    gg_t *m = &machine;
    /* power-of-two page counts mirror with a mask; others (rare) divide */
    const unsigned n = m->rom_pages;
    const unsigned page = !n ? 0 : (n & (n - 1)) ? value % n : value & (n - 1);
    if (m->page[s] != page || (s == 2 && m->page2 != page)) { /* generated code checks at its next sync */
        m->remapped = 1;
        m->next_evt = m->cyc;
    }
    m->page[s] = (uint8_t)page;
    if (s == 2) {
        m->page2 = (uint8_t)page;
        if (m->ram_ctl & 8) m->page[2] = 0xff;
    }
#if GG_STREAM
    const unsigned index = gg_page_index[page];
    m->slot[s] = index == 0xff ? 0 : m->rom + (size_t)index * 0x4000;
#else
    m->slot[s] = m->rom + (size_t)page * 0x4000;
#endif
}

/* All six windows can move together, including the interrupt vectors.
 * page2 is the FFF3 bank register, ram_ctl the FFFC mode register. */
static void korean150_remap(void) {
    gg_t *m = &machine;
    if (!GG_KOREAN150) return;
    const unsigned count = m->rom_pages * 2u;
    for (unsigned s = 0; s < GG_SLOTS; s++) {
        unsigned page = m0_korean150_page(m->ram_ctl, m->page2, s);
        if (page >= count) page = count ? page % count : 0; /* no division on Cortex-M0+ in the common case */
        m->page[s] = (uint8_t)page;
#if GG_STREAM
        const unsigned index = gg_page_index[page >> 1];
        m->slot[s] = index == 0xffu ? 0 : m->rom + (size_t)index * 0x4000u + (page & 1u) * 0x2000u;
#else
        m->slot[s] = m->rom + (size_t)page * 0x2000u;
#endif
    }
}

/* The slots' pointers again, after the pages changed behind our back (a save
 * state's load): slot 2 shows the ROM page behind the cartridge RAM. */
void gg_remap(void) {
    if (GG_KOREAN150) { korean150_remap(); return; }
    for (unsigned s = 0; s < 3; s++) {
        const uint8_t pg = s == 2 && (machine.ram_ctl & 8) ? machine.page2 : machine.page[s];
#if GG_STREAM
        const unsigned index = pg < machine.rom_pages ? gg_page_index[pg] : 0xffu;
        machine.slot[s] = index == 0xffu ? 0 : machine.rom + (size_t)index * 0x4000u;
#else
        machine.slot[s] = machine.rom + (size_t)pg * 0x4000u;
#endif
    }
}

#if GG_STREAM || GG_CART_ON_CARD
/* The card cache shared with the Game Boy (m0_cache.h): ROM sectors that
 * are not mapped directly (on the card, or LZSS-coded in the pool) and the
 * cartridge RAM's save file, written back when dirty. */
#define M0C_BLOCK GG_CACHE_BLOCK
#define M0C_SHIFT (GG_CACHE_BLOCK == 16 ? 4 : GG_CACHE_BLOCK == 32 ? 5 : GG_CACHE_BLOCK == 64 ? 6 : GG_CACHE_BLOCK == 128 ? 7 : GG_CACHE_BLOCK == 256 ? 8 : 9)
#define M0C_WAYS (GG_CACHE_BYTES / GG_CACHE_BLOCK)
#if GG_CACHE_DYN
#define M0C_DYN 1
#define M0C_MAX_WAYS (GG_CACHE_MAX_BYTES / GG_CACHE_BLOCK)
#define M0C_BUF_ATTR __attribute__((section(".cachepool"))) /* last in RAM, under the stack */
#define M0C_LOADS gg_cache_loads
#endif
#define M0C_BUF card_buf     /* Pokitto: in the USB RAM bank (pokitto.ld) */
#define M0C_TAG card_tag
#define M0C_ROM_LAST card_rom_last
#define M0C_MISSES gg_stream_misses
#define M0C_SAVE GG_CART_ON_CARD
/* The option menu runs on the lent blocks (sg_menu.h): the emulation is
 * stopped, so nothing reads the ROM or the save while they are lent out. */
#define M0C_LEND 1
#define M0C_LEND_KEEP 0
#include "m0_cache.h"

uint8_t *gg_stream_scratch(void) { return card_buf[0]; }

#if GG_STREAM
/* ROM sector sec into dst: from the pool when it is coded there, else from
 * the card.  Counted for meta-usb.py: sectors decoded, sectors read. */
uint32_t gg_stream_unpacked, gg_stream_reads;
static int rom_sector(uint32_t sec, uint8_t *dst) {
#if defined(GG_POOL_SECTORS)
    if (gg_pool_index[sec] != GG_POOL_NONE) {
#ifdef GG_POOL_PACKED
        const uint32_t record = gg_pool_records[gg_pool_index[sec]];
        const uint8_t *src = gg_pool + (record & 0x3fffffffu);
        if (record >> 30) m0_rom_unpack512(src, dst);
        else M0_COPY(512, dst, src); /* records follow packed ones: any alignment */
        gg_stream_unpacked++;
#else
        /* the pool is raw: a whole sector, copied */
        M0_COPY(512, dst, gg_pool + ((size_t)gg_pool_index[sec] << 9));
#endif
        return 1;
    }
#endif
    gg_stream_reads++;
    return gg_platform_rom_read(sec, dst);
}
#define M0C_ROM_SECTOR(sec, dst) rom_sector((sec), (dst))
#else
#define M0C_ROM_SECTOR(sec, dst) 0
#endif

#if GG_CART_ON_CARD
static uint8_t save_pending;
static uint32_t save_change;
#define M0C_SAVE_READ(sec, dst) gg_platform_save_read((sec), (dst))
#define M0C_SAVE_WRITE(sec, src) gg_platform_save_write((sec), (src))
#define M0C_PENDING save_pending
#define M0C_NOTE_CHANGE() (save_change = machine.frames)
#define M0C_FLUSH gg_save_flush
#endif
#include "m0_cache.inc"

#define card_reset cache_reset

#if GG_STREAM || GG_CART_ON_CARD
/* The option menu's stack (sg_menu.h): the cache's blocks lent out, the
 * save written back first; lend returns their top, 8-aligned. */
uintptr_t gg_cache_lend(void) { return cache_lend(); }
void gg_cache_unlend(void) { cache_unlend(); }
#endif
#if GG_CACHE_DYN
uintptr_t gg_cache_top(void) { return cache_top(); }
unsigned gg_cache_ways(void) { return cache_ways; }
int gg_cache_grow(void) { return cache_grow(); }
int gg_cache_shrink(void) { return cache_shrink(); }
#endif

#if GG_CART_ON_CARD
void gg_save_tick(void) {
    if (save_pending && machine.frames - save_change >= 120) gg_save_flush();
}
#endif

#if GG_STREAM
#if defined(__ARM_ARCH_6M__) && defined(GG_POOL_SECTORS) && defined(GG_POOL_PACKED)
/* The Cortex-M0+: every read of a page left on the card comes here, about
 * 240 times a frame on Ristar, so it saves nothing on the stack.  A raw
 * pool sector's byte, else the cache's block read last, else a tail call
 * to cache_rom_fill (gg_stream_fill).  Same choices as the C below. */
uint8_t gg_stream_fill(uint32_t offset) __attribute__((alias("cache_rom_fill"))); /* its global name */
#if GG_CACHE_BLOCK == 16 /* M0C_SHIFT as the assembler's number */
#define GG_SHIFT_TXT "4"
#elif GG_CACHE_BLOCK == 32
#define GG_SHIFT_TXT "5"
#elif GG_CACHE_BLOCK == 64
#define GG_SHIFT_TXT "6"
#elif GG_CACHE_BLOCK == 128
#define GG_SHIFT_TXT "7"
#elif GG_CACHE_BLOCK == 256
#define GG_SHIFT_TXT "8"
#else
#define GG_SHIFT_TXT "9"
#endif
__attribute__((naked)) uint8_t gg_stream_read(uint32_t offset) {
    (void)offset;
    __asm__ volatile(
        "   .syntax unified\n"
        "   ldr   r2, =gg_pool_index\n"
        "   lsrs  r3, r0, #9\n"
#ifdef GG_POOL_WIDE
        "   lsls  r3, r3, #1\n"
        "   ldrh  r3, [r2, r3]\n"
        "   ldr   r1, =65535\n"
        "   cmp   r3, r1\n"
#else
        "   ldrb  r3, [r2, r3]\n"
        "   cmp   r3, #255\n"
#endif
        "   beq   2f\n"
        "   ldr   r2, =gg_pool_records\n"
        "   lsls  r3, r3, #2\n"
        "   ldr   r3, [r2, r3]\n"
        "   lsrs  r2, r3, #30\n"
        "   bne   2f\n"                   /* packed: decoded into the cache */
        "   ldr   r2, =gg_pool\n"
        "   adds  r3, r2\n"
        "   lsls  r0, r0, #23\n"
        "   lsrs  r0, r0, #23\n"
        "   ldrb  r0, [r3, r0]\n"
        "   bx    lr\n"
        "2: ldr   r3, =card_rom_last\n"
        "   ldrb  r3, [r3]\n"
        "   lsls  r2, r3, #2\n"
        "   ldr   r1, =card_tag\n"
        "   ldr   r1, [r1, r2]\n"
        "   lsrs  r2, r0, #" GG_SHIFT_TXT "\n"
        "   cmp   r1, r2\n"
        "   bne   3f\n"
        "   lsls  r3, r3, #" GG_SHIFT_TXT "\n"
        "   ldr   r2, =card_buf\n"
        "   adds  r3, r2\n"
        "   lsls  r0, r0, #(32 - " GG_SHIFT_TXT ")\n"
        "   lsrs  r0, r0, #(32 - " GG_SHIFT_TXT ")\n"
        "   ldrb  r0, [r3, r0]\n"
        "   bx    lr\n"
        "3: ldr   r3, =gg_stream_fill\n"
        "   bx    r3\n"
        "   .ltorg\n"
        "   .syntax divided\n");
}
#else
uint8_t gg_stream_read(uint32_t offset) {
#ifdef GG_POOL_SECTORS
    const unsigned pool = gg_pool_index[offset >> 9];
    if (pool != GG_POOL_NONE) {
#ifdef GG_POOL_PACKED
        const uint32_t record = gg_pool_records[pool];
        if (!(record >> 30)) return gg_pool[(record & 0x3fffffffu) + (offset & 511)];
#else
        return gg_pool[pool << 9 | (offset & 511)];
#endif
    }
#endif
    return cache_rom_byte(offset);
}
#endif
#endif
#if GG_CART_ON_CARD
uint8_t gg_cart_read(uint32_t offset) { return cache_ram_load(offset); }
void gg_cart_store(uint32_t offset, uint8_t v) { cache_ram_store(offset, v); }
#endif
#endif

/* Sega mapper: FFFC RAM control, FFFD-FFFF pages of slots 0-2 (the RAM
 * byte is written as well, by gg_write). */
void gg_mapper_write(uint16_t a, uint8_t v) {
    gg_t *m = &machine;
    if (GG_KOREAN150) {
        if (a == 0xfffc) m->ram_ctl = v & 0xf0u;
        else if (a == 0xfff3) m->page2 = v & 0x3fu;
        else return;
        korean150_remap();
        m->remapped = 1;
        m->next_evt = m->cyc;
        return;
    }
    if (GG_CODEMASTERS) { map_slot(a >> 14, v); return; } /* 0000, 4000, 8000 */
    if (a == 0xfffc) {
#if GG_CART_ON_CARD
        if ((m->ram_ctl & 8) && !(v & 8)) gg_save_flush(); /* games disable the RAM after saving */
#endif
        m->ram_ctl = v;
        if (v & 8) m->cart_ram_used = 1;
        m->page[2] = (v & 8) ? 0xff : m->page2;
        m->remapped = 1;
        m->next_evt = m->cyc;
    } else map_slot(a - 0xfffd, v);
}

void gg_write_cart(uint16_t a, uint8_t v) {
#ifdef GG_ROM_TRACE /* a write only when the byte changes: the firmware's cache_ram_store */
    gg_cart_trace(gg_cart_offset(a, machine.ram_ctl), machine.cart_ram && machine.cart_ram[gg_cart_offset(a, machine.ram_ctl)] != v);
#endif
#if GG_CART_ON_CARD
    gg_cart_store(gg_cart_offset(a, machine.ram_ctl), v);
#else
    if (machine.cart_ram) machine.cart_ram[gg_cart_offset(a, machine.ram_ctl)] = v;
#endif
}

/* ---- ports ---------------------------------------------------------------- */

uint8_t gg_in(uint8_t port) {
    gg_t *m = &machine;
    static int in_trace = -1;
    if (in_trace < 0) { in_trace = getenv("GG_IN_TRACE") ? 1 : 0; gg_watch_addr_init(); }
    if (in_trace && ((port & 0xc0) == 0xc0 || port == 0xbf || (port & 0xfe) == 0x7e)) {
        /* pad / vcounter / hcounter / VDP status reads; vcounter/hcounter print their value */
        const uint8_t pv = (port & 0xfe) == 0x7e ? gg_in(port & 0xc1) : (port == 0xbf ? m->vdp_status : 0);
        fprintf(stderr, "gg_in %02x -> %02x (buttons=%02x ctl=%02x st=%02x) pc=%04x cyc=%u fr=%u vc=%02x\n",
                port, pv, m->buttons, m->io_ctl, m->vdp_status, m->cpu.pc, m->cyc, m->frames, gg_vcounter());
    }
    if (port <= 6 && !GG_IS_SMS) { /* Game Gear registers: START, region, link port, stereo */
        if (!port) return (uint8_t)((m->buttons & GG_START ? 0 : 0x80) | 0x40); /* export, NTSC */
        if (port == 4) return 0xff;              /* serial receive: nothing */
        if (port == 5) return m->io[5] & 0xf8;   /* serial: not busy, nothing received, no error */
        return m->io[port];
    }
    switch (port & 0xc1) {
    case 0x40: return gg_vcounter();
    case 0x41: { /* H counter: pixel position / 2 within the line */
        const uint32_t t = m->cyc - m->frame_start, line = ((t >> 2) * 18397u) >> 20; /* t / 228, exact below 77000 */
        return (uint8_t)(((t - line * GG_LINE_CYCLES) * 768u) >> 10); /* * 171 / 228 */
    }
    case 0x80: return gg_vdp_data_read();
    case 0x81: return gg_vdp_status_read();
    case 0xc0:
        if (GG_IS_SMS) { /* port DC: the pad is behind the TH mux — TH low shows only
                          * up/down on D0-D1 and left/right on D4-D5, never the triggers */
            const unsigned c = m->io_ctl, tha = (c & 2) ? 1 : (c >> 5) & 1;
            const unsigned pad = m->buttons & 63;
            if (tha) return (uint8_t)~pad;
            return (uint8_t)~((pad & 3) | ((pad >> 2) & 0x30) | 0x0c);
        }
        return (uint8_t)~(m->buttons & 63);
    case 0xc1:
        if (GG_IS_SMS) { /* RESET up, TH A/B: outputs read back their level */
            const unsigned c = m->io_ctl, tha = (c & 2) ? 1 : (c >> 5) & 1, thb = (c & 8) ? 1 : (c >> 7) & 1;
            return (uint8_t)(0x3f | tha << 6 | thb << 7);
        }
        return 0xff;
    default: return 0xff;
    }
}

void gg_out(uint8_t port, uint8_t v) {
    if (GG_IS_SMS) {
        if ((port & 0xc1) == 0x01) { machine.io_ctl = v; return; } /* I/O control */
        if (port < 0x40) return;                                      /* memory control */
    } else if (port <= 6) { /* link port (written values read back), stereo: no sound effect here */
        if (port) machine.io[port] = v;
        return;
    }
    switch (port & 0xc1) {
    case 0x40: case 0x41: gg_psg_write(v); break;
    case 0x80: gg_vdp_data_write(v); break;
    case 0x81: gg_vdp_control_write(v); break;
    default: break; /* memory and I/O control */
    }
}

/* ---- machine -------------------------------------------------------------- */

void gg_irq_update(void) {
    gg_t *m = &machine;
    m->cpu.irq = ((m->vdp_status & 128) && (m->vdp[1] & 32)) || (m->line_irq && (m->vdp[0] & 16));
    if (m->cpu.irq && m->cpu.iff1) m->next_evt = m->cyc;
}

/* Next device event; an acceptable interrupt makes it now. */
static void schedule(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    m->next_evt = m->frame_start + (m->vblank_done ? GG_FRAME_CYCLES : GG_VBLANK_CYCLES);
    const uint32_t line = gg_vdp_next_event();
    if (line && (int32_t)(line - m->next_evt) < 0) m->next_evt = line;
    /* Consoles: lines are drawn every GG_RENDER_STEP lines, so that rows
     * go out to the display while the game runs, not in one burst. */
#ifdef GG_RENDER_STEP
    if (m->row && m->render_line < GG_WIN_TOP + GG_WIN_LINES) {
        const uint32_t step = m->frame_start + (uint32_t)((m->render_line / GG_RENDER_STEP + 1) * GG_RENDER_STEP) * GG_LINE_CYCLES;
        if ((int32_t)(step - m->next_evt) < 0) m->next_evt = step;
    }
#endif
    if (c->nmi || (c->irq && c->iff1 && !c->ei_delay)) m->next_evt = m->cyc;
}

void gg_init(const uint8_t *rom, size_t size, uint8_t *cart_ram) {
#ifdef GG_SMS
    gg_init_system(rom, size, cart_ram, GG_SMS);
#else
    gg_init_system(rom, size, cart_ram, 0);
#endif
}

void gg_init_system(const uint8_t *rom, size_t size, uint8_t *cart_ram, int sms) {
    gg_t *m = &machine;
    sg_fn (*lookup)(uint16_t) = m->lookup;
    uint32_t *coverage = m->coverage;
    uint8_t *entries = m->entries;
    uint32_t coverage_size = m->coverage_size;
    memset(m, 0, sizeof *m);
    m->lookup = lookup;
    m->coverage = coverage; m->entries = entries; m->coverage_size = coverage_size;
    m->sms = (uint8_t)!!sms;
    m->rom = rom; /* rom_pages * 16 KiB readable bytes (callers pad) */
    m->rom_size = (uint32_t)size;
    m->rom_pages = (uint16_t)((size + 0x3fff) / 0x4000);
    /* Codemasters boards: checksum and its complement at 7FE6/7FE8.  Only
     * the generic host detects it: generated projects fix GG_MAPPER, and a
     * streamed one may hold page 0 alone in flash, so 7FE6 may lie past the
     * array -- past the end of flash when it is full (device, 2026-10-17:
     * HardFault at boot). */
#ifndef GG_MAPPER
    m->mapper = (uint8_t)m0_sega_mapper(rom, size);
#else
    m->mapper = GG_MAPPER;
#endif
    m->cart_ram = cart_ram;
#if GG_STREAM || GG_CART_ON_CARD
    card_reset();
#endif
    m->cpu.user = m;
    m->cpu.read = bus_read; m->cpu.write = bus_write;
    m->cpu.in = bus_in; m->cpu.out = bus_out;
    gg_reset();
}

void gg_reset(void) {
    __asm__ volatile("" : : "r"(&gg_input_probe));
    gg_t *m = &machine;
    z80_t keep = m->cpu;
    z80_reset(&m->cpu);
    m->cpu.user = keep.user; m->cpu.read = keep.read; m->cpu.write = keep.write;
    m->cpu.in = keep.in; m->cpu.out = keep.out;
    m->cpu.sp = 0xdff0;
    memset(m->ram, 0, sizeof m->ram);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->cram, 0, sizeof m->cram);
    if (m->cart_ram) memset(m->cart_ram, 0, GG_CART_RAM_SIZE);
    m->ram_ctl = 0;
    if (GG_KOREAN150) { m->page2 = 0; korean150_remap(); }
    else for (unsigned s = 0; s < 3; s++) map_slot(s, (uint8_t)s);
    /* VDP registers as left by the boot ROM */
    static const uint8_t regs[11] = { 0x36, 0x80, 0xff, 0xff, 0xff, 0xff, 0xfb, 0x00, 0x00, 0x00, 0xff };
    memset(m->vdp, 0, sizeof m->vdp);
    memcpy(m->vdp, regs, sizeof regs);
    m->sat_base = (uint16_t)((m->vdp[5] & 0x7e) << 7);
    m->vdp_address = 0;
    m->vdp_code = m->vdp_pending = m->vdp_latch = m->vdp_buffer = m->cram_latch = 0;
    m->vdp_status = 0;
    m->line_irq = 0;
    m->sprites_dirty = 1;
    for (unsigned i = 0; i < 32; i++) m->pal[i] = 0;
    m->cyc = m->frame_start = 0;
    m->render_line = 0;
    /* line N renders at the start of line N, not of N+1: the sprite
     * collision/overflow flags must be visible to a mid-line $BF poll
     * (Castle reads $BF on line 159 and acts on the flags) */
    m->render_at = 0;
    m->vblank_done = 0;
    m->frames = 0;
    m->lc_count = m->vdp[10];
    gg_vdp_frame_start();
    m->buttons = 0;
    {   /* the link port and stereo registers at power on */
        static const uint8_t io[7] = { 0xc0, 0x7f, 0xff, 0x00, 0xff, 0x00, 0xff };
        memcpy(m->io, io, sizeof io);
        m->io_ctl = 0xff;
    }
    memset(m->psg_tone, 0, sizeof m->psg_tone);
    memset(m->psg_volume, 15, sizeof m->psg_volume);
    m->psg_noise = m->psg_latch = 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = 0;
    m->audio_head = m->audio_tail = 0;
    m->audio_lfsr = 0x8000;
    memset(m->audio_count, 0, sizeof m->audio_count);
    memset(m->audio_output, 0, sizeof m->audio_output);
    m->interpreted = m->compiled = 0;
    schedule();
}

/* Master System: PAUSE pressed raises the NMI (immediately: byte-identical
 * replay to the reference core, which asserts it at the frame end — the
 * games' pause handlers are frame-aligned, Alex Kidd/Sonic replayed identically) */
void gg_buttons(uint8_t pressed) {
    gg_t *m = &machine;
    static int trace = -1;
    if (trace < 0) trace = getenv("GG_BTN_TRACE") ? 1 : 0;
    if (trace && pressed != m->buttons) {
        static const char *const names[7] = { "U", "D", "L", "R", "B1", "B2", "START" };
        char s[64] = "";
        for (int b = 0; b < 7; b++)
            if (pressed & (1u << b)) { strncat(s, " ", sizeof s - strlen(s) - 1); strncat(s, names[b], sizeof s - strlen(s) - 1); }
        fprintf(stderr, "gg_buttons: %02x%s\n", pressed, s);
    }
    if (GG_IS_SMS && (pressed & GG_PAUSE) && !(m->buttons & GG_PAUSE)) {
        if (trace) fprintf(stderr, "gg_buttons: PAUSE NMI raised\n");
        m->cpu.nmi = 1; m->next_evt = m->cyc;
    }
    m->buttons = pressed;
}

/* ---- events and interrupts ------------------------------------------------ */

void sg_sync(void) {
    gg_t *m = &machine;
#ifdef GG_RENDER_STEP
    gg_vdp_catchup();
#endif
    for (;;) {
        gg_line_counter_catchup();
        if (!m->vblank_done) {
            if ((int32_t)(m->cyc - (m->frame_start + GG_VBLANK_CYCLES)) < 0) break;
            gg_vdp_catchup(); /* the 192 lines have ended */
            m->vblank_done = 1;
            m->vdp_status |= 128;
        } else {
            if ((int32_t)(m->cyc - (m->frame_start + GG_FRAME_CYCLES)) < 0) break;
            gg_audio_catchup();
            m->frame_start += GG_FRAME_CYCLES;
            m->vblank_done = 0;
            m->render_line = 0;
            m->render_at = m->frame_start; /* line 0 at its own start, see init */
            gg_vdp_frame_start();
            m->frames++;
            if (m->frame) m->frame(m->platform);
        }
    }
    gg_irq_update();
    schedule();
}

/* Take an NMI or an acceptable IRQ: push pc, jump to the vector. */
static int nmi_trace = -1;
static int irq_enter(void) {
    gg_t *m = &machine;
    if (nmi_trace < 0) nmi_trace = getenv("GG_BTN_TRACE") ? 1 : 0;
    z80_t *c = &m->cpu;
    unsigned cycles;
    if (c->nmi) {
        if (nmi_trace) fprintf(stderr, "nmi taken at pc=%04x (frame %u)\n", c->pc, m->frames);
        c->nmi = 0;
        c->iff2 = c->iff1; c->iff1 = 0;
        z80_push(c, c->pc);
        if (m->entries && m->irq_depth < 8) m->irq_sp[m->irq_depth++] = c->sp;
        c->pc = 0x66;
        cycles = 11;
    } else if (c->irq && c->iff1 && !c->ei_delay) {
        if (nmi_trace) fprintf(stderr, "irq taken at pc=%04x frame=%u st=%02x line_irq=%d (frame_ie=%d line_ie=%d)\n",
                               c->pc, m->frames, m->vdp_status, m->line_irq,
                               m->vdp[1] & 32, m->vdp[0] & 16);
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

/* Nonzero when pc moved, or when a page changed since the last event (a
 * block copy over the mapper registers, an interrupt handler that does not
 * restore them): generated code then returns to the dispatcher, which
 * looks pc up again under the new mapping. */
int sg_event(uint16_t resume_pc) {
    gg_t *m = &machine;
    for (;;) {
        sg_sync();
        m->cpu.pc = resume_pc;
        if (m->stop) return 1;
        if (m->remapped) { m->remapped = 0; return 1; } /* back to the dispatcher */
        if (!irq_enter()) return 0;
        sg_exec();
        if (m->cpu.pc != resume_pc || m->stop) return 1;
    }
}

void sg_halt(void) {
    gg_t *m = &machine;
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

/* SG_EXEC_HEADROOM (gg.h): room above the limit for the frames between
 * here and the first ENTER().  The limit already holds the runtime's
 * reserve; 768 there refused every native entry once the stack was under
 * ~1.4 KiB (Master System, 2026-10-21). */
#if GG_CACHE_DYN
uint32_t sg_exec_refused;
uintptr_t sg_pool_sp;
#endif

static void dispatch_one(int compiled) {
    gg_t *m = &machine;
    if (m->cpu.halted) {
        sg_halt();
        if (!m->stop) (void)sg_event(m->cpu.pc);
        return;
    }
    if (m->cpu.ei_delay) {
        sg_step(m->cpu.pc);
        return;
    }
    sg_fn fn = compiled && m->lookup ? m->lookup(m->cpu.pc) : 0;
    if (fn) { m->compiled++; fn(); }
    else sg_interp_block();
}

void sg_exec(void) {
    char probe;
#if GG_CACHE_DYN
    if (sg_pool_sp && (uintptr_t)&probe > sg_pool_sp) gg_platform_pool();
#endif
    if ((uintptr_t)&probe < sg_stack_limit + SG_EXEC_HEADROOM) {
#if GG_CACHE_DYN
        sg_exec_refused++; /* the frontend's measure of the stack's shortage */
#endif
        return;
    }
    uint16_t sp0 = machine.cpu.sp;
    do {
        if (machine.stop) return;
        dispatch_one(1);
    } while ((int16_t)(machine.cpu.sp - sp0) <= 0);
}

void gg_run(void) {
    char probe;
    if (!sg_stack_limit) sg_stack_limit = (uintptr_t)&probe - 4096u;
    int room = (uintptr_t)&probe >= sg_stack_limit + SG_EXEC_HEADROOM;
    while (!machine.stop) dispatch_one(room);
}

/* Profiling: targets of RET, RETI/RETN and JP (HL/IX/IY), interrupt
 * returns left out (see sg1000.c). */
static void note_dynamic(const z80_decoded_t *d, uint16_t sp, uint16_t pc) {
    gg_t *m = &machine;
    const unsigned op = d->opcode;
    const int ret = d->group == 0 ? op == 0xc9 || (op & 0xc7) == 0xc0 : d->group == 2 && (op & 0xc7) == 0x45;
    if (!ret && !(d->group == 0 && op == 0xe9)) return;
    while (m->irq_depth && sp > m->irq_sp[m->irq_depth - 1]) m->irq_depth--;
    if (ret && m->irq_depth && sp == m->irq_sp[m->irq_depth - 1]) { m->irq_depth--; return; }
    const uint32_t loc = gg_location(pc);
    if (loc < m->coverage_size) m->entries[loc] = 1;
}

/* Host profiling: one more execution of the instruction at loc; the
 * explorer (gg_host --explore) counts the locations never seen before. */
#ifdef GG_ROM_TRACE
uint32_t gg_new_locations;
static inline void gg_cover(uint32_t loc) {
    if (!machine.coverage[loc]) gg_new_locations++;
    machine.coverage[loc]++;
}
#else
static inline void gg_cover(uint32_t loc) { machine.coverage[loc]++; }
#endif

/* The instructions the fast path declines: decoded and executed generically
 * (the same split as sg1000.c, whose block loop this one follows). */
static __attribute__((noinline)) int interp_generic(uint16_t pc) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    z80_decoded_t d;
    if (!z80_decode(c, &d)) { c->fault = 1; m->stop = 1; return 0; }
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

static inline __attribute__((always_inline)) int interp_step(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    if (__builtin_expect((int32_t)(m->cyc - m->next_evt) >= 0, 0) && sg_event(c->pc)) return 0;
    const uint16_t pc = c->pc;
    if (m->coverage) {
        const uint32_t loc = gg_location(pc);
        if (loc < m->coverage_size) gg_cover(loc);
#ifdef GG_ROM_TRACE
        uint32_t offset;
        if (gg_rom_offset(pc, &offset)) gg_rom_trace_insn(offset);
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

/* The block loop is the one place the step is inlined (a second copy would double the fast path's flash). */
static __attribute__((noinline)) unsigned interp_run(unsigned limit) {
    unsigned n = 0;
    while (n < limit) { n++; if (!interp_step()) break; }
    return n;
}
static void interp_one(void) { (void)interp_run(1); }

/* OUTI / OUTD in RAM, where games unroll their VRAM uploads: z80_execute's
 * effects without the generic decode.  1 done, 0 another instruction, -1
 * stopped by an event. */
static int ram_outx(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    const uint16_t pc = c->pc;
    if (pc < 0xc000 || pc == 0xffff || m->ram[pc & 8191] != 0xed) return 0;
    const uint8_t op = m->ram[(pc + 1) & 8191];
    if ((op & 0xf7) != 0xa3) return 0;
    if ((int32_t)(m->cyc - m->next_evt) >= 0 && sg_event(pc)) return -1;
    if (c->pc != pc) return 0; /* the event moved pc */
    if (m->coverage) { const uint32_t loc = gg_location(pc); if (loc < m->coverage_size) gg_cover(loc); }
    c->pc = (uint16_t)(pc + 2);
    c->r = (uint8_t)((c->r & 128) | ((c->r + 2) & 127));
    const uint8_t v = gg_native_read(c, c->hl);
    c->b--;
    c->hl = (uint16_t)(c->hl + (op & 8 ? -1 : 1));
    gg_out(c->c, v);
    const unsigned s = v + c->l;
    c->f = (uint8_t)(z80_sz(c->b) | (v & 128 ? Z80_N : 0) | (s > 255 ? Z80_H | Z80_C : 0) | z80_parity((uint8_t)((s & 7) ^ c->b)));
    m->cyc += 16;
    m->interpreted++;
    if (c->ei_delay && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
    return 1;
}

void sg_interp_block(void) {
    unsigned n = 0;
    if (machine.cpu.pc >= 0xc000)
        for (int r; n < 32 && (r = ram_outx()) != 0; n++) if (r < 0) return;
    if (n < 32) (void)interp_run(32 - n);
}

int sg_step(uint16_t after) {
    do interp_one();
    while (machine.cpu.ei_delay && !machine.stop && !machine.cpu.fault);
    return machine.cpu.pc == after && !machine.stop;
}

/* Save states: the machine as raw bytes, then the cartridge RAM when the
 * game has some.  The load re-sets what is not state — the bus callbacks,
 * the buffers and hooks this firmware set at init — and rebuilds the slots'
 * pointers from the restored pages (gg_remap).  Bytes are only comparable
 * within a build: the header's format tag refuses the others. */
uint32_t gg_state_bytes(void) {
    return (uint32_t)sizeof(gg_t) + (machine.cart_ram ? GG_CART_RAM_SIZE : 0u);
}

void gg_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load) {
    if (!load) {
        io(&machine, (uint32_t)sizeof(gg_t), ctx);
        if (machine.cart_ram) io(machine.cart_ram, GG_CART_RAM_SIZE, ctx);
        return;
    }
    const uint8_t *rom = machine.rom;
    uint8_t *cart = machine.cart_ram;
    uint16_t *row_out = machine.row_out, *row_tmp = machine.row_tmp;
    void *platform = machine.platform;
    int (*draw_line)(void *, unsigned y) = machine.draw_line;
    void (*row)(void *, unsigned y, uint16_t *pixels) = machine.row;
    void (*frame)(void *) = machine.frame;
    sg_fn (*lookup)(uint16_t pc) = machine.lookup;
    uint32_t *coverage = machine.coverage;
    uint8_t *entries = machine.entries;
    z80_read_fn read = machine.cpu.read, in = machine.cpu.in;
    z80_write_fn write = machine.cpu.write, out = machine.cpu.out;
    io(&machine, (uint32_t)sizeof(gg_t), ctx);
    if (machine.cart_ram) io(machine.cart_ram, GG_CART_RAM_SIZE, ctx);
    machine.rom = rom;
    machine.cart_ram = cart;
    machine.row_out = row_out;
    machine.row_tmp = row_tmp;
    machine.platform = platform;
    machine.draw_line = draw_line;
    machine.row = row;
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
    gg_remap();
}

uint32_t gg_state_frame(void) { return machine.frames; }
