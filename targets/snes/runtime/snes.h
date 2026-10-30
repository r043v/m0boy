/* The 5A22 (WDC 65C816): the 6502's registers widened (C as B:A, X and Y
 * with hidden high bytes), the direct page, the two bank registers and the
 * emulation flag.  The interpreter runs the documented opcode set — every
 * byte is a defined opcode, $42 WDM is a two-byte NOP.  The bus lands on
 * snes_bus_read/snes_bus_write: the LoROM or HiROM windows of the
 * cartridge, 128 KiB of WRAM with its mirrors, and a partial I/O page
 * (PPU ports, DMA, NMI and WRAM port). The zero page
 * is D's window in bank $00 and the stack is bank $00, S — never banked.
 *
 * Phase 1 facts, to be cross-checked like the PCE's mednafen pass:
 * the cycle counts follow the WDC manual's matrix (the D-page, width and
 * page-cross penalties included), BRK/COP push PC+2 in both modes, the
 * WAI/STP sleep spends the rest of the slice, and the frame is 59562 CPU
 * cycles (the slow-ROM divide is not modelled). */
#ifndef SNES_CPU_H
#define SNES_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SNES_P_C 0x01
#define SNES_P_Z 0x02
#define SNES_P_I 0x04
#define SNES_P_D 0x08
#define SNES_P_X 0x10   /* 1: index registers 8-bit (pushed as B) */
#define SNES_P_M 0x20   /* 1: accumulator 8-bit */
#define SNES_P_V 0x40
#define SNES_P_N 0x80

/* the frame: 262 lines of 1364 master clocks, the CPU at master/6 (the
 * phase-1 runtime runs the same rate whatever MEMSEL says) */
#define SNES_FRAME_CYCLES 59562u

typedef struct {
    uint16_t a;      /* C: B in the high byte, A in the low */
    uint16_t x, y;   /* the high bytes stay zero while the X flag is set */
    uint16_t s;
    uint16_t d;      /* the direct page */
    uint8_t k;       /* the program bank */
    uint8_t dbr;     /* the data bank */
    uint8_t p;       /* N V M X D I Z C; B exists only on the stack */
    uint8_t e;       /* the emulation flag (not part of P) */
    uint16_t pc;
    uint32_t cycles;
    uint8_t mdr;     /* the open bus's last value */
    /* the bus */
    uint8_t *wram;   /* 128 KiB at $7E0000 */
    const uint8_t *rom;
    uint32_t rom_bytes;
    int hirom;
    /* the phase-1 I/O state */
    int nmi_enable;
    int vblank_flag;
    int nmi_pending; /* the frame's edge, latched */
    int irq_line;    /* the level of $4211's TIMEUP: set by the H/V timers */
    int halted;      /* STP: the CPU stopped until the next reset */
    int waiting;     /* WAI: waits for an interrupt request */
    uint32_t wram_addr; /* the $2181-$2183 port */
} snes_cpu_t;

/* the S-SMP (SPC700): the interpreter's state, the 64 KiB A-RAM and the
 * four comm ports the CPU sees at $2140-$217F.  The boot's IPL ROM is
 * simulated by a state machine (no ROM bytes), the timers tick at 8 kHz
 * (T0/T1) and 64 kHz (T2), their outputs at source/(target+1). */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, sp, psw;
    uint8_t ram[65536];
    uint8_t ports_in[4], ports_out[4];
    uint8_t control;    /* $F1: the timer run bits */
    uint8_t dsp_addr;   /* $F2 */
    uint8_t halted;     /* SLEEP/STOP */
    uint8_t timer_phase[3], timer_out[3];
    uint8_t ipl_state;  /* 0: ready, 1: command wait, 2: uploading, 3: program runs */
    uint8_t ipl_y;      /* the block's byte counter, echoed on port 0 */
    uint8_t ipl_pending, ipl_fresh1; /* the byte-transfer handshaking */
    uint16_t ipl_addr;  /* $2142/3: the block's page or the entry point */
    uint8_t tick8k, tick64k;
    uint64_t acc, dsp_acc;
} snes_smp_t;

/* the S-DSP: the register file the SMP sees at $F2/$F3, the eight BRR
 * voices and the sample ring the host drains (32000 Hz stereo). */
typedef struct {
    uint8_t regs[0x80];
    uint8_t key_on, key_off, end_flags;
    int16_t ring[8192];
    uint16_t ring_r, ring_w;
    struct {
        int16_t prev1, prev2;  /* the BRR filter's history */
        int16_t out;           /* the voice's latest decoded sample */
        uint16_t start, loop;  /* block addresses from the directory */
        uint16_t src;          /* the current block's byte address */
        uint16_t pos;          /* the fixed-point read position (1/4096 samples) */
        uint8_t hdr;           /* the current block's header */
        uint8_t nib;           /* the next sample nibble in the block */
        uint8_t adsr_state;    /* 0 gain, 1 attack, 2 decay, 3 sustain, 4 release */
        uint8_t rate_count;
        int16_t env;
    } voice[8];
} snes_dsp_t;

/* One machine: CPU, WRAM, the cartridge's SRAM, PPU memories, the APU and
 * general DMA state. */
typedef struct {
    snes_cpu_t cpu;
    uint8_t wram[131072];
    uint8_t sram[32768]; /* the cartridge's battery window $6000-$7FFF */
    uint32_t sram_bytes; /* what the header declared, a power of two or 0 */
    uint8_t sram_dirty;
    uint8_t vram[65536], cgram[512], oam[544];
    uint8_t ppu_regs[0x40], dma[8][16];
    uint8_t hdma_enable;  /* $420C, write only */
    uint8_t hdma_run[8], hdma_done[8];  /* started / ended this frame */
    uint8_t hdma_count[8], hdma_len[8], hdma_rep[8]; /* the current table entry */
    uint16_t vmaddr, cgaddr, oamaddr, scroll[8];
    uint8_t cg_latch, oam_latch, scroll_latch;
    uint8_t m7[12];      /* the mode 7 matrix A-D, then the center X and Y */
    uint8_t m7_lo, m7_phase; /* the write-twice latch of $211B-$2120 */
    uint8_t obj_range_over, obj_time_over; /* the $213F flags of the last evaluated line */
    uint8_t timer_latch;
    uint8_t mpy_a, div_l, div_h; /* the multiply/divide unit's operands */
    uint16_t mpy_product, div_quot; /* the unit's latched results */
    uint8_t div_rem, mpyed; /* $4216 shows the remainder until a new multiply */
    uint16_t htime, vtime; /* $4207-$420A, the H/V compare values */
    uint8_t timeup;        /* $4211 bit 7, cleared by the read */
    uint8_t h_irq_en, v_irq_en; /* $4200 bits 4-5 */
    uint8_t vblank, hblank;
    uint8_t screen[224 * 256 * 3]; /* the frame as the lines displayed it */
    uint16_t joypad, joy_auto, joy_shift;
    uint8_t joy_strobe, joy_count, joy_auto_enable, joy_busy;
    snes_smp_t smp;
    snes_dsp_t dsp;
    uint32_t frames;
} snes_t;

/* rom must live as long as the machine; sram_bytes is the cartridge's
 * declared battery size (0: the SRAM window stays open bus) */
extern snes_t machine;
void snes_init(const uint8_t *rom, uint32_t rom_bytes, int hirom, uint32_t sram_bytes);
void snes_reset(void);
/* run until the cycle count reaches `until` (past the last instruction
 * that spends it, as the budget goes negative) */
void snes_run(snes_cpu_t *cpu, uint32_t until);
/* one frame of emulation (the harness calls it once per frame) */
void snes_frame(void);
/* Standard controller report: BYsS UDLR AXLR 0000, pressed bits set. */
void snes_set_joypad(uint16_t buttons);
/* Render one line from the current registers as 256 RGB888 pixels. */
void snes_render_line(unsigned y, uint8_t *row);
/* The whole 224-line frame from the current registers (the reference the
 * tests render; the frame loop fills machine.screen itself). */
void snes_render(uint8_t *rgb);

/* the bus: everything but the ROM/WRAM windows */
uint8_t snes_bus_read(uint32_t at);
void snes_bus_write(uint32_t at, uint8_t v);
/* the interrupt sequence on the machine's fields (the dispatcher's;
 * the B bit clear on the stack, D cleared) */
void snes_take_interrupt(snes_cpu_t *c, uint16_t vector);

/* the entry key the dispatcher and the recompiler share: K, DBR, the
 * direct page and the M/X/E flags packed, K lowest */
uint64_t snes_ctx_key(const snes_cpu_t *cpu);

/* the S-SMP: advance by `cpu_cycles` of the 65816's clock (the fixed
 * master ratio), interpret the SPC700 and tick the timers; the DSP
 * renders into the ring at 32000 Hz.  The IPL boot runs on the CPU's
 * writes through snes_smp_port_write. */
void snes_apu_run(uint32_t cpu_cycles);
void snes_smp_port_write(unsigned n, uint8_t v);
/* drain n stereo samples the host (16-bit interleaved), for --wav and
 * audio_hash */
unsigned snes_apu_read(int16_t *dst, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
