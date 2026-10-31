/* The PC Engine machine: the HuC6280, the VDC, the VCE's palette, the
 * PSG, the timer and the pad around 8 KiB of WRAM and the cartridge.  A
 * frame runs line by line — the CPU gets its 455 cycles (1365 master
 * clocks at 7.16 MHz), then the VDC's line work runs — so a mid-frame
 * register write applies from the next line's latches.  One machine per
 * firmware (as the other runtimes' own machine): the runtime owns it, the
 * host feeds machine.input (the standard button bit order) and draws
 * through machine.row(). */
#ifndef PCE_RUNTIME_H
#define PCE_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "huc6280.h"
#include "vdc.h"
#include "psg.h"

/* pce.input: bit 0 I, 1 II, 2 Select, 3 Run, 4 Up, 5 Right, 6 Down, 7 Left */
#define PCE_BTN_I      0x01
#define PCE_BTN_II     0x02
#define PCE_BTN_SELECT 0x04
#define PCE_BTN_RUN    0x08
#define PCE_BTN_UP     0x10
#define PCE_BTN_RIGHT  0x20
#define PCE_BTN_DOWN   0x40
#define PCE_BTN_LEFT   0x80

typedef struct {
    huc6280_t cpu;
    vdc_t vdc;
    psg_t psg;
    uint8_t wram[8192];
    uint16_t vce_colors[512]; /* the 9-bit words */
    uint16_t vce_lut[512];    /* RGB565 */
    uint8_t vce_cr;
    unsigned vce_address;
    /* the timer: 1024 CPU cycles a tick, the IRQ latched to the ack */
    uint8_t timer_load;
    int timer_value;   /* -1 during the reload gap (reads as $FF) */
    uint8_t timer_enable;
    uint8_t timer_reload;
    uint8_t timer_irq;
    uint32_t timer_clock;
    /* the pad: the host's byte, the port's SEL latch */
    uint8_t input;
    uint8_t pad_sel;
    uint32_t frames;
    /* what the display showed this frame (the harness's window) */
    unsigned active_width, active_height, first_active_line;
    uint8_t have_frame;
    uint16_t rowbuf[1024];
    /* row_raw: the row callback takes the VDC's linebuf (9-bit palette
     * indices) instead of the rowbuf's RGB565 — the two-core frontends
     * apply the VCE on the render core */
    unsigned row_raw;
    void (*row)(void *host, unsigned line, const uint16_t *pixels, unsigned width);
    void *host;
} pce_t;

/* rom must live as long as the machine; its size is padded to 8 KiB
 * pages by the project generator */
extern pce_t machine;
void pce_init(const uint8_t *rom, uint32_t rom_bytes);
void pce_reset(void);
/* one frame (262 lines): emulation, then the row callback has drawn the
 * frame's lines (256 or 352 pixels wide).  Call once per frame. */
void pce_frame(void);
/* the frame's sound: n samples of unsigned PCM at rate Hz */
void pce_audio(uint8_t *dst, unsigned n, unsigned rate);
/* the consoles' drivers' entry points (fixed 11025 Hz) */
void pce_audio_set_rate(unsigned rate);
void pce_audio_render(uint8_t *buffer, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
