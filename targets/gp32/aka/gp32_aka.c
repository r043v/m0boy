/* GP32 firmware for the Gamebuino AKA (ESP32-S3): the m0boy GP32 machine in
   PSRAM, its LCD turned onto the 320 x 240 panel, buttons and sound. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "gp32_hle.h"
#include "gp32_blit.h"
#include "aka_hw.h"
#include "gp32_payload.h"
#ifdef M0_GP32_PAGED_CARD
#include "gp32_aka_card.h"
#endif

/* aka_hw.cpp: the display task of core 0. */
void m0_lcd_fill(uint16_t color);
void m0_lcd_flush(void);
void m0_lcd_send_batch(const void *batch, unsigned row0, unsigned rows);
void m0_display_start(void (*draw)(void));
int m0_display_idle(void);
void m0_display_post(void);
void esp32_audio_start(unsigned hz);
void esp32_audio_pull(uint8_t *out, unsigned count);

/* aka_hw.h's M0_MB_* bits. */
enum {
    KEY_DOWN = 0x01, KEY_LEFT = 0x02, KEY_RIGHT = 0x04, KEY_UP = 0x08,
    KEY_A = 0x10, KEY_B = 0x20, KEY_MENU = 0x40, KEY_HOME = 0x80
};

/* The panel in its own orientation, which is the GP32's: row x is LCD line x,
   from the bottom of the held console upwards. */
#define PANEL_WIDTH 240u
#define PANEL_ROWS 320u
#define BATCH ((unsigned)GP32_BLIT_LINES)
#define BATCHES (PANEL_ROWS / BATCH)
/* Pages of the picture the LCD shows, moved to internal memory at run time:
   at most this many, as long as this much internal memory stays free. */
/* Internal pages for the shown picture: one 320x240 buffer of 16 bit pixels,
   or the two buffers of an 8 bit program. */
#define VIDEO_PAGES 12u
#ifndef M0_GP32_AKA_RESERVE
#define M0_GP32_AKA_RESERVE (16u * 1024u)
#endif
/* A page not shown for this many drawn frames may leave internal memory. */
#define VIDEO_IDLE 30u
/* Instructions cost this many FCLK cycles: fewer instructions per frame. */
#ifndef M0_GP32_AKA_CPI
#define M0_GP32_AKA_CPI 16
#endif
#define AUDIO_RATE 11025u
/* The assembly routines of gp32_blit.S; 0 keeps the C ones. */
#ifndef M0_GP32_AKA_PIE
#define M0_GP32_AKA_PIE 1
#endif
#if defined(__XTENSA__) && M0_GP32_AKA_PIE
#define HAVE_KERNELS 1
#else
#define HAVE_KERNELS 0
#endif
/* Frameskip.  N >= 0: one LCD frame drawn, then at least N left out.  -1
   (automatic): the next picture waits until the display has rested twice as
   long as the last one took, so that it uses the shared PSRAM at most a third
   of the time; never more than 60 pictures per second, never under 10.  The
   program itself still computes every frame. */
#ifndef M0_GP32_AKA_FRAMESKIP
#define M0_GP32_AKA_FRAMESKIP -1
#endif
/* 16 KiB pages of the guest's SDRAM held in internal memory, and which ones.
   The PSRAM holds the rest; it has no room for all 8 MiB beside the heap. */
#ifndef M0_GP32_AKA_FAST_PAGES
#define M0_GP32_AKA_FAST_PAGES 0, 1, 2, 3
#endif

static S3c2400 *soc;
static const uint16_t fast_list[] = {M0_GP32_AKA_FAST_PAGES};
#define FAST_PAGES (sizeof fast_list / sizeof *fast_list)
static uint8_t fast[FAST_PAGES][GP32_RAM_PAGE_SIZE] __attribute__((aligned(32)));
/* One batch is converted while the other is on its way to the panel. */
static uint16_t strip[2][BATCH * PANEL_WIDTH] __attribute__((aligned(64)));
static uint16_t colors[256] __attribute__((aligned(16)));
/* What the display core draws from: the bus, the LCD registers and the
   palette as they were when the frame was handed over. */
static S3c2400 *view;
/* An LCD line which is not one run of host memory. */
static uint32_t spare[GP32_LCD_WIDTH / 2] __attribute__((aligned(16)));
/* A checksum of each batch on the panel, and the weights it is made with. */
static uint64_t shown_hash[BATCHES];
static int16_t weights[PANEL_WIDTH] __attribute__((aligned(16)));
/* The display core's copy of the palette registers. */
static uint32_t palette[256] __attribute__((aligned(16)));
static const uint16_t constants[3] = {0xffc0, 0x001f, 0x0001};
static bool kernels; /* the assembly routines passed their check */
static unsigned display_us, fast_lines;
/* Duration of the last picture, read once the display core is idle again. */
static uint32_t last_us;
static struct {
    uint8_t *at, *home;
    uint32_t used;
    int page;
} video[VIDEO_PAGES];
static unsigned video_pages;

static uint8_t ring[1024];
static volatile unsigned ring_head, ring_tail;
static uint32_t phase;

/* Out of line: an emulator of the board supplies it by name. */
__attribute__((noinline)) uint32_t m0_micros(void) { return (uint32_t)esp_timer_get_time(); }
void m0_watchdog_feed(void) {}

/* Fast pages go to the internal array, the others to the PSRAM, in as many
   blocks as its heap needs: its largest free block does not hold them all.
   Returns the first PSRAM block, NULL when memory runs out. */
static uint8_t *map_ram(void)
{
    uint8_t *first = NULL, *slow = NULL;
    size_t wanted = GP32_RAM_PAGES - FAST_PAGES, held = 0;
    for (unsigned page = 0; page < GP32_RAM_PAGES; ++page) {
        uint8_t *at = NULL;
        for (unsigned i = 0; i < FAST_PAGES; ++i)
            if (fast_list[i] == page)
                at = fast[i];
        if (!at) {
            if (!held) {
                held = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / GP32_RAM_PAGE_SIZE;
                if (held > wanted)
                    held = wanted;
                /* The allocator may refuse the very size it reports. */
                while (held && !(slow = heap_caps_aligned_alloc(16, held * GP32_RAM_PAGE_SIZE,
                                                        MALLOC_CAP_SPIRAM)))
                    --held;
                if (!held)
                    return NULL;
                if (!first)
                    first = slow;
            }
            at = slow;
            slow += GP32_RAM_PAGE_SIZE;
            --held;
            --wanted;
        }
        gp32_ram_page[page] = (uintptr_t)at - ((uintptr_t)page << GP32_RAM_PAGE_BITS);
    }
    return first;
}

/* The machine's samples, reduced to the panel path's 11025 Hz mono bytes. */
static void sample(void *context, int16_t left, int16_t right, unsigned rate)
{
    (void)context;
    phase += AUDIO_RATE;
    while (rate && phase >= rate) {
        phase -= rate;
        unsigned head = ring_head, next = (head + 1) & (sizeof ring - 1);
        if (next != __atomic_load_n(&ring_tail, __ATOMIC_ACQUIRE)) {
            ring[head] = (uint8_t)(((left + right) >> 9) + 128);
            __atomic_store_n(&ring_head, next, __ATOMIC_RELEASE);
        }
    }
}

void esp32_audio_pull(uint8_t *out, unsigned count)
{
    static uint8_t last = 128;
    while (count--) {
        unsigned tail = ring_tail;
        if (tail != __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE)) {
            last = ring[tail];
            __atomic_store_n(&ring_tail, (tail + 1) & (sizeof ring - 1), __ATOMIC_RELEASE);
        }
        *out++ = last;
    }
}

/* RGB 5:5:5 and a shared low bit, to the panel's RGB 5:6:5. */
static inline uint16_t rgb565(unsigned c)
{
    return (uint16_t)((c & 0xffc0u) | (c & 1u) << 5 | (c >> 1 & 31u));
}

/* Any format, one line: pixel c of the row is column c of the LCD line. */
static void convert(uint16_t *out, const uint8_t *bytes, const uint32_t *header)
{
    unsigned format = header[GP32_LCD_FORMAT], count = header[GP32_LCD_COUNT];
    unsigned bits = format & 255u, lanes = format >> 8 & 3u;
    const uint32_t *words = (const uint32_t *)(const void *)bytes;
    for (unsigned column = 0; column < PANEL_WIDTH; ++column) {
        uint16_t pixel = 0;
        if (column < count) {
            if (!words) {
                pixel = rgb565(header[GP32_LCD_COLOR]);
            } else {
                unsigned offset = column * bits;
                uint32_t data = words[offset >> 5], swapped = 0;
                for (unsigned i = 0; i < 4; ++i)
                    swapped |= (data >> ((i ^ lanes) * 8) & 255u) << (i * 8);
                unsigned value = swapped >> (32 - bits - (offset & 31u)) & ((1u << bits) - 1u);
                pixel = bits != 16 ? colors[value] : rgb565(value);
            }
        }
        out[column] = pixel;
    }
}

static void line8_reference(uint16_t *out, const uint8_t *in, const uint16_t *table,
                            unsigned turn)
{
    for (unsigned c = 0; c < PANEL_WIDTH; ++c)
        out[c] = table[in[c ^ turn]];
}

/* Odd weights from -2047 to 2047, one for each pixel of a line and no two the
   same: a single changed pixel changes its line's sum, and the sum of 240
   products fits in 40 bits. */
static void make_weights(void)
{
    uint32_t seed = 0x2545f491u;
    for (unsigned i = 0; i < PANEL_WIDTH; ++i) {
        bool taken;
        do {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            weights[i] = (int16_t)((int)(seed >> 8 & 0xffeu) - 2047);
            taken = false;
            for (unsigned j = 0; j < i; ++j)
                taken |= weights[j] == weights[i];
        } while (taken);
    }
}

static uint32_t hash_reference(const uint16_t *batch)
{
    uint32_t hash = 0;
    for (unsigned y = 0; y < BATCH; ++y, batch += PANEL_WIDTH) {
        int64_t sum = 0;
        for (unsigned x = 0; x < PANEL_WIDTH; ++x)
            sum += (int32_t)(int16_t)batch[x] * weights[x];
        hash = hash * 0x01000193u + (uint32_t)sum;
        hash = hash * 0x01000193u + ((uint32_t)((uint64_t)sum >> 32) & 255u);
    }
    return hash;
}

/* Never zero, which stands for "nothing sent yet". */
static uint64_t hash_batch(const uint16_t *batch)
{
#if HAVE_KERNELS
    if (kernels)
        return UINT64_C(1) << 63 | gp32_hash(batch, weights);
#endif
    return UINT64_C(1) << 63 | hash_reference(batch);
}

static void convert_palette(void)
{
#if HAVE_KERNELS
    if (kernels) {
        gp32_palette(colors, palette, constants);
        return;
    }
#endif
    for (unsigned i = 0; i < 256; ++i)
        colors[i] = rgb565(palette[i]);
}

#if HAVE_KERNELS
/* The assembly routines against the C ones before the machine uses them.  Two
   internal pages of the guest's SDRAM serve as scratch: boot clears them. */
static bool kernels_check(void)
{
    uint8_t *bytes = (uint8_t *)spare;
    for (unsigned i = 0; i < sizeof spare; ++i)
        bytes[i] = (uint8_t)(i * 167u + (i >> 8) * 13u);
    for (unsigned i = 0; i < 256; ++i)
        colors[i] = (uint16_t)(i * 40503u);
    for (unsigned turn = 0; turn < 4; ++turn) {
        line8_reference(strip[0], bytes, colors, turn);
        gp32_line8(strip[1], bytes, colors, turn);
        if (memcmp(strip[0], strip[1], PANEL_WIDTH * 2))
            return false;
    }
    for (unsigned lanes = 0; lanes <= 2; lanes += 2) {
        uint32_t header[GP32_LCD_HEADER] = {0};
        header[GP32_LCD_FORMAT] = 16u | lanes << 8;
        header[GP32_LCD_COUNT] = PANEL_WIDTH;
        convert(strip[0], bytes, header);
        gp32_line16(strip[1], (const uint16_t *)(const void *)bytes, lanes == 0, constants);
        if (memcmp(strip[0], strip[1], PANEL_WIDTH * 2))
            return false;
    }
    for (unsigned i = 0; i < BATCH * PANEL_WIDTH; ++i)
        strip[0][i] = (uint16_t)(i * 40503u + (i >> 5));
    for (unsigned round = 0; round < 2; ++round) {
        if (gp32_hash(strip[0], weights) != hash_reference(strip[0]))
            return false;
        for (unsigned i = 0; i < BATCH * PANEL_WIDTH; ++i) /* negative sums too */
            strip[0][i] = weights[i % PANEL_WIDTH] < 0 ? 0x7000 : 0x9000;
    }
    for (unsigned i = 0; i < 256; ++i)
        palette[i] = i * 0x9e3779b9u;
    gp32_palette(colors, palette, constants);
    for (unsigned i = 0; i < 256; ++i)
        if (colors[i] != rgb565(palette[i]))
            return false;
    if (FAST_PAGES >= 2) {
        for (unsigned i = 0; i < GP32_RAM_PAGE_SIZE; ++i)
            fast[0][i] = (uint8_t)(i * 29u + (i >> 7));
        gp32_copy64(fast[1], fast[0], GP32_RAM_PAGE_SIZE / 64);
        if (memcmp(fast[0], fast[1], GP32_RAM_PAGE_SIZE))
            return false;
    }
    return true;
}
#endif

static void line(uint16_t *out, unsigned x)
{
    uint32_t header[GP32_LCD_HEADER];
    const uint8_t *bytes = s3c2400_lcd_row(view, x, header, spare);
    unsigned format = header[GP32_LCD_FORMAT];
    unsigned bits = format & 255u, lanes = format >> 8 & 3u;
    if (bytes && header[GP32_LCD_COUNT] >= PANEL_WIDTH) {
        if (bits == 8 && !((uintptr_t)bytes & 3u)) {
            ++fast_lines;
#if HAVE_KERNELS
            if (kernels) {
                gp32_line8(out, bytes, colors, 3u ^ lanes);
                return;
            }
#endif
            line8_reference(out, bytes, colors, 3u ^ lanes);
            return;
        }
#if HAVE_KERNELS
        /* The two byte orders which keep a halfword's bytes together. */
        if (bits == 16 && !(lanes & 1u) && !((uintptr_t)bytes & 15u) && kernels) {
            ++fast_lines;
            gp32_line16(out, (const uint16_t *)(const void *)bytes, lanes == 0, constants);
            return;
        }
#endif
    }
    convert(out, bytes, header);
}

/* Runs on the display core, between m0_display_post and idle.  The picture is
   read from guest memory while the machine runs on: a program which draws into
   the shown buffer may tear.  Batches which did not change are not sent. */
static GP32_HOT void present(void)
{
    static unsigned current, renewed;
    uint32_t start = m0_micros();
    convert_palette();
    for (unsigned n = 0; n < BATCHES; ++n) {
        uint16_t *batch = strip[current];
        for (unsigned y = 0; y < BATCH; ++y)
            line(batch + y * PANEL_WIDTH, n * BATCH + y);
        uint64_t hash = hash_batch(batch);
        /* One batch per picture goes out even if its sum did not change: two
           pictures with the same sum, however unlikely, do not stay. */
        if (hash == shown_hash[n] && n != renewed)
            continue;
        shown_hash[n] = hash;
        m0_lcd_send_batch(batch, n * BATCH, BATCH);
        current ^= 1u;
    }
    renewed = renewed + 1 < BATCHES ? renewed + 1 : 0;
    last_us = m0_micros() - start;
    display_us += last_us;
}

/* Whether the frame the LCD just ended is to be drawn. */
static bool wanted(uint64_t frames, uint32_t since)
{
#if M0_GP32_AKA_FRAMESKIP >= 0
    (void)since;
    return frames > M0_GP32_AKA_FRAMESKIP;
#else
    uint32_t wait = 3 * last_us;
    if (wait < 16000)
        wait = 16000;
    if (wait > 100000)
        wait = 100000;
    return frames && since >= wait;
#endif
}

static void move_page(unsigned page, uint8_t *to)
{
    const uint8_t *from = GP32_RAM(soc->ram, page << GP32_RAM_PAGE_BITS);
#if HAVE_KERNELS
    if (kernels && !(((uintptr_t)to | (uintptr_t)from) & 15u))
        gp32_copy64(to, from, GP32_RAM_PAGE_SIZE / 64);
    else
#endif
        memcpy(to, from, GP32_RAM_PAGE_SIZE);
    gp32_ram_page[page] = (uintptr_t)to - ((uintptr_t)page << GP32_RAM_PAGE_BITS);
}

/* Brings the pages of the shown picture into internal memory.  Only between two
   runs of the machine and while the display core rests: nothing else holds an
   address into guest memory then.  With room for one picture only, the first
   buffer of a double buffered program stays and the other stays outside. */
static void place_video(void)
{
    static uint32_t tick;
    uint32_t offset, length;
    ++tick;
    if (!video_pages || !s3c2400_lcd_span(soc, &offset, &length))
        return;
    unsigned last = (offset + length - 1) >> GP32_RAM_PAGE_BITS;
    for (unsigned page = offset >> GP32_RAM_PAGE_BITS; page <= last; ++page) {
        bool fixed = false;
        for (unsigned i = 0; i < FAST_PAGES; ++i)
            fixed |= fast_list[i] == page;
        if (fixed)
            continue;
        int slot = -1;
        for (unsigned i = 0; i < video_pages; ++i)
            if (video[i].page == (int)page) {
                video[i].used = tick;
                fixed = true;
            } else if (video[i].page < 0) {
                slot = (int)i;
            } else if (tick - video[i].used > VIDEO_IDLE &&
                       (slot < 0 || (video[slot].page >= 0 && video[i].used < video[slot].used))) {
                slot = (int)i;
            }
        if (fixed || slot < 0)
            continue;
        if (video[slot].page >= 0)
            move_page((unsigned)video[slot].page, video[slot].home);
        video[slot].home = GP32_RAM(soc->ram, page << GP32_RAM_PAGE_BITS);
        video[slot].page = (int)page;
        video[slot].used = tick;
        move_page(page, video[slot].at);
    }
}

static void claim_video_pages(void)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    while (video_pages < VIDEO_PAGES &&
           heap_caps_get_free_size(internal) >= M0_GP32_AKA_RESERVE + GP32_RAM_PAGE_SIZE) {
        uint8_t *at = heap_caps_aligned_alloc(16, GP32_RAM_PAGE_SIZE, internal);
        if (!at)
            break;
        video[video_pages].at = at;
        video[video_pages++].page = -1;
    }
    esp_rom_printf("gp32: %u video pages inside, %u bytes of internal memory left\n",
                   video_pages, (unsigned)heap_caps_get_free_size(internal));
}

/* The GP32 buttons, from bit 0: left, down, right, up, L, B, A, R, start,
   select. */
static uint16_t buttons(void)
{
    uint8_t keys = esp32_buttons(), side = esp32_shoulders();
    return (uint16_t)((keys & KEY_LEFT ? 0x001 : 0) | (keys & KEY_DOWN ? 0x002 : 0) |
                      (keys & KEY_RIGHT ? 0x004 : 0) | (keys & KEY_UP ? 0x008 : 0) |
                      (keys & KEY_B ? 0x020 : 0) | (keys & KEY_A ? 0x040 : 0) |
                      (keys & KEY_MENU ? 0x100 : 0) | (keys & KEY_HOME ? 0x200 : 0) |
                      (side & M0_SHOULDER_L ? 0x010 : 0) | (side & M0_SHOULDER_R ? 0x080 : 0));
}

static void fatal(const char *text)
{
    esp_rom_printf("gp32: %s\n", text);
#ifdef M0_GP32_PAGED_CARD
    if (soc && soc->smc.image) /* what the program saved stays */
        gp32_aka_card_close();
#endif
    while (!m0_display_idle())
        delay(1);
    m0_lcd_flush();
    m0_lcd_fill(0xf800);
    for (;;)
        delay(1000);
}

int m0boy_console_main(void)
{
    m0_display_start(present);
    make_weights();
#if HAVE_KERNELS
    kernels = kernels_check();
    esp_rom_printf(kernels ? "gp32: assembly routines checked\n"
                           : "gp32: assembly routines DIFFER, C used\n");
#endif
    uint8_t *ram = map_ram();
    view = heap_caps_calloc(1, sizeof *view, MALLOC_CAP_SPIRAM);
    /* The processor and register state is the hottest data: internal RAM. */
    soc = heap_caps_calloc(1, sizeof *soc, MALLOC_CAP_INTERNAL);
    if (!soc) {
        esp_rom_printf("gp32: no room inside for the machine state, expect it to be slow\n");
        soc = heap_caps_calloc(1, sizeof *soc, MALLOC_CAP_SPIRAM);
    }
    if (!ram || !soc || !view)
        fatal("not enough memory");
    soc->ram = ram;
    const char *error = gp32_hle_boot(soc, gp32_payload, gp32_payload_size,
                                      (Arm920tConfig){0}, (S3c2400Hooks){0}, (S3c2400Video){0});
    if (error)
        fatal(error);
#ifdef GP32_PAYLOAD_DIRECTORY
    gp32_hle_set_directory(soc, GP32_PAYLOAD_DIRECTORY);
#endif
#ifdef M0_GP32_PAGED_CARD
#ifdef GP32_AKA_CARD_FOLDER
    size_t card_size = gp32_aka_card_open(GP32_AKA_CARD_FOLDER);
#else
    size_t card_size = gp32_aka_card_open(GP32_AKA_CARD_NAME83);
#endif
    if (!card_size || !gp32_smc_attach(&soc->smc, gp32_card_bytes(0, false), card_size, true))
        fatal("no SmartMedia card " GP32_AKA_CARD_FILE " on the SD card");
    esp_rom_printf("gp32: card " GP32_AKA_CARD_FILE ", %u bytes of PSRAM free\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
    soc->cycles_per_instruction = M0_GP32_AKA_CPI;
    soc->audio = (S3c2400Audio){sample, 0};
    esp32_audio_start(AUDIO_RATE);
    claim_video_pages();

    uint32_t shown = 0, report = m0_micros(), machine_us = 0;
    uint64_t seen = 0, steps = 0, frames = 0;
    unsigned drawn = 0;
    for (;;) {
        uint32_t before = m0_micros();
        S3c2400Stop stop = s3c2400_run(soc, 100000);
        machine_us += m0_micros() - before;
        if (stop == S3C2400_STOP_EXIT || soc->hle_exited)
            fatal("the program ended");
        if (stop == S3C2400_STOP_EXCEPTION) {
            char text[96];
            snprintf(text, sizeof text, "gp32: exception, pc %08x lr %08x swi %06x %06x\n",
                     (unsigned)soc->cpu.r[15], (unsigned)soc->cpu.r[14],
                     (unsigned)soc->hle_swi, (unsigned)soc->hle_unhandled);
            esp_rom_printf(text);
            fatal("unhandled exception");
        }
        uint32_t now = m0_micros();
        /* Never waits for the display: a busy one skips this frame. */
        if (soc->frames != seen && m0_display_idle() &&
            wanted(soc->frames - seen, now - shown)) {
            seen = soc->frames;
            shown = now;
            place_video();
            view->bus = soc->bus;
            memcpy(view->lcd, soc->lcd, sizeof view->lcd);
            memcpy(palette, soc->palette, sizeof palette);
            m0_display_post();
            ++drawn;
        }
        s3c2400_set_buttons(soc, buttons());
#ifdef M0_GP32_PAGED_CARD
        gp32_aka_card_service(now);
#endif
        m0_watchdog_feed();
        if (now - report >= 2000000) {
            /* Formatted first: some board emulators print the format only. */
            char text[224];
            snprintf(text, sizeof text, "gp32: %u kinst/2s, %u frames, %u drawn (%u fast lines), "
                     "machine %u ms, display %u ms, lcd %u bit at %07x, pc %08x, "
                     "internal %u (lowest %u)\n",
                     (unsigned)((soc->steps - steps) / 1000),
                     (unsigned)(soc->frames - frames), drawn, fast_lines,
                     (unsigned)(machine_us / 1000), (unsigned)(display_us / 1000),
                     1u << ((((soc->lcd[0] >> 1) & 15u) - 8u) & 7u),
                     (unsigned)(soc->lcd[5] << 1), (unsigned)soc->cpu.r[15],
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                              MALLOC_CAP_8BIT));
            esp_rom_printf(text);
            machine_us = display_us = fast_lines = 0;
            report = now;
            steps = soc->steps;
            frames = soc->frames;
            drawn = 0;
        }
    }
}
