/* The NES machine: the 6502, the PPU and the APU around 2 KiB of RAM and
 * an NROM cartridge (16 or 32 KiB of PRG mirrored per its size, 8 KiB of
 * CHR ROM).  A frame runs line by line — the CPU gets its 341/3 cycles,
 * then the PPU renders the line — so a mid-frame register write applies
 * from the next line, which is when Super Mario Bros makes its own.
 * One machine per firmware (as the Z80 runtime's own machine): the runtime
 * owns it, the host feeds machine.input (the standard controller bit order)
 * and takes the 160x128 picture through machine.ppu.panel/panel_out (ppu.h),
 * machine.ppu.skip leaving a frame out. */

/* the generated project's config first: NES_PPU_REFERENCE gates struct
 * fields (ppu.h, the row callback below) every translation unit must agree
 * on; the checkout has no nes_config.h, the harness passes the define. */
#if __has_include("nes_config.h")
#include "nes_config.h"
#endif
#ifndef NES_RUNTIME_H
#define NES_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "6502.h"
#include "apu.h"
#include "ppu.h"

/* nes.input: bit 0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right */
#define NES_A 0x01
#define NES_B 0x02
#define NES_SELECT 0x04
#define NES_START 0x08
#define NES_UP 0x10
#define NES_DOWN 0x20
#define NES_LEFT 0x40
#define NES_RIGHT 0x80

typedef struct {
    nes6502_t cpu;
    nes_ppu_t ppu;
    nes_apu_t apu;
    uint8_t ram[2048];
    uint8_t oam[256];
#ifndef NES_NO_PRG_RAM
#define NES_NO_PRG_RAM 0
#endif
#if !NES_NO_PRG_RAM
    uint8_t wram[8192];    /* cartridge PRG-RAM, $6000-$7FFF (MMC1/MMC3 boards) */
#endif
    uint8_t chr_ram[8192]; /* CHR-RAM cartridges': the PPU writes it through
                            * $2007 (ROM cartridges leave the board's) */
    const uint8_t *prg;
    uint32_t prg_size;
    const uint8_t *chr;
    uint8_t input;         /* the host writes it before each frame */
    uint8_t shift[2];      /* the controllers' serial shifters */
    uint8_t strobe;
    uint8_t pad_reads;     /* $4016/$4017 reads since the latch: bit0 pad1, bit1 pad2,
                            * 4-bit counters (the ninth read onward reports 1) */
    uint32_t dots;         /* PPU dots into the current line's CPU slice */
    uint32_t line;
    uint32_t frames;       /* completed nes_frame calls (the m0input probe) */
#ifdef NES_PPU_REFERENCE
    /* the reference renderer's lines (the host harness), 264 pixels in
     * fetch order: screen pixel c at c + ppu.x */
    void (*row)(void *host, unsigned line, const uint16_t *pixels);
#endif
    void *host;
} nes_t;

/* NROM: prg 16 or 32 KiB, chr 8 KiB, vertical_mirror from iNES flags 6. */
#ifndef M0_TL
#define M0_TL
#endif
extern M0_TL nes_t machine;
void nes_init(const uint8_t *prg, uint32_t prg_bytes,
              const uint8_t *chr, uint32_t chr_bytes, int vertical_mirror);
void nes_reset(void);
/* one frame (262 lines): emulation, the panel rows as their lines are
 * done.  Call once per frame. */
void nes_frame(void);
/* the current frame's remaining lines (an observer stopped the frame
 * mid-way: machine.line holds the next un-run line). */
void nes_frame_rest(void);
/* the frame's sound: n samples of unsigned PCM at rate Hz */
void nes_audio(uint8_t *dst, unsigned n, unsigned rate);
/* the consoles' drivers' entry points (the rate set, 11025 Hz until then) */
void nes_audio_set_rate(unsigned rate);
void nes_audio_render(uint8_t *buffer, unsigned n);
/* A frame-boundary save state; pointers and renderer caches are rebuilt on load. */
uint32_t nes_state_bytes(void);
void nes_state_io(void (*io)(void *data, uint32_t bytes, void *ctx), void *ctx, int load);
uint32_t nes_state_frame(void);
uint32_t nes_state_identity(void);

#endif


#ifdef __cplusplus
}
#endif
