#ifndef M0_NGP_H
#define M0_NGP_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Neo Geo Pocket / Neo Geo Pocket Color machine.  The CPU is a TLCS-900H
 * compatible core (24-bit address space, four register banks of
 * XWA/XBC/XDE/XHL plus XIX/XIY/XIZ/XSP), the video the K2GE (two scroll
 * planes + 64 sprites, 160x152, 12-bit colour or 4-tone grey), the sound a
 * T6W28 PSG driven by a Z80 and two direct DACs.  The timing model is the
 * event one the NGP emulation lineage settled on: 515 CPU ticks a line, 198
 * lines a frame, the V-INT moment when line 152 is entered.  Cycle counts
 * per instruction are the documented approximations of that lineage; the
 * interpreter and the recompiled code share them, which is what the
 * run_hash contract needs.
 *
 * Memory map (24-bit):
 *   0x000000-0x0000FF  CPU internal I/O (timers, interrupt controller,
 *                      serial, RTC, sound ports, Z80 control)
 *   0x004000-0x006FFF  work RAM, 12 KiB (0x6C00-0x6FFF the BIOS workspace,
 *                      0x6FB8 the 18-slot user interrupt vector table)
 *   0x007000-0x007FFF  the sound Z80's 4 KiB shared RAM
 *   0x008000-0x00BFFF  K2GE: registers, palette RAM, sprite RAM, two scroll
 *                      maps, character RAM
 *   0x200000-0x3FFFFF  cartridge window 1 (the first 2 MiB)
 *   0x800000-0x9FFFFF  cartridge window 2 (a 4 MiB cart's upper half)
 *   0xFF0000-0xFFFFFF  BIOS ROM (synthesised: the machine boots the
 *                      cartridge directly, like every NGP emulator). */

enum { NGP_SCREEN_W = 160, NGP_SCREEN_H = 152, NGP_LINES = 198, NGP_LINE_TICKS = 515 };
enum { NGP_RAM_BYTES = 0x4000, NGP_BIOS_BYTES = 0x10000 };
enum { NGP_IRQ_TABLE = 0x6FB8, NGP_IRQ_SLOTS = 18, NGP_IRQ_STUB = 0xFF23DF };
/* interrupt indexes in the RAM vector table; the sources the machine raises */
enum {
    NGP_INT_SWI3 = 0, NGP_INT_SWI4 = 1, NGP_INT_SWI5 = 2, NGP_INT_SWI6 = 3,
    NGP_INT_RTC = 4,  NGP_INT_VBLANK = 5, NGP_INT_Z80 = 6,
    NGP_INT_TIMER0 = 7, NGP_INT_TIMER1 = 8, NGP_INT_TIMER2 = 9, NGP_INT_TIMER3 = 10,
    NGP_INT_TX0 = 11, NGP_INT_RX0 = 12,
    NGP_INT_DMA0 = 14, NGP_INT_DMA1 = 15, NGP_INT_DMA2 = 16, NGP_INT_DMA3 = 17,
};
/* buttons as 0x6F82 holds them: up, down, left, right, A, B, option, power */
enum { NGP_BTN_UP = 1, NGP_BTN_DOWN = 2, NGP_BTN_LEFT = 4, NGP_BTN_RIGHT = 8,
       NGP_BTN_A = 16, NGP_BTN_B = 32, NGP_BTN_OPTION = 64, NGP_BTN_POWER = 128 };
enum { NGP_ROM_MAX = 4 * 1024 * 1024, NGP_FLASH_BLOCK = 8192,
       NGP_FLASH_BLOCKS = NGP_ROM_MAX / NGP_FLASH_BLOCK };

/* the TLCS-900H register file: sr holds the flags (S 0x80, Z 0x40, H 0x10,
 * V 0x04, N 0x02, C 0x01), the interrupt mask level in bits 12-14 and the
 * register bank pointer in bits 8-9 */
typedef struct {
    uint32_t pc;
    uint16_t sr;
    uint8_t f_dash;                 /* the shadow flags of EX F,F' */
    uint32_t bank[4][4];            /* XWA XBC XDE XHL per bank */
    uint32_t gpr[4];                /* XIX XIY XIZ XSP */
    uint8_t halted;
    uint8_t fault;                  /* an undefined opcode ran */
} ngp_cpu_t;

/* K2GE registers as the guest writes them, plus the RAMs */
typedef struct {
    uint8_t control_int;            /* 0x8000: V-INT 0x80, H-INT 0x40 */
    uint8_t wba_h, wba_v;           /* 0x8002/3 window position */
    uint8_t wsi_h, wsi_v;           /* 0x8004/5 window size */
    uint8_t screen_period;          /* 0x8006 */
    uint8_t control_2d;             /* 0x8012: outside-window colour 0-2, negative 0x80 */
    uint8_t po_h, po_v;             /* 0x8020/1 sprite plane offset */
    uint8_t p_f;                    /* 0x8030: plane swap 0x80 */
    uint8_t s1so_h, s1so_v;         /* 0x8032/3 scroll 1 offset */
    uint8_t s2so_h, s2so_v;         /* 0x8034/5 scroll 2 offset */
    uint8_t bg_col;                 /* 0x8118: background colour / enable */
    uint8_t k2ge_mode;              /* 0x87e2: 0x80 = monochrome */
    uint8_t c_ovr, blink;           /* 0x8010 status bits */
    uint8_t scrp1plt[6], scrp2plt[6], spplt[6]; /* the mono palettes */
    uint16_t raster_line;
    uint8_t scroll_vram[0x1000];    /* 0x9000: two 32x32 2-byte tile maps */
    uint8_t character_ram[0x2000];  /* 0xA000: 512 tiles of 2bpp 8x8 */
    uint8_t sprite_vram[0x100];     /* 0x8800: 64 sprites of 4 bytes */
    uint8_t sprite_pal[0x40];       /* 0x8C00: the sprite palette index */
    uint8_t palette_ram[0x200];     /* 0x8200: 256 16-bit colour entries */
} ngp_gfx_t;

typedef struct ngp_t ngp_t;
typedef uint8_t (*ngp_read_fn)(ngp_t *m, uint32_t addr);

struct ngp_t {
    ngp_cpu_t cpu;
    ngp_gfx_t gfx;
    /* timers (the TLCS-900H's own 8-bit timers) */
    uint8_t trun, t01mod, t23mod, trdc, tffcr;
    uint8_t timer_tick[4];          /* the 8-bit counters, as the guest reads them */
    uint8_t timer_threshold[4];
    uint32_t timer_clock[4];        /* sub-counter, CPU ticks */
    uint32_t timer_hint;            /* ticks accumulated towards the next line */
    uint8_t timer0_tick, timer2_tick; /* chain flags */
    /* interrupt controller */
    uint8_t int_prio[0xB];          /* 0x70-0x7A, two levels a byte */
    uint8_t int_pending[24];
    uint8_t hdma_vector[4];         /* 0x7C-0x7F: the micro-DMA start vectors */
    uint8_t h_int;
    uint8_t v_int;                  /* the frame boundary: line 152 entered */
    /* micro-DMA: four channels (S, D, 16-bit count, mode) */
    uint32_t dma_s[4], dma_d[4];
    uint16_t dma_c[4];
    uint8_t dma_m[4];
    /* memory: 0x4000-0x7FFF, the 12 KiB of work RAM then the Z80's 4 KiB */
    uint8_t ram[NGP_RAM_BYTES];
    uint8_t *rom;                   /* the cartridge image, up to 4 MiB */
    uint32_t rom_size;
    uint8_t *bios;                  /* the synthesised BIOS */
    uint8_t rtc_latch[7];
    /* sound */
    uint8_t z80_enabled, chip_enabled, comm_byte;
    int32_t z80_runtime;
    /* T6W28: three squares (period, phase, left/right volumes) + noise */
    uint16_t sq_period[3];
    uint8_t sq_phase[3];
    uint8_t sq_volume_left[4], sq_volume_right[4];
    uint8_t noise_period_select, noise_tap, noise_shifter, noise_phase;
    uint16_t noise_period_extra;
    uint16_t sq_acc[3], noise_acc;  /* the chip's own phase accumulators */
    int32_t psg_runtime;            /* CPU ticks owed to the half-rate chip */
    uint8_t latch_left, latch_right;
    uint8_t dac_left, dac_right;
    /* frame */
    uint32_t cyc;                   /* CPU ticks since reset */
    uint32_t frames;
    uint8_t buttons;                /* 0x6F82 as the game reads it */
    uint8_t comm_status;            /* 0xB2 */
    uint8_t serial_buf;             /* 0x50 */
    /* flash writes through the cartridge windows land here (the saves) */
    uint8_t *flash_shadow;          /* unused: the writes go into the rom copy */
    uint32_t flash_dirty[NGP_FLASH_BLOCKS / 32];
    uint32_t flash_blocks;
    uint8_t flash_unlocked;         /* a BIOS FLASHWRITE in flight */
    uint8_t flash_command;          /* the SNK flash command latch */
    uint8_t flash_status;           /* reading 0x220000/0x230000 */
    /* the harness reads these */
    uint32_t last_pc, last_good_pc; /* the wild fetch and where it came from */
    uint32_t executed;
};

/* the machine instance; M0_TL makes a thread-local flavour for deepeel */
#ifndef M0_TL
#define M0_TL
#endif
extern M0_TL ngp_t machine;

void ngp_reset(void);
/* runs until the next V-INT moment (one frame); the picture lands in the
 * frame the harness reads through ngp_frame() */
unsigned ngp_run_frame(void);
/* the interpreter's one instruction; returns its cycles (the native code
 * calls ngp_tick with the same value after each region) */
int32_t ngp_step(void);
/* the recompiled code's one instruction (ngp_gen.c, under NGP_NATIVE):
 * the decode state handed in precomputed, the interpreter's own handlers
 * run; ngp_gen_exec takes a two-byte group form (the (mem) address and its
 * extra cycles folded), ngp_gen_exec1 a single-byte form.  The dispatcher
 * lives in ngp_step: ngp_gen_lookup names the translated entry at pc (the
 * hint checked first), ngp_gen_enter runs it. */
int32_t ngp_gen_exec(uint8_t f, uint8_t s, int size, uint8_t rcode, uint8_t brc,
                     uint32_t mem, int extra);
int32_t ngp_gen_exec1(uint8_t f);
int ngp_gen_lookup(uint32_t pc, int hint);
int32_t ngp_gen_enter(int entry);
/* the entry the executed instruction names as its successor (ngp_gen.c
 * seeds it before each handler run; -1: none) — the dispatcher's hint, so
 * sequential flow costs no hash search */
extern int ngp_gen_hint;
/* advance timers, video, the sound Z80 and interrupts by n CPU ticks */
void ngp_tick(uint32_t ticks);
/* the K2GE's line work: draws the line, raises H-INT/V-INT; returns 1 when
 * the frame boundary (line 152) was entered */
int ngp_gfx_line(void);
uint8_t ngp_gfx_read(uint32_t addr);
void ngp_gfx_write(uint32_t addr, uint8_t v);

uint8_t ngp_read(uint32_t addr);
uint8_t ngp_peek(uint32_t addr);   /* the read with no side effect (flash status latch untouched) */
void ngp_write(uint32_t addr, uint8_t v);
uint16_t ngp_read16(uint32_t addr);
void ngp_write16(uint32_t addr, uint16_t v);
uint32_t ngp_read32(uint32_t addr);
void ngp_write32(uint32_t addr, uint32_t v);

void ngp_interrupt(unsigned index, uint8_t level);
void ngp_set_int(unsigned index, int set);
void ngp_check_pending(void);
/* an IRQ source fired: a micro-DMA channel may name its vector, else it raises */
void ngp_test_int_hdma(unsigned index, uint8_t vector);

/* cartridge loading: points the machine at the image, reads the header */
int ngp_load_rom(const uint8_t *data, uint32_t bytes);
/* the flash writes as a save: the dirty-bit array then each dirty 8 KiB
 * block as (offset, bytes); io(false) writes, io(true) reads back */
uint32_t ngp_flash_bytes(void);
void ngp_flash_io(void *ctx,
                   void (*put)(void *ctx, const void *data, uint32_t n),
                   int (*get)(void *ctx, void *data, uint32_t n));

/* the rendered frame as RGB565, NGP_SCREEN_W x NGP_SCREEN_H */
const uint16_t *ngp_frame(void);
/* one audio frame of stereo s16 (the harness picks the sample rate) */
unsigned ngp_audio(int16_t *out, unsigned frames, unsigned rate);

/* state save/load (m0boy's stream contract: the byte count first) */
uint32_t ngp_state_bytes(void);
void ngp_state_io(void *data, uint32_t bytes, int load);

/* runtime internals (the machine's own files) */
/* the register-code file: bytes 0x00-0x3F name every bank's registers,
 * 0xE0+ the current bank, 0xF0+ the globals (words index r>>1, longs r>>2) */
uint8_t ngp_code_b(unsigned r);
void ngp_code_b_set(unsigned r, uint8_t v);
uint16_t ngp_code_w(unsigned r);
void ngp_code_w_set(unsigned r, uint16_t v);
uint32_t ngp_code_l(unsigned r);
void ngp_code_l_set(unsigned r, uint32_t v);
void ngp_z80_reset(void);
void ngp_z80_irq(void);
void ngp_z80_nmi(void);
void ngp_z80_run(uint32_t ticks);
void ngp_psg_reset(void);
void ngp_psg_tick(void);
void ngp_psg_write_left(uint8_t v);
void ngp_psg_write_right(uint8_t v);
uint8_t ngp_psg_read_left(void);
uint8_t ngp_psg_read_right(void);
void ngp_dac_write_left(uint8_t v);
void ngp_dac_write_right(uint8_t v);
void ngp_bios_install(void);
/* the BIOS hooks: opcode 0x1F inside the synthesised BIOS, dispatched by PC */
void ngp_bios_hle(void);

#ifdef __cplusplus
}
#endif
#endif
