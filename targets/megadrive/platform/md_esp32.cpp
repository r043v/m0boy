/* Mega Drive frontend for the ESP32 consoles (Gamebuino AKA, ODROID-GO):
 * the md_full machine sits in SRAM and there is no framebuffer — every VDP
 * line is rendered RGB565 straight into a small ring of 8-row batches that
 * the i80 DMA drains while the emulation goes on (the audio is decimated to
 * the family's 11025 Hz mono 8-bit ring).  On the AKA (MD_RENDER_THREAD) the
 * second core renders: the emulation only captures what a line needs besides
 * VRAM (MdLineCtx: registers, VSRAM, CRAM LUT) and the render task on core 0
 * draws each batch one batch behind, straight into the DMA ring.  VRAM is
 * read at render time, so a 68000 write during active display can show
 * through a line; the registers and the palette are the line's own.
 * Buttons: A, B, MENU/C = C, RUN/D = Start, the joystick completes the D-pad. */
#include <string.h>
extern "C" {
#include "md_full.h"
#include "m0_audio.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h"
void m0_lcd_wide_begin(uint8_t row0, uint8_t rows);
void m0_lcd_wide_reserve(void);
void m0_lcd_wide_submit(const void *batch, uint8_t rows);
void m0_lcd_wide_flush(void);
void m0_lcd_send_wide(const void *batch, uint8_t row0, uint8_t rows);
}
#include "m0_target.h"
#include "md_config.h"
/* aka_headless (tools/AKA_Emulator) never fires the transfers' done
 * interrupt, so AKA_EMU_SYNC=1 keeps the synchronous per-row send of the
 * other frontends for headless runs — used together with -DMD_RENDER_THREAD=0
 * (the emulator's core-0 tasks would starve otherwise, see
 * docs/AKA_EMU_PROBLEMES.fr.md):
 *   pio run -d <project> -DMD_RENDER_THREAD=0 -DAKA_EMU_SYNC=1 */
#ifndef AKA_EMU_SYNC
#define AKA_EMU_SYNC 0
#endif
#ifndef MD_RENDER_THREAD
#define MD_RENDER_THREAD 0
#endif
#if MD_RENDER_THREAD
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif

extern "C" const uint8_t md_rom[];
extern "C" const uint32_t md_rom_size;

static MdFull machine;

/* four 8-row batches; a batch buffer is refilled only after its slot has
 * been reserved, and the console driver's queue depth equals the ring, so
 * the transfer that still read it is then known to be finished */
#define RING_BATCHES 4
static uint16_t ring[RING_BATCHES][8][320] __attribute__((aligned(64)));

#if MD_RENDER_THREAD
/* two slots of eight captured lines: the emulation fills a slot batch by
 * batch, the render task draws it and hands it back — the emulation can
 * lead the second core by at most one batch of eight lines */
#define CTX_SLOTS 2
static MdLineCtx ctx[CTX_SLOTS][8];
static SemaphoreHandle_t slot_free[CTX_SLOTS], slot_draw[CTX_SLOTS];

/* called at the end of every active line, inside md_full_frame */
static void md_line_hook(void *, const MdFull *m, unsigned line) {
    const unsigned slot = (line >> 3) & (CTX_SLOTS - 1u);
    if ((line & 7u) == 0u) xSemaphoreTake(slot_free[slot], portMAX_DELAY);
    md_full_line_ctx(m, &ctx[slot][line & 7u]);
    if ((line & 7u) == 7u) xSemaphoreGive(slot_draw[slot]);
}

/* core 0: draw the captured batches in order, one frame of window per 28
 * batches, each straight into the DMA ring */
static void render_task(void *) {
    unsigned batch = 0;
    for (;;) {
        const unsigned slot = batch & (CTX_SLOTS - 1u);
        xSemaphoreTake(slot_draw[slot], portMAX_DELAY);
        if (batch % (MD_ROWS / 8u) == 0u) {
            m0_lcd_wide_flush();
            m0_lcd_wide_begin(MD_PANEL_ROW0, MD_ROWS);
        }
        m0_lcd_wide_reserve();
        uint16_t (*out)[320] = ring[batch & (RING_BATCHES - 1u)];
        const unsigned line0 = (batch % (MD_ROWS / 8u)) * 8u;
        for (unsigned l = 0; l < 8u; ++l)
            md_full_render_line565_ctx(&machine, &ctx[slot][l], line0 + l, out[l]);
        m0_lcd_wide_submit(out, 8);
        xSemaphoreGive(slot_free[slot]);
        ++batch;
    }
}
#else
/* called at the end of every active line, inside md_full_frame: render the
 * line RGB565 into the ring, hand full batches to the DMA queue */
static void md_line_hook(void *, const MdFull *m, unsigned line) {
    uint16_t (*r)[320] = ring[(line >> 3) & (RING_BATCHES - 1u)];
    if ((line & 7u) == 0u && !AKA_EMU_SYNC) m0_lcd_wide_reserve();
    md_full_render_line565(m, line, r[line & 7]);
    if ((line & 7u) == 7u) {
#if AKA_EMU_SYNC
        /* every row finds its own window on the sync path: send the
         * finished block instead of queueing it */
        m0_lcd_send_wide(r, MD_PANEL_ROW0 + (line & ~7u), 8);
#else
        m0_lcd_wide_submit(r, 8);
#endif
    }
}
#endif

static int16_t pcm[2 * 800];
static int held;
extern "C" void md_platform_audio_rate(unsigned hz) { (void)hz; }
/* n mono samples of the frame, each the average of four stereo 44.1 kHz
 * frames off the machine's stream (held when the stream runs short). */
extern "C" void md_platform_audio_render(uint8_t *out, unsigned n) {
    const unsigned got = md_full_audio_read(&machine, pcm, n * 4u);
    unsigned f = 0;
    for (unsigned i = 0; i < n; ++i) {
        int sum = 0, frames = 0;
        for (; frames < 4 && f < got; ++frames, ++f) sum += pcm[2 * f] + pcm[2 * f + 1];
        int v = frames ? (sum / (2 * frames)) >> 7 : held;
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        held = v;
        out[i] = (uint8_t)(v + 128);
    }
}

extern "C" int m0boy_console_main(void) {
    m0_lcd_init();
#if MD_VIDEO_ASM
    /* before the reset: the check borrows the machine's storage (it is far
     * too big for this task's stack) and the reset clears it again */
    if (md_span_selftest(&machine)) {   /* the tile-run kernel diverged from the C */
        m0_lcd_fill(M0_COLOR(0x00c00000));
        for (;;) { m0_watchdog_feed(); delay(1000); }
    }
#endif
    md_full_reset(&machine);
#if MD_RENDER_THREAD
    for (unsigned i = 0; i < CTX_SLOTS; ++i) {
        slot_free[i] = xSemaphoreCreateBinary();
        slot_draw[i] = xSemaphoreCreateBinary();
        xSemaphoreGive(slot_free[i]);
    }
    xTaskCreatePinnedToCore(render_task, "md_rend", 4096, NULL, 1, NULL, 0);
#endif
    machine.line_hook = md_line_hook;
    m0_audio_init();
    uint32_t deadline = m0_micros();
    for (;;) {
        machine.screen = NULL;
#if !MD_RENDER_THREAD && !AKA_EMU_SYNC
        m0_lcd_wide_begin(MD_PANEL_ROW0, MD_ROWS);
#endif
        md_full_frame(&machine, 1);   /* native regions, interpreter fallback */
        if (machine.fault) {
            m0_lcd_fill(M0_COLOR(0x00800000));
            for (;;) { m0_watchdog_feed(); delay(1000); }
        }
        m0_audio_frame();
        deadline += 1000000u / 60u;
        const uint32_t now = m0_micros();
        if ((int32_t)(now - deadline) > 0) deadline = now;   /* late: keep emulating */
        const uint8_t mb = esp32_buttons();
        unsigned pad = 0;
        if (mb & M0_MB_UP) pad |= 1;
        if (mb & M0_MB_DOWN) pad |= 2;
        if (mb & M0_MB_LEFT) pad |= 4;
        if (mb & M0_MB_RIGHT) pad |= 8;
        if (mb & M0_MB_A) pad |= 16;
        if (mb & M0_MB_B) pad |= 32;
        if (mb & M0_MB_HOME) pad |= 64;    /* MENU/C */
        if (mb & M0_MB_MENU) pad |= 128;   /* RUN/D: Start */
        machine.pad = (uint8_t)pad;
        m0_watchdog_feed();
    }
}
