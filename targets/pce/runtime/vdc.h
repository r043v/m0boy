/* The VDC (HuC6270): 64 KiB of VRAM, the background and the sprites, the
 * SATB and VRAM DMAs, and the interrupt sources the CPU polls.  The line
 * model follows the mednafen core's phase machine, one step a line: the
 * registers cache at the VSW entry, the scroll latches and the sprite
 * fetch at the line start, the row composed where the active area would
 * start.  The rows leave as 9-bit palette indices (0-511) in linebuf; the
 * VCE's table turns them into pixels (pce.c).  What the model leaves out,
 * on purpose, for now: the CPU wait states on VDC access, the DMA's cycle
 * stealing (a DMA completes at once), the sprite CG fetch's own window,
 * the BSY bit of the status (always clear) and the one-word sprite CG
 * mode.  A mid-line scroll write takes effect on the next line. */
#ifndef PCE_VDC_H
#define PCE_VDC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define VDC_VRAM_WORDS 32768 /* 64 KiB, addressed as 16-bit words */

/* status bits, cleared by the status read */
#define VDCS_CR 0x01 /* sprite #0 collision */
#define VDCS_OR 0x02 /* sprite overflow */
#define VDCS_RR 0x04 /* the scanline compare (RCR) */
#define VDCS_DS 0x08 /* the SATB DMA finished */
#define VDCS_DV 0x10 /* the VRAM DMA finished */
#define VDCS_VD 0x20 /* vertical blanking */

typedef struct {
    uint16_t vram[VDC_VRAM_WORDS];
    uint16_t sat[256]; /* the 64 sprites' 4 words, copied by the SATB DMA */
    uint8_t select;
    uint16_t mawr, marr;
    uint16_t cr, rcr, bxr, byr, mwr;
    uint16_t hsr, hdr, vsr, vdr, vcr;
    uint16_t dcr, sour, desr, lenr, dvssr;
    uint16_t read_buffer;
    uint8_t write_latch;
    uint8_t status;
    /* the line machine's state */
    int vphase, vcounter; /* the phase machine, one step a line */
    int rcr_count;
    int burst_mode;
    int bg_y;             /* the BYR latch, one a line */
    int vb_test, sat_test;
    uint8_t sat_pending;
    /* the row being composed */
    uint16_t linebuf[1024] __attribute__((aligned(8)));
    unsigned row_width;   /* (HDW+1)*8 */
    /* what the display shows this frame */
    unsigned active_width, active_height;
    int active_started;
} vdc_t;

void vdc_reset(vdc_t *v);
/* the CPU's access: at 0-3 within the VDC's window */
uint8_t vdc_read(vdc_t *v, unsigned at);
void vdc_write(vdc_t *v, unsigned at, uint8_t value);
/* one line of the frame: the phase machine's step, the scroll latches,
 * the DMAs' triggers, and the row composed when the line is in the active
 * area (linebuf holds row_width 9-bit indices, else the backdrop's 0) */
void vdc_line(vdc_t *v, unsigned line);

#ifdef __cplusplus
}
#endif
#endif
