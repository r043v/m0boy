#ifndef M0_SG1000_H
#define M0_SG1000_H
#ifndef M0_TL
#define M0_TL
#endif
/* SG-1000 runtime shared by recompiled code and the Z80 interpreter.
 *
 * Time is a T-state counter (machine.cyc) with two device events per frame:
 * VBlank at line 192 and the end of the frame at line 262.  Video lines are
 * rendered lazily, when the program touches the VDP or at VBlank, from the
 * VDP state of that moment: nothing is checked per line in generated code,
 * and a line still sees every register/VRAM write made before it ended.
 * The PSG is caught up the same way, before each write and at frame end.
 *
 * Execution: sg1000_run() looks pc up in the generated entry table (regions
 * of native code) and falls back to the interpreter for anything else.  Both
 * engines synchronise devices and take interrupts through sg_event, so a
 * precise build (sync before every instruction) reproduces the interpreter. */
#include "z80.h"
#include <stddef.h>
/* Generated projects configure the runtime (ROM size, cartridge RAM). */
#if defined(__has_include)
#if __has_include("sg_config.h")
#include "sg_config.h"
#endif
#endif
/* MSX (targets/msx): a Z80 + TMS9918A machine whose BIOS is replaced by a
 * simulated one (msx.c).  Memory is four 16 KiB pages, each in a primary
 * slot chosen through the PPI; the bus goes through a table of 8 KiB
 * windows.  SG_MSX_RAM bytes of RAM are present at the top of the address
 * space (16 KiB: C000-FFFF; 8 KiB on the consoles: E000-FFFF). */
#ifndef SG_MSX
#define SG_MSX 0
#endif
#if SG_MSX
#ifndef SG_MSX_RAM
#define SG_MSX_RAM 8192
#endif
#define SG_RAM_BYTES SG_MSX_RAM
#else
#define SG_RAM_BYTES 1024
#endif
/* Cartridge RAM leaves no room for the 4 KiB colour-pair table. */
#ifndef SG_LOW_RAM
#define SG_LOW_RAM 0
#endif
/* SG_MIX_SLOTS: the renderer's colour blends as a pool of that many rows
 * filled on demand (tms9918.c) instead of the 4.5 KiB table (0), which the
 * asm loops can then use with little RAM. */
#ifndef SG_MIX_SLOTS
#define SG_MIX_SLOTS 0
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define SG_WIDTH 256
#define SG_HEIGHT 192
#define SG_LINES 262
#define SG_LINE_CYCLES 228
#define SG_FRAME_CYCLES (SG_LINES * SG_LINE_CYCLES)
#define SG_VBLANK_CYCLES (SG_HEIGHT * SG_LINE_CYCLES)
/* Device line: 256 -> SG_OUT_WIDTH pixels, box filter (5 device pixels per
 * tile).  The PC keeps the TMS9918A's own 256 (sg_config.h); the consoles
 * resample to their panel's 160. */
#ifndef SG_OUT_WIDTH
#define SG_OUT_WIDTH 160
#endif
#define SG_CART_RAM_SIZE 8192

typedef void (*sg_fn)(void);

typedef struct {
    z80_t cpu;               /* registers; cpu.user points back here */
    uint32_t cyc;            /* T-states, wrapping */
    uint32_t next_evt;       /* sync when cyc reaches it (forced to cyc for IRQs) */
    /* What every interpreted instruction touches comes first: a Thumb-1 load reaches 124 bytes from the base,
     * further fields cost an extra literal load each. */
    uint32_t interpreted, compiled; /* statistics: instructions / region entries */
    uint32_t *coverage;           /* host: per-pc hit counters (65536), or NULL */
    uint8_t *entries;             /* host: targets of RET / JP (HL) seen (65536), or NULL */
#if SG_MSX
    const uint8_t *rd_win[8];     /* per 8 KiB window, where reads go (NULL: msx_read_slow); rebuilt from ppi_a by msx_remap */
#endif
    uint32_t frame_start;    /* cycle of line 0 of the current frame */
    uint32_t render_at;      /* cycle at which line render_line ends */
    uint32_t frames;
    uint16_t render_line;
    uint8_t vblank_done, stop;
    const uint8_t *rom;
    uint32_t rom_size, rom_mask; /* mask: power-of-two size - 1, else 0 */
    uint8_t *cart_ram;       /* 8 KiB at 8000-BFFF (mirrored), or NULL */
    uint8_t ram[SG_RAM_BYTES], vram[16384], vdp[8];
    uint16_t vdp_address, sat_base;
    uint8_t vdp_latch, vdp_pending, vdp_buffer, vdp_status;
    uint8_t sprites_dirty;   /* sprite Y, terminator, size or table changed */
    uint8_t buttons[2], pause;
    /* SN76489 */
    uint16_t psg_tone[3];
    uint8_t psg_volume[4], psg_noise, psg_latch;
    uint32_t audio_rate, audio_phase, audio_tick_phase, audio_lfsr, audio_cyc;
    uint32_t audio_div, audio_tick_int, audio_tick_frac; /* PSG ticks per sample */
    int32_t audio_count[4];  /* PSG ticks to the next edge (<= 0: edge due) */
    uint16_t audio_head, audio_tail;
    uint8_t audio_output[4], audio_buffer[512];
    /* Output.  row: SG_OUT_WIDTH RGB565 pixels from palette (true RGB565,
     * the platform converts to its panel format); draw_line
     * selects the source lines that are displayed (others only update
     * sprite status).  ref_row: full 256-pixel colour indices, host only. */
    uint16_t palette[16];
    uint16_t *row_out;       /* where the next displayed line is rendered */
    void *platform;
    int (*draw_line)(void *, unsigned y); /* 0 none, 1 line y, 2 lines y - 1 and y */
    void (*row)(void *, unsigned y, uint16_t *pixels); /* pixels == row_out; may move row_out */
    void (*ref_row)(void *, unsigned y, const uint8_t *pixels);
    void (*frame)(void *);
    /* Execution */
    sg_fn (*lookup)(uint16_t pc); /* generated entry table, or NULL */
    uint16_t irq_sp[8];           /* host: stack of interrupt return slots, for entries */
    uint8_t irq_depth;
    uint8_t cart_ram_used;        /* a write reached 8000-BFFF */
#if SG_MSX
    /* Memory: per 8 KiB window, where writes go (NULL: nothing, a bank register, or the SCC); rd_win is above. */
    uint8_t *wr_win[8];
    uint8_t ppi_a, ppi_c, ay_latch, ay_reg[16];
    uint16_t msx_base, msx_init;  /* address of the cartridge's first byte, its INIT routine */
    /* Cartridge banks: the 8 KiB unit each window (2..5: 4000, 6000, 8000, A000) shows,
     * which windows show the cartridge, the mapper (M0_MSX_*, 0: unbanked) and the units */
    uint16_t page[6], rom_units;
    uint8_t cart_vis, cart_mask, msx_mapper;
    uint32_t coverage_size;       /* host profiling of banked cartridges: locations (page << 16 | pc) */
    /* AY-3-8910 sample generation (msx_ay.c) */
    uint16_t ay_count[3], ay_noise_count, ay_env_count;
    uint8_t ay_out[3], ay_env_pos, ay_env_attack, ay_env_hold, ay_env_final;
    /* SCC (K051649) of the Konami SCC mapper: 4 waveforms of 32 signed bytes (the fifth voice shares the
     * fourth's), per voice a 12-bit period, a volume, the enable mask; scc_on while the 9000 bank holds 3F */
    int8_t scc_wave[4][32];
    uint16_t scc_period[5];
    uint32_t scc_inc[5], scc_phase[5], scc_k;
    uint8_t scc_vol[5], scc_enable, scc_on;
    uint8_t click_used, click;  /* the key-click bit of the PPI (port C bit 7): a 1-bit speaker some games play samples on */
    uint32_t ay_lfsr;
    uint32_t msx_calls;           /* BIOS calls made (statistics) */
#endif
} sg1000_t;

extern M0_TL sg1000_t machine;
extern const uint32_t sg1000_palette[16];
/* Below this stack pointer regions and nested dispatch loops unwind. */
extern M0_TL uintptr_t sg_stack_limit;

void sg1000_init(const uint8_t *rom, size_t size, uint8_t *cart_ram);
void sg1000_reset(void);
void sg1000_buttons(unsigned player, uint8_t pressed);
void sg1000_pause(int pressed);
/* Run until machine.stop is set (by a frame callback); resumable. */
void sg1000_run(void);

/* Bus, at machine.cyc (callers flush pending cycles first). */
#if SG_MSX
uint8_t msx_read_slow(uint16_t a);
static inline uint8_t sg1000_read(uint16_t a) {
    const uint8_t *w = machine.rd_win[a >> 13];
    return w ? m0_load_u8(w + (a & 0x1fff)) : msx_read_slow(a);
}
void msx_mapper_write(uint16_t a, uint8_t v);
void msx_scc_write(unsigned reg, uint8_t value); /* 9800-98FF while the SCC is on */
static inline void sg1000_write(uint16_t a, uint8_t v) {
    uint8_t *w = machine.wr_win[a >> 13];
    if (w) w[a & 0x1fff] = v;
    else if (machine.msx_mapper) msx_mapper_write(a, v);
}
static inline void sg1000_write_cart(uint16_t a, uint8_t v) { (void)a; (void)v; }
void msx_set_mapper(int mapper);     /* before sg1000_init: force the mapper (-1: detect) */
int msx_mapper_forced(void);
void msx_remap(void);
uint8_t msx_vdp_in(uint8_t port);        /* the TMS9918A data and status ports */
void msx_vdp_out(uint8_t port, uint8_t value);
void msx_vdp_data(uint8_t value);
void msx_boot(void);                 /* BIOS-simulated start: state the BIOS leaves, jump to INIT */
void msx_state_loaded(void);
extern const uint8_t msx_bios[];
#define MSX_BIOS_BYTES 512
#else
static inline uint8_t sg1000_read(uint16_t a) {
    if (a >= 0xc000) return machine.ram[a & 1023];
    if (a >= 0x8000 && machine.cart_ram) return machine.cart_ram[a & (SG_CART_RAM_SIZE - 1)];
    if (machine.rom_mask) return m0_load_u8(machine.rom + (a & machine.rom_mask));
    return a < machine.rom_size ? m0_load_u8(machine.rom + a) : 0xff;
}
void sg1000_write_cart(uint16_t a, uint8_t v);
static inline void sg1000_write(uint16_t a, uint8_t v) {
    if (a >= 0xc000) machine.ram[a & 1023] = v;
    else if (a >= 0x8000) sg1000_write_cart(a, v);
}
#endif
uint8_t sg1000_in(uint8_t port);
void sg1000_out(uint8_t port, uint8_t v);

/* Devices and events */
void sg_sync(void);                  /* process due device events */
int sg_event(uint16_t resume_pc);    /* sync + interrupt; nonzero: pc moved */
void sg_halt(void);                  /* HALT: skip time to the next interrupt */
void sg_irq_update(void);
void sg_exec(void);                  /* dispatch until the guest returns */
void sg_interp_block(void);
int sg_step(uint16_t after);         /* one interpreted instruction; 1: continue at after */

/* Video */
void sg1000_vdp_catchup(void);       /* render the lines that ended before cyc */
void sg1000_palette_changed(void);                  /* after writing machine.palette */
void sg1000_sprite_y(unsigned i, unsigned old, unsigned value); /* bus: SAT Y write */
void sg1000_render_line(unsigned y, uint8_t *pixels); /* reference: 256 indices or status only */
void sg1000_render_out(unsigned y, uint16_t *out);    /* device: 160 RGB565, box-filtered */
void sg1000_filter_line(const uint8_t *pixels, uint16_t *out); /* 256 indices -> 160 RGB565 */
void sg1000_render_pair(unsigned ya, uint16_t *out);  /* device row from lines ya and ya + 1 */
unsigned sg1000_render_selftest(void);              /* SG_SELFTEST: Thumb-1 vs C */

/* Save states (meta_state.cpp): the machine as raw bytes, then the
 * cartridge RAM; a load keeps this run's pointers and callbacks. */
uint32_t sg_state_bytes(void);
void sg_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
uint32_t sg_state_frame(void);

/* Audio: unsigned 8-bit samples at rate, following emulated time */
void sg1000_audio_rate(unsigned rate);
void sg1000_audio_catchup(void);
void sg1000_audio_render(uint8_t *buffer, unsigned n);
static inline unsigned sg1000_audio_available(void) {
    return (unsigned)(machine.audio_head - machine.audio_tail) & 511u;
}

#ifdef __cplusplus
}
#endif
#endif
