/* PC Engine frontend for the ESP32 consoles (Gamebuino AKA, ODROID-GO):
 * the HuC6280/VDC runtime (recompiled code with the interpreter behind it),
 * the consoles' LCD, audio, clock and button drivers.  The runtime pushes
 * every rendered line here (pce.c's row callback): the VDC's active window
 * is planned onto the panel's 120 source rows (the glass doubles them,
 * M0_PANEL_SRC_ROWS = M0_SCREEN_ROWS = 120), the width is sampled 256 -> 160
 * with the 5-in-8 box filter (another active width: nearest pixel).  Two
 * source lines that land on one panel row are blended (m0_avg2_panel), so
 * no line is dropped. */
#include <cstring>
#include "pce_config.h"
#include "pce.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_audio.h"
#include "m0_lcd.h"
#if M0_TARGET_ESP32
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif
#else
#error "the PC Engine's console frontend is the ESP32 family's for now"
#endif
#include "m0_scale.h" /* the box filter (256 -> 160) and the vertical plan */
#include "m0_rows.h"
extern "C" {
extern const uint8_t pce_rom[];
}
static const char version[] __attribute__((used)) = "m0boy-pce-" M0_TARGET_NAME " " GBL_GEN_VERSION;

static uint16_t line_buffer[2][160] __attribute__((aligned(4)));
#if !PCE_TWO_CORE
static m0_rows rows;
#endif
static uint32_t deadline;
#ifndef PCE_MAX_SKIP
#define PCE_MAX_SKIP 2
#endif
static int skip_video;
static unsigned skipped_run;
static unsigned last_plan_row;
static int have_last;

/* The AKA's rows leave through the wide path's DMA queue (the Mega Drive
 * frontend's path): batches of eight panel rows, one address window per
 * frame, four transfers in flight — the emulation only stalls when every
 * queued transfer still reads a batch, and the bus time overlaps the next
 * frame's emulation.  aka_headless (tools/AKA_Emulator) never fires the
 * transfers' done interrupt, so AKA_EMU_SYNC=1 keeps the synchronous
 * per-row send of the other frontends for headless runs:
 *   pio run -d <project> -DAKA_EMU_SYNC=1
 * With PCE_RENDER_THREAD (pce_config.h, the AKA's projects) the second core
 * takes the pixel chain over, as the Mega Drive's MD_RENDER_THREAD: the
 * emulation only pushes the VDC's raw lines (9-bit palette indices, two
 * slots of eight lines of context) and core 0's task applies the VCE, the
 * 256->160 box filter, the panel's doubling and the DMA submits. */
#ifndef AKA_EMU_SYNC
#define AKA_EMU_SYNC 0
#endif
#ifndef PCE_RENDER_THREAD
#define PCE_RENDER_THREAD 0
#endif
#if __has_include("aka_hw.h") && !AKA_EMU_SYNC
#define PCE_WIDE_ROWS 1
extern "C" {
void m0_lcd_wide_begin(uint8_t row0, uint8_t rows);
void m0_lcd_wide_reserve(void);
void m0_lcd_wide_submit(const void *batch, uint8_t rows);
void m0_lcd_wide_flush(void);
}
#else
#define PCE_WIDE_ROWS 0
#endif
#if PCE_WIDE_ROWS && PCE_RENDER_THREAD
#define PCE_TWO_CORE 1
#else
#define PCE_TWO_CORE 0
#endif

static int display_row(unsigned line);
#if PCE_TWO_CORE
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/* four 8-row batches; a batch buffer is refilled only after its slot has
 * been reserved, and the console driver's queue depth equals the ring, so
 * the transfer that still read it is then known to be finished */
#define RING_BATCHES 4
static uint16_t ring[RING_BATCHES][8][320] __attribute__((aligned(64)));

/* two slots of eight pushed lines: the emulation fills a slot line by
 * line, the render task draws it and hands it back — the emulation can
 * lead the second core by at most one slot of eight lines */
#define PCE_CTX_WIDTH 512 /* the VDC's widest row */
#define CTX_SLOTS 2
typedef struct {
    int16_t r;         /* the line's plan row, -1: outside the window */
    uint8_t blend;     /* the second source line drawn on that plan row */
    uint8_t frame_start;
    uint16_t width;
    uint16_t raw[PCE_CTX_WIDTH];
} pce_ctx_line;
static pce_ctx_line ctx[CTX_SLOTS][8];
static SemaphoreHandle_t slot_free[CTX_SLOTS], slot_draw[CTX_SLOTS];
static unsigned pushed;

static void row(void *, unsigned line, const uint16_t *pixels, unsigned width) {
    (void)line;
    if (skip_video) return;
    const int r = display_row(line);
    if (r < 0) {
        have_last = 0;
        return;
    }
    const unsigned slot = (pushed >> 3) & (CTX_SLOTS - 1u);
    pce_ctx_line *c = &ctx[slot][pushed & 7u];
    if ((pushed & 7u) == 0u) xSemaphoreTake(slot_free[slot], portMAX_DELAY);
    c->r = (int16_t)r;
    c->blend = (uint8_t)(have_last && r == (int)last_plan_row);
    c->frame_start = (uint8_t)(r == 0 && !c->blend);
    c->width = (uint16_t)(width < PCE_CTX_WIDTH ? width : PCE_CTX_WIDTH);
    memcpy(c->raw, pixels, (size_t)c->width * 2);
    if ((pushed & 7u) == 7u) xSemaphoreGive(slot_draw[slot]);
    pushed++;
    last_plan_row = (unsigned)r;
    have_last = 1;
}

/* core 0: draw the pushed lines in order — the VCE's lookup, the 256->160
 * box filter, the blend of the pair's second line, the panel's doubling —
 * straight into the DMA ring (-O2: the project builds -Os, this loop is
 * the render core's whole budget) */
__attribute__((optimize("-O2")))
static void render_task(void *) {
    static uint16_t prev_row[160];
    unsigned consumed = 0;
    int open = -1; /* the batch (plan row >> 2) being filled */
    for (;;) {
        const unsigned slot = (consumed >> 3) & (CTX_SLOTS - 1u);
        xSemaphoreTake(slot_draw[slot], portMAX_DELAY);
        for (unsigned i = 0; i < 8u; i++) {
            const pce_ctx_line *c = &ctx[slot][i];
            if (c->frame_start) {
                if (open >= 0) { m0_lcd_wide_submit(ring[open & (RING_BATCHES - 1u)], 8); open = -1; }
                m0_lcd_wide_flush();
                m0_lcd_wide_begin(0, 240);
            }
            if (c->r < 0) continue;
            const unsigned b = (unsigned)c->r >> 2;
            if ((int)b != open) {
                if (open >= 0) m0_lcd_wide_submit(ring[open & (RING_BATCHES - 1u)], 8);
                m0_lcd_wide_reserve();
                open = (int)b;
            }
            const pce_t *p = &machine;
            uint16_t tmp[160];
            const unsigned w = c->width;
            if (w == 256) {
                for (unsigned x = 0; x < 256; x += 8) {
                    uint32_t s[8];
                for (unsigned j = 0; j < 8; j++) {
                    const uint16_t px = c->raw[x + j];
                    s[j] = m0_spread565(p->vce_final[px & 0x1FF]);
                }
                    m0_box58_fast(s, tmp + x / 8 * 5);
                }
            } else {
                for (unsigned x = 0; x < 160; x++) {
                    const uint16_t px = c->raw[(unsigned)((uint32_t)x * w / 160)];
                    tmp[x] = p->vce_final[px & 0x1FF];
                }
            }
            if (c->blend) {
                for (unsigned x = 0; x < 160; x++) tmp[x] = m0_avg2_panel(prev_row[x], tmp[x]);
            } else {
                memcpy(prev_row, tmp, sizeof tmp);
            }
            uint16_t *a = ring[b & (RING_BATCHES - 1u)][(c->r & 3u) * 2u];
            uint16_t *d = a + 320;
            /* m0_nearest(x, 160, 320) is (x + 1) / 2; the panel doubles the
             * row, so both panel rows carry the same pixels */
            for (unsigned x = 0; x < 320; x++) {
                const unsigned j = (x + 1) >> 1;
                a[x] = d[x] = m0_panel(tmp[j < 160 ? j : 159]);
            }
            if ((c->r & 3u) == 3u) {
                m0_lcd_wide_submit(ring[b & (RING_BATCHES - 1u)], 8);
                open = -1;
            }
        }
        xSemaphoreGive(slot_free[slot]);
        consumed += 8;
    }
}
#else /* single core: the rows compose and leave on this core */
#if PCE_WIDE_ROWS
/* one batch of eight panel rows (four source rows doubled) */
static uint16_t batch[8][320] __attribute__((aligned(64)));
static void send_row(const uint16_t *src, unsigned r) {
    if (r == 0) {
        m0_lcd_wide_flush();
        m0_lcd_wide_begin(0, 240);
    }
    if ((r & 3u) == 0) m0_lcd_wide_reserve();
    const unsigned l = (r & 3u) * 2u;
    /* m0_nearest(x, 160, 320) is (x + 1) / 2; the panel doubles the row,
     * so both panel rows carry the same pixels */
    uint16_t *a = batch[l], *b = batch[l + 1];
    for (unsigned x = 0; x < 320; x++) {
        const unsigned i = (x + 1) >> 1; /* m0_nearest(x, 160, 320) */
        a[x] = b[x] = m0_panel(src[i < 160 ? i : 159]);
    }
    if ((r & 3u) == 3u) m0_lcd_wide_submit(batch, 8);
    rows.slot ^= 1; /* m0_rows_send's flip: the blend reads the previous line */
}
#else
static void send_row(const uint16_t *src, unsigned r) {
    m0_rows_send(&rows, src, r, 1);
}
#endif
#endif

static void row(void *, unsigned line, const uint16_t *pixels, unsigned width);
static void pce_start(void) {
    pce_init(pce_rom, PCE_ROM_SIZE);
    machine.row = row; /* pce_init clears it: every rendered line comes here */
    machine.row_raw = PCE_TWO_CORE; /* the raw lines go to the render core */
}
/* The active window's plan: [first_active_line, +active_height) onto the
 * panel's 120 rows.  The VDC's window is the game's own (VDW/ VDS), read
 * live: a game that reprograms it mid-frame replans from the next line. */
static int display_row(unsigned line) {
    const unsigned first = machine.first_active_line, height = machine.vdc.active_height;
    if (!machine.have_frame || height < 24 || line < first || line - first >= height) return -1;
    return (int)m0_row_of(line - first, height, 120);
}
#if !PCE_TWO_CORE
static void row(void *, unsigned line, const uint16_t *pixels, unsigned width) {
    if (skip_video) return;
    const int r = display_row(line);
    if (r < 0) {
        have_last = 0;
        return;
    }
    uint16_t *dst = line_buffer[rows.slot];
    if (width == 256) {
        for (unsigned x = 0; x < 256; x += 8) {
            uint32_t s[8];
            for (unsigned i = 0; i < 8; i++) s[i] = m0_spread565(pixels[x + i]);
            m0_box58_fast(s, dst + x / 8 * 5);
        }
    } else {
        for (unsigned x = 0; x < 160; x++)
            dst[x] = m0_panel(pixels[(unsigned)((uint32_t)x * width / 160)]);
    }
    if (have_last && r == (int)last_plan_row) {
        /* the second line of the pair blends over the first (already sent):
         * the averaged row goes out from the other buffer */
        const uint16_t *prev = line_buffer[rows.slot ^ 1];
        for (unsigned x = 0; x < 160; x++) dst[x] = m0_avg2_panel(prev[x], dst[x]);
        last_plan_row = (unsigned)r;
        send_row(dst, (unsigned)r);
        return;
    }
    last_plan_row = (unsigned)r;
    have_last = 1;
    send_row(dst, (unsigned)r);
}
#endif /* !PCE_TWO_CORE */
/* ---- option menu (sg_menu.h): HOME held half a second, MENU stays Pause -- */
extern "C" uint16_t *m0_menu_scratch(unsigned *rows_out) {
    *rows_out = 2;
    return line_buffer[0];
}
#define SG_MENU_TAP() do { } while (0)
#define SG_MENU_RESTART() do { \
        pce_start(); \
        deadline = m0_micros(); \
        skipped_run = 0; \
    } while (0)
#define SG_MENU_QUIT() do { esp32_restart(); } while (0)
#define SG_MENU_RESUME() do { \
        m0_lcd_fill(0); \
        deadline = m0_micros(); \
        skip_video = 0; \
        skipped_run = 0; \
    } while (0)
#include "sg_frontend.h" /* the console setup, the pad, the pacing, the stack guard */
#include "sg_menu.h"

/* the stack guard's target (sg_frontend.h's SG_STACK_GUARD writes it; the
 * Sega runtimes own the symbol, the PCE's lives here) */
extern "C" {
uintptr_t sg_stack_limit;
}
/* the Arduino core calls m0boy_console_main (esp32_hw.cpp's setup) */
extern "C" int m0boy_console_main(void);
extern "C" int m0boy_console_main() {
    SG_STACK_PAINT();
    SG_PLATFORM_INIT();
    pce_start();
    SG_STACK_GUARD();
#if PCE_TWO_CORE
    for (unsigned i = 0; i < CTX_SLOTS; ++i) {
        slot_free[i] = xSemaphoreCreateBinary();
        slot_draw[i] = xSemaphoreCreateBinary();
        xSemaphoreGive(slot_free[i]);
    }
    xTaskCreatePinnedToCore(render_task, "pce_rend", 4096, NULL, 1, NULL, 0);
#endif
    m0_audio_init();
    deadline = m0_micros();
    /* Keep an inspectable source/target version stamp in the firmware. */
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        pce_frame();
        m0_audio_frame();
        m0_lcd_end_frame();
        const uint8_t mb = M0_PAD(SG_PAD_RAW());
        unsigned keys = 0;
        if (mb & M0_MB_UP) keys |= PCE_BTN_UP;
        if (mb & M0_MB_DOWN) keys |= PCE_BTN_DOWN;
        if (mb & M0_MB_LEFT) keys |= PCE_BTN_LEFT;
        if (mb & M0_MB_RIGHT) keys |= PCE_BTN_RIGHT;
        if (mb & M0_MB_A) keys |= PCE_BTN_I;
        if (mb & M0_MB_B) keys |= PCE_BTN_II;
        if (mb & M0_MB_MENU) keys |= PCE_BTN_RUN; /* the AKA's RUN or D */
        machine.input = (uint8_t)keys;
        sg_menu_frame();
        sg_menu_poll(mb);
        sg_menu_tick();
        SG_PACE_FRAME(deadline, skip_video, skipped_run, PCE_MAX_SKIP);
    }
}
