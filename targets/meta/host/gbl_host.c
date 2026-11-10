/*
 * Headless host harness for the gbl runtime.
 *
 * It runs the same runtime and generated code as the Meta firmware, so it is
 * the place to record coverage, compare compiled execution against the
 * interpreter, and capture frames.
 *
 *   gbl_host [--frames N] [--interp] [--input FILE] [--coverage FILE]
 *            [--dump-frame N FILE.ppm]... [--hash-every K] [--stack-kib N | --stack-bytes N]
 *            [--wav FILE.wav] [--mute MASK] [--wram-hash-bytes N] [--dump-wram FILE]
 *            [--static-skip-check] [--max-seconds S] [--save FILE]
 *            [--trace FRAME COUNT (profile build)] [--agent]
 *            [--explore FRAMES [--random SEED] (profile build)]
 *
 * Input files contain "<frame> <buttons>" lines; buttons are the gbl mask
 * (hex or names joined with '+': a b select start right left up down).
 * --frames 0 runs until interrupted.  --max-seconds ends the run cleanly
 * (coverage and reports written) after S seconds of wall-clock time.
 * --save FILE keeps cartridge RAM in FILE (512-byte sectors, like the
 * META's .SAV file); without it the RAM starts erased and is not kept.
 *
 * Built with GBL_SDL (the <name>_play target) the harness opens a window:
 * arrows, X = A, Z = B, Enter = Start, Backspace/Right Shift = Select,
 * Escape quits and writes --coverage; --record-input saves the session in
 * the same input format for exact replays.
 *
 * --agent hands the buttons to another program (tools/ai-play): at a VBlank
 * it prints "agent <frame> <executed addresses> <frame hash> <IO> <OAM>
 * <VRAM>" (hex) on stdout and reads one line from stdin, "<buttons>:<frames>"
 * steps (e.g. "right+a:12 right:8"), queued like --input lines; it asks
 * again once they are played.  "quit" or end of input ends the run.  The
 * program keeps the steps as an input file, which replays the same game.
 *
 * --explore FRAMES plays that many frames with the coverage-guided player
 * of m0_explore.h (seed --random): segments from the start or from save
 * states that reached code or ROM data never seen.  Coverage and the ROM
 * read profile are those of every frame played; there is no input file.
 */
#include "gbl.h"
#include "gbl_config.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double max_seconds;
static struct timespec run_start;

static double elapsed_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - run_start.tv_sec) + (t.tv_nsec - run_start.tv_nsec) * 1e-9;
}

static uint8_t frame_buf[144][160];
#ifdef GBL_SDL
static uint32_t max_frames = 0; /* play until the window closes */
#else
static uint32_t max_frames = 600;
#endif
static uint32_t hash_every;
static uint32_t wram_hash_bytes = 0x2000;
static const char *wram_dump_path;
static int static_check;           /* --static-skip-check */
static uint8_t static_skip_next;
static uint8_t displayed[144][160];
static uint32_t static_skipped, static_stale, static_stale_run, static_stale_max;
static uint64_t run_hash = 1469598103934665603ull;
/* Machine state at every VBlank entry: CPU registers, cycle counter, WRAM,
 * HRAM and IO.  Equal between two builds means identical timing too. */
static uint64_t state_hash = 1469598103934665603ull;

static void state_mix(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { state_hash ^= b[i]; state_hash *= 1099511628211ull; }
}

#define MAX_DUMPS 1024
static struct { uint32_t frame; const char *path; } dumps[MAX_DUMPS];
static int dump_count;

#define MAX_INPUTS 262144 /* les parties automatiques d'aujourd'hui : 3,2 M images, une ligne sur 20 */
static struct { uint32_t frame; uint8_t buttons; } inputs[MAX_INPUTS];
static int input_count, input_pos;

static const char *coverage_path;
static uint32_t coverage_count; /* distinct executed addresses (--agent) */
static int agent;
static uint32_t agent_next;
static FILE *wav;
static uint32_t wav_bytes, wav_frac;
#define WAV_RATE 22050u

static void wav_header(void) {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                     16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 8, 0,
                     'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t v[4] = {36 + wav_bytes, WAV_RATE, WAV_RATE, wav_bytes};
    int at[4] = {4, 24, 28, 40};
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++) h[at[i] + b] = (uint8_t)(v[i] >> (8 * b));
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), wav);
    fseek(wav, 0, SEEK_END);
}

static void wav_frame(void) {
    /* One DMG frame of samples: 70224 cycles at 4194304 Hz. */
    uint8_t buf[512];
    wav_frac += WAV_RATE * 70224u;
    unsigned n = wav_frac / 4194304u;
    wav_frac %= 4194304u;
    gbl_apu_render(buf, n);
    fwrite(buf, 1, n, wav);
    wav_bytes += n;
}
#ifdef GBL_COVERAGE
static uint8_t coverage[GBL_ROM_BANKS][0x4000];
static uint32_t hits[GBL_ROM_BANKS][0x4000];

void gbl_coverage_hit(uint16_t bank, uint16_t addr, int entry) {
    if (bank >= GBL_ROM_BANKS) return;
    if (!coverage[bank][addr & 0x3FFF]) coverage_count++;
    coverage[bank][addr & 0x3FFF] |= (uint8_t)(1 | (entry ? 2 : 0));
    if (hits[bank][addr & 0x3FFF] != 0xFFFFFFFFu) hits[bank][addr & 0x3FFF]++;
}

static void write_coverage(void) {
    if (!coverage_path) return;
    FILE *f = fopen(coverage_path, "w");
    if (!f) { perror(coverage_path); return; }
    fprintf(f, "# gbl coverage v1: bank addr flags(1=exec,2=entry) executions\n");
    for (int b = 0; b < GBL_ROM_BANKS; b++)
        for (int a = 0; a < 0x4000; a++)
            if (coverage[b][a])
                fprintf(f, "%02x %04x %x %x\n", b, b ? a + 0x4000 : a, coverage[b][a], hits[b][a]);
    fclose(f);
}
#else
static void write_coverage(void) {
    if (coverage_path) fprintf(stderr, "coverage requested but build lacks GBL_COVERAGE\n");
}
#endif

#ifdef GBL_COVERAGE
#include "m0_explore.h"
static m0x_t explorer;
static int explore_on, explore_pending;
static uint32_t explore_frames, explore_seed, explore_new_pages;
#endif

#ifdef GBL_TRACE
static long trace_left = -1;
static uint32_t trace_from_frame;
void gbl_trace_hook(uint16_t pc) {
    if (trace_left < 0 || gbl_frame < trace_from_frame) return;
    if (trace_left-- == 0) exit(0);
    printf("%u:%04x a%02x bc%04x de%04x hl%04x sp%04x z%u c%u ime%u ly%u lcdc%02x cyc%u\n",
           pc < 0x4000 ? 0 : gbr.rom_bank, pc, gbr.a, gbr.bc, gbr.de, gbr.hl, gbr.sp,
           gbr.zr == 0, gbr.cf, gbr.ime, gbl_io[0x44], gbl_io[0x40], gbr.cyc);
}
#endif

static void write_ppm(const char *path) {
    static const uint8_t shade[4] = {0xE0, 0xA8, 0x60, 0x18};
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n160 144\n255\n");
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) {
            uint8_t s = shade[frame_buf[y][x] & 3];
            uint8_t px[3] = {s, s, s};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

static uint64_t frame_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) { h ^= frame_buf[y][x]; h *= 1099511628211ull; }
    return h;
}

static uint16_t line_buf[160] __attribute__((aligned(4)));

/* The host renders every line, but checks the line-signature contract: a
 * line whose signature equals its previous one must have equal pixels. */
static uint32_t line_sig[144];
static uint8_t line_sig_valid[144], line_claimed_same[144];
static uint32_t lines_same, lines_same_wrong, lines_checked;

int gbl_platform_line_check(uint8_t ly, uint32_t sig) {
    if (ly >= 144) return 1;
    lines_checked++;
    line_claimed_same[ly] = line_sig_valid[ly] && line_sig[ly] == sig;
    line_sig[ly] = sig;
    line_sig_valid[ly] = 1;
    return 1;
}

uint16_t *gbl_platform_line_begin(uint8_t ly) {
    (void)ly;
    return line_buf;
}

void gbl_platform_line_end(uint8_t ly) {
    /* gbl_lcd_palette stays at its identity default, so pixels are shades. */
    if (ly < 144) {
        if (line_claimed_same[ly]) {
            lines_same++;
            for (int x = 0; x < 160; x++)
                if (frame_buf[ly][x] != (uint8_t)line_buf[x]) { lines_same_wrong++; break; }
            line_claimed_same[ly] = 0;
        }
        for (int x = 0; x < 160; x++) frame_buf[ly][x] = (uint8_t)line_buf[x];
    }
}

#ifdef GBL_ROM_TRACE
/* ---- SD streaming study (built with -DGBL_ROM_TRACE) ---------------------
 *
 * Every read from the switchable banks lands here.  4 KiB chunks marked
 * resident (the hottest ones of a previous run, --rom-resident FILE KIB)
 * would stay in flash; other reads go through an LRU cache of 512-byte SD
 * sectors (--rom-cache-sectors N).  Misses per frame are what an SD reader
 * would have to fetch.  --rom-profile-out FILE writes read counts per
 * 256-byte page for choosing the resident set. */
#define ROM_TRACE_CHUNKS (GBL_ROM_BANKS * 4)
static uint64_t trace_page_reads[GBL_ROM_BANKS * 64];
static uint8_t trace_resident[ROM_TRACE_CHUNKS];
static const char *trace_profile_out;
static unsigned trace_cache_n = 8, trace_resident_kib;
static int32_t trace_tag[256];
static uint32_t trace_age[256], trace_clock;
static uint64_t trace_reads, trace_cold_reads, trace_misses, trace_seq_misses;
static int32_t trace_last_miss = -2;
static uint32_t trace_frame_misses, *trace_hist, trace_hist_n, trace_hist_cap;
/* --rom-trace-out FILE: every banked read in order, the format the Game
 * Gear's harness writes (tools/profile/m0romsim; bank 0 stays in flash) */
#include "m0_rom_trace.h"
static m0_rom_trace_t rom_trace;
static const char *rom_trace_path;
void gbl_eram_trace(uint32_t off, unsigned write) { m0_rom_trace_cart(&rom_trace, off, write); }

#if GBL_ROM_STREAM
extern const uint8_t gbl_rom_full[];
#define TRACE_ROM gbl_rom_full
#else
#define TRACE_ROM gbl_rom
#endif
extern uint8_t gbl_rom_fetch;

uint8_t gbl_rom_trace_read(uint32_t bank, uint16_t a) {
    uint32_t abs = bank * 0x4000u + (uint32_t)(a - 0x4000u);
    trace_reads++;
    m0_rom_trace_access(&rom_trace, abs, gbl_rom_fetch ? 1u : 0u);
    /* The profile counts data reads: compiled code does not fetch opcodes
     * (the generator adds the fetches of code it leaves interpreted). */
    if (!gbl_rom_fetch && !trace_page_reads[abs >> 8]++) {
#ifdef GBL_COVERAGE
        explore_new_pages++;
#endif
    }
    if (!trace_resident[abs >> 12]) {
        int32_t sec = (int32_t)(abs >> 9);
        unsigned lru = 0;
        trace_cold_reads++;
        trace_clock++;
        for (unsigned i = 0; i < trace_cache_n; i++) {
            if (trace_tag[i] == sec) { trace_age[i] = trace_clock; goto hit; }
            if (trace_age[i] < trace_age[lru]) lru = i;
        }
        trace_tag[lru] = sec;
        trace_age[lru] = trace_clock;
        trace_misses++;
        trace_frame_misses++;
        if (sec == trace_last_miss + 1) trace_seq_misses++;
        trace_last_miss = sec;
    }
hit:
    return TRACE_ROM[abs];
}

static void trace_frame(void) {
    m0_rom_trace_frame(&rom_trace);
    if (trace_hist_n == trace_hist_cap) {
        trace_hist_cap = trace_hist_cap ? trace_hist_cap * 2 : 4096;
        trace_hist = realloc(trace_hist, trace_hist_cap * sizeof(*trace_hist));
    }
    trace_hist[trace_hist_n++] = trace_frame_misses;
    trace_frame_misses = 0;
}

static int trace_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* Resident set: the hottest 4 KiB chunks of a profile, up to kib KiB. */
static void trace_load_resident(const char *path, unsigned kib) {
    static uint64_t chunk[ROM_TRACE_CHUNKS];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    unsigned page;
    unsigned long long n;
    while (fscanf(f, "%x %llu", &page, &n) == 2)
        if (page / 16 < ROM_TRACE_CHUNKS) chunk[page / 16] += n;
    fclose(f);
    for (unsigned k = 0; k < kib / 4; k++) {
        int best = -1;
        for (int c = 0; c < ROM_TRACE_CHUNKS; c++)
            if (!trace_resident[c] && chunk[c] && (best < 0 || chunk[c] > chunk[best])) best = c;
        if (best < 0) break;
        trace_resident[best] = 1;
    }
}

static void trace_report(void) {
    unsigned touched = 0, resident = 0;
    for (int c = 0; c < ROM_TRACE_CHUNKS; c++) {
        uint64_t n = 0;
        for (int p = 0; p < 16; p++) n += trace_page_reads[c * 16 + p];
        touched += n != 0;
        resident += trace_resident[c];
    }
    if (trace_profile_out) {
        FILE *f = fopen(trace_profile_out, "w");
        if (!f) { perror(trace_profile_out); exit(2); }
        for (unsigned p = 0; p < GBL_ROM_BANKS * 64u; p++)
            if (trace_page_reads[p]) fprintf(f, "%x %llu\n", p, (unsigned long long)trace_page_reads[p]);
        fclose(f);
    }
    uint32_t *h = malloc((trace_hist_n + 1) * sizeof(*h));
    memcpy(h, trace_hist, trace_hist_n * sizeof(*h));
    qsort(h, trace_hist_n, sizeof(*h), trace_cmp);
    uint32_t n = trace_hist_n ? trace_hist_n : 1, over10 = 0, over30 = 0;
    for (uint32_t i = 0; i < trace_hist_n; i++) { over10 += h[i] > 10; over30 += h[i] > 30; }
    printf("rom_stream banked_reads %llu cold_reads %llu chunks_touched %u (%u KiB) resident %u KiB cache %u sectors\n",
           (unsigned long long)trace_reads, (unsigned long long)trace_cold_reads, touched, touched * 4,
           resident * 4, trace_cache_n);
    printf("rom_stream misses %llu (%.1f%% sequential) per_frame mean %.2f p50 %u p95 %u p99 %u max %u "
           "frames>10 %.2f%% frames>30 %.2f%%\n",
           (unsigned long long)trace_misses,
           trace_misses ? 100.0 * trace_seq_misses / trace_misses : 0.0,
           (double)trace_misses / n, h[n / 2], h[n * 95 / 100], h[n * 99 / 100],
           trace_hist_n ? h[trace_hist_n - 1] : 0, 100.0 * over10 / n, 100.0 * over30 / n);
    free(h);
}
#endif

#if GBL_SAVE_BYTES > 0
/* Cartridge RAM file (--save FILE), or kept in memory for the run.  What
 * was never written reads as the .SAV compile.sh puts on the card (0xFF),
 * a missing file too: a failed read would also fail the write-back of a
 * block smaller than a sector (its sector is read first), the game's
 * writes would be lost, and profiles recorded on another path than the
 * console's (Super Mario Land 2's demo: a quarter of the code it runs). */
static const char *save_path;
static uint8_t save_mem[GBL_SAVE_BYTES];
static uint8_t save_mem_valid[(GBL_SAVE_BYTES + 511) / 512];
static uint32_t save_writes;

int gbl_platform_save_read(uint32_t sector, uint8_t *dst) {
    if (sector * 512u >= GBL_SAVE_BYTES) return 0;
    if (save_path) {
        FILE *f = fopen(save_path, "rb");
        size_t n = f && !fseek(f, (long)sector * 512L, SEEK_SET) ? fread(dst, 1, 512, f) : 0;
        if (f) fclose(f);
        if (n < 512) memset(dst + n, 0xFF, 512 - n); /* past the file's end: a fresh card */
        return 1;
    }
    if (!save_mem_valid[sector]) {
        memset(dst, 0xFF, 512);
        return 1;
    }
    memcpy(dst, save_mem + sector * 512u, 512);
    return 1;
}

int gbl_platform_save_write(uint32_t sector, const uint8_t *src) {
    if (sector * 512u >= GBL_SAVE_BYTES) return 0;
    save_writes++;
    if (save_path) {
        FILE *f = fopen(save_path, "r+b");
        if (!f) f = fopen(save_path, "w+b");
        if (!f) { perror(save_path); return 0; }
        int ok = !fseek(f, (long)sector * 512L, SEEK_SET) && fwrite(src, 1, 512, f) == 512;
        fclose(f);
        return ok;
    }
    memcpy(save_mem + sector * 512u, src, 512);
    save_mem_valid[sector] = 1;
    return 1;
}
#endif

#if GBL_ROM_STREAM
/* SD sectors read per frame (histogram capped at 63). */
static uint32_t stream_hist[64], stream_frames, stream_prev, stream_max;
static uint32_t stream_sd_reads;

static void stream_frame(void) {
    uint32_t d = gbl_stream_misses - stream_prev;
    stream_prev = gbl_stream_misses;
    stream_hist[d > 63 ? 63 : d]++;
    if (d > stream_max) stream_max = d;
    stream_frames++;
}

/* Streaming project on the host: the whole ROM is linked in. */
extern const uint8_t gbl_rom_full[];
int gbl_platform_rom_read(uint32_t sector, uint8_t *dst) {
    stream_sd_reads++;
    uint32_t off = sector * 512u;
    for (uint32_t i = 0; i < 512u; i++) dst[i] = off + i < gbl_rom_size ? gbl_rom_full[off + i] : 0xFF;
    return 1;
}
#endif

static void finish(void) {
    uint32_t f = gbl_frame;
#if GBL_DEBUG_RESTART
    fprintf(stderr, "irq check: %u entries, %u RETI checked, %u wrong, %u unmatched\n",
            (unsigned)gbl_dbg_irq.entries, (unsigned)gbl_dbg_irq.checked, (unsigned)gbl_dbg_irq.bad,
            (unsigned)gbl_dbg_irq.unmatched);
#endif
#ifdef GBL_COVERAGE
    if (explore_on) {
        f = explorer.frames;
        fprintf(stderr, "explore: %u frames, %u segments, %u snapshots, %u locations\n", explorer.frames,
                explorer.segments, explorer.pool_size, coverage_count);
    }
#endif
#if GBL_SAVE_BYTES > 0 || GBL_RTC
    gbl_save_flush(); /* like the META returning to the loader */
#endif
    write_coverage();
#ifdef GBL_ROM_TRACE
    trace_report();
    if (m0_rom_trace_close(&rom_trace)) perror(rom_trace_path);
#endif
    if (wav) { wav_header(); fclose(wav); }
    printf("frames %u run_hash %016llx interp_instrs %u\n", f,
           (unsigned long long)run_hash, gbl_interp_instrs);
    uint64_t wh = 1469598103934665603ull;
    for (uint32_t i = 0; i < wram_hash_bytes && i < 0x2000; i++) { wh ^= gbl_wram[i]; wh *= 1099511628211ull; }
    printf("wram_hash %016llx\n", (unsigned long long)wh);
    printf("state_hash %016llx\n", (unsigned long long)state_hash);
#if GBL_SAVE_BYTES > 0
    printf("save sectors_written %u\n", save_writes);
#endif
#if GBL_ROM_STREAM
    {
        uint32_t n = stream_frames ? stream_frames : 1, over10 = 0, over30 = 0;
        for (uint32_t k = 0; k < 64; k++) {
            if (k > 10) over10 += stream_hist[k];
            if (k > 30) over30 += stream_hist[k];
        }
        uint32_t acc = 0, p95 = 64, p99 = 64;
        for (uint32_t k = 0; k < 64; k++) {
            acc += stream_hist[k];
            if (p95 == 64 && acc >= n * 95u / 100u) p95 = k;
            if (p99 == 64 && acc >= n * 99u / 100u) p99 = k;
        }
        printf("rom_stream sectors_read %u per_frame mean %.3f p95 %u p99 %u max %u frames>10 %u frames>30 %u\n",
               gbl_stream_misses, (double)gbl_stream_misses / n, p95, p99, stream_max, over10, over30);
        printf("rom_sources sd_reads %u", stream_sd_reads);
#if GBL_ROM_PACKED
        printf(" flash_unpacked %u", gbl_stream_unpacked);
#endif
        printf("\n");
    }
#endif
    printf("line_skip unchanged %u of %u wrong %u\n", lines_same, lines_checked, lines_same_wrong);
    if (static_check)
        printf("static_skip skipped %u stale %u stale_run_max %u\n",
               static_skipped, static_stale, static_stale_max);
    if (wram_dump_path) {
        FILE *df = fopen(wram_dump_path, "wb");
        if (df) { fwrite(gbl_wram, 1, sizeof(gbl_wram), df); fclose(df); }
    }
    printf("pc %04x sp %04x bank %u ime %u ie %02x if %02x lcdc %02x stat %02x ly %u\n",
           gbr.pc, gbr.sp, gbr.rom_bank, gbr.ime, gbl_ie, gbl_io[0x0F], gbl_io[0x40],
           gbl_io[0x41], gbl_io[0x44]);
    fflush(stdout);
    exit(0);
}

#ifdef GBL_SDL
#include <SDL.h>

static SDL_Window *sdl_win;
static SDL_Renderer *sdl_ren;
static SDL_Texture *sdl_tex;
static SDL_AudioDeviceID sdl_audio;
static uint32_t sdl_audio_frac;
static uint64_t sdl_next;
static FILE *record;
static uint8_t record_last = 0xFF;
static const char *record_path;

static void sdl_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        exit(2);
    }
    sdl_win = SDL_CreateWindow("gbl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               160 * 4, 144 * 4, SDL_WINDOW_RESIZABLE);
    sdl_ren = SDL_CreateRenderer(sdl_win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(sdl_ren, 160, 144);
    sdl_tex = SDL_CreateTexture(sdl_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 160, 144);
    SDL_AudioSpec want = {0}, have;
    want.freq = WAV_RATE;
    want.format = AUDIO_U8;
    want.channels = 1;
    want.samples = 512;
    sdl_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sdl_audio) SDL_PauseAudioDevice(sdl_audio, 0);
    sdl_next = SDL_GetPerformanceCounter();
    if (record_path) {
        record = fopen(record_path, "w");
        if (!record) { perror(record_path); exit(2); }
        fprintf(record, "# gbl input recording: <frame> <buttons>\n");
    }
}

static void record_buttons(uint8_t b) {
    static const char *names[8] = {"a", "b", "select", "start", "right", "left", "up", "down"};
    if (!record || b == record_last) return;
    record_last = b;
    fprintf(record, "%u ", gbl_frame);
    if (!b) fprintf(record, "none");
    for (int i = 0, first = 1; i < 8; i++)
        if (b & (1u << i)) { fprintf(record, "%s%s", first ? "" : "+", names[i]); first = 0; }
    fprintf(record, "\n");
}

static void sdl_frame(void) {
    static const uint32_t shade[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};
    static uint32_t pix[144 * 160];
    for (int y = 0; y < 144; y++)
        for (int x = 0; x < 160; x++) pix[y * 160 + x] = shade[frame_buf[y][x] & 3];
    SDL_UpdateTexture(sdl_tex, NULL, pix, 160 * 4);
    SDL_RenderClear(sdl_ren);
    SDL_RenderCopy(sdl_ren, sdl_tex, NULL, NULL);
    SDL_RenderPresent(sdl_ren);

    if (sdl_audio) {
        uint8_t buf[512];
        sdl_audio_frac += WAV_RATE * 70224u;
        unsigned n = sdl_audio_frac / 4194304u;
        sdl_audio_frac %= 4194304u;
        gbl_apu_render(buf, n);
        SDL_QueueAudio(sdl_audio, buf, n);
    }

    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
            if (record) fclose(record);
            finish();
        }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_X]) b |= 0x01;
    if (k[SDL_SCANCODE_Z]) b |= 0x02;
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= 0x04;
    if (k[SDL_SCANCODE_RETURN]) b |= 0x08;
    if (k[SDL_SCANCODE_RIGHT]) b |= 0x10;
    if (k[SDL_SCANCODE_LEFT]) b |= 0x20;
    if (k[SDL_SCANCODE_UP]) b |= 0x40;
    if (k[SDL_SCANCODE_DOWN]) b |= 0x80;
    if (input_count == 0) {
        record_buttons(b);
        gbl_set_buttons(b);
    }

    /* 59.73 Hz; audio queue depth keeps the two clocks from drifting apart. */
    uint64_t freq = SDL_GetPerformanceFrequency();
    sdl_next += freq * 70224u / 4194304u;
    uint64_t now = SDL_GetPerformanceCounter();
    if (sdl_audio && SDL_GetQueuedAudioSize(sdl_audio) > 4 * 400) sdl_next += freq / 1000u;
    if (now < sdl_next) SDL_Delay((Uint32)((sdl_next - now) * 1000u / freq));
    else if (now - sdl_next > freq / 4) sdl_next = now;
}
#endif

/* --state-test N M: save a state just after frame N, play M frames, load
 * it, play the same M frames again; both runs must end in the same state
 * (compare in --interp or --meta-precise builds: the fast build may take an
 * interrupt a few instructions apart after the restart). */
static uint32_t st_frame, st_len, st_end;
static int st_input_pos;
static int st_phase; /* 0 off, 1 save requested, 2 saved, 3 load requested, 4 loaded */
static uint8_t *st_buf;
static uint32_t st_size, st_pos;
static uint64_t st_hash1;
static jmp_buf st_restart;

static void st_write(void *data, uint32_t n, void *ctx) {
    (void)ctx;
    st_buf = realloc(st_buf, st_pos + n);
    memcpy(st_buf + st_pos, data, n);
    st_pos += n;
}
static void st_read(void *data, uint32_t n, void *ctx) {
    (void)ctx;
    memcpy(data, st_buf + st_pos, n);
    st_pos += n;
}
static void st_hash_fn(void *data, uint32_t n, void *ctx) {
    uint64_t *h = ctx;
    const uint8_t *b = data;
    for (uint32_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 1099511628211ull; }
}
static uint64_t st_machine_hash(void) {
    uint64_t h = 1469598103934665603ull;
    gbl_state_io(st_hash_fn, &h, 0);
    return h;
}

#ifdef GBL_COVERAGE
/* --explore: save states through gbl_state_io, taken and loaded at safe
 * points (instruction boundaries); a load starts gbl_run over. */
static uint32_t explore_cursor;
static void explore_put(void *data, uint32_t n, void *ctx) {
    memcpy((uint8_t *)ctx + explore_cursor, data, n);
    explore_cursor += n;
}
static void explore_get(void *data, uint32_t n, void *ctx) {
    memcpy(data, (uint8_t *)ctx + explore_cursor, n);
    explore_cursor += n;
}
static void explore_save(void *ctx, void *state) {
    (void)ctx;
    explore_cursor = 0;
    gbl_state_io(explore_put, state, 0);
}
static void explore_load(void *ctx, const void *state) {
    (void)ctx;
    explore_cursor = 0;
    gbl_state_io(explore_get, (void *)state, 1);
}
static void explore_begin(void) {
    const m0x_machine_t m = { 0, gbl_state_bytes(), explore_save, explore_load, 0 };
    max_frames = 0;
    explore_on = m0x_begin(&explorer, &m, explore_seed, explore_frames);
    if (!explore_on) { fprintf(stderr, "explore: out of memory\n"); exit(2); }
}
/* Once per frame: the player's keys, or the end of the segment. */
static void explore_vblank(void) {
    if (max_seconds > 0 && (explorer.frames & 15) == 0 && elapsed_seconds() >= max_seconds) finish();
    if (explore_pending) return; /* the safe point comes before the next frame */
    const unsigned k = m0x_keys(&explorer);
    if (k == M0X_END) {
        explore_pending = 1;
        gbl_safe_request = 1;
        return;
    }
    if (coverage_count + explore_new_pages > explorer.novelty) gbl_safe_request = 1;
    gbl_set_buttons((uint8_t)((k & M0X_B1 ? 0x01 : 0) | (k & M0X_B2 ? 0x02 : 0) | (k & M0X_START ? 0x08 : 0) |
                              (k & M0X_RIGHT ? 0x10 : 0) | (k & M0X_LEFT ? 0x20 : 0) |
                              (k & M0X_UP ? 0x40 : 0) | (k & M0X_DOWN ? 0x80 : 0)));
}
#endif

void gbl_platform_safe_point(void) {
#ifdef GBL_COVERAGE
    if (explore_on) m0x_observe(&explorer, coverage_count + explore_new_pages);
    if (explore_pending) {
        explore_pending = 0;
        if (!m0x_next(&explorer, coverage_count + explore_new_pages)) finish();
        longjmp(st_restart, 1);
    }
#endif
    if (st_phase == 1) {
        st_pos = 0;
        gbl_state_io(st_write, 0, 0);
        st_size = st_pos;
        st_input_pos = input_pos;
        st_phase = 2;
        printf("state_test saved %u bytes at frame %u pc %04x\n", st_size, gbl_frame, gbr.pc);
    } else if (st_phase == 3) {
        st_pos = 0;
        gbl_state_io(st_read, 0, 1);
        input_pos = st_input_pos;
        st_phase = 4;
        longjmp(st_restart, 1);
    }
}

static uint8_t parse_buttons(const char *s);

static void agent_hex(const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    putchar(' ');
    for (size_t i = 0; i < n; i++) { putchar(digits[p[i] >> 4]); putchar(digits[p[i] & 15]); }
}

/* --agent: one question per batch of steps; the steps start at this VBlank. */
static void agent_step(uint32_t f, uint64_t h) {
    printf("agent %u %u %016llx", f, coverage_count, (unsigned long long)h);
    agent_hex(gbl_io, sizeof(gbl_io));
    agent_hex(gbl_oam, sizeof(gbl_oam));
    agent_hex(gbl_vram, sizeof(gbl_vram));
    putchar('\n');
    fflush(stdout);
    static char line[4096];
    if (!fgets(line, sizeof(line), stdin) || !strncmp(line, "quit", 4)) finish();
    uint32_t at = f;
    for (char *save, *tok = strtok_r(line, " \t\r\n", &save); tok; tok = strtok_r(0, " \t\r\n", &save)) {
        char *colon = strchr(tok, ':');
        unsigned n = colon ? (unsigned)atoi(colon + 1) : 1;
        if (colon) *colon = 0;
        if (input_count == MAX_INPUTS) finish();
        inputs[input_count].frame = at;
        inputs[input_count].buttons = parse_buttons(tok);
        input_count++;
        at += n ? n : 1;
    }
    agent_next = at > f ? at : f + 1;
}

void gbl_platform_vblank(void) {
    uint32_t f = gbl_frame;
    if (st_len && st_phase == 0 && f == st_frame) {
        st_phase = 1;
        gbl_safe_request = 1;
    } else if (st_phase == 2 && f == st_end) {
        st_hash1 = st_machine_hash();
        st_phase = 3;
        gbl_safe_request = 1;
    } else if (st_phase == 4 && f == st_end) {
        uint64_t h2 = st_machine_hash();
        printf("state_test %s %016llx %016llx\n", h2 == st_hash1 ? "ok" : "FAIL",
               (unsigned long long)st_hash1, (unsigned long long)h2);
        fflush(stdout);
        exit(h2 == st_hash1 ? 0 : 1);
    }
#ifdef GBL_ROM_TRACE
    trace_frame();
#endif
#if GBL_ROM_STREAM
    stream_frame();
#endif
#ifdef GBL_COVERAGE
    if (explore_on) { explore_vblank(); return; }
#endif
    uint64_t h = frame_hash();
    run_hash ^= h;
    run_hash *= 1099511628211ull;
    {
        uint16_t regs[6] = {gbr.bc, gbr.de, gbr.hl, gbr.sp, gbr.pc,
                            (uint16_t)(gbr.a | gbl_getf() << 8)};
        uint32_t cyc = gbr.cyc;
        state_mix(regs, sizeof(regs));
        state_mix(&cyc, sizeof(cyc));
        state_mix(gbl_wram, sizeof(gbl_wram));
        state_mix(gbl_hram, sizeof(gbl_hram));
        state_mix(gbl_io, sizeof(gbl_io));
    }
    if (hash_every && f % hash_every == 0)
        printf("frame %u hash %016llx state %016llx\n", f, (unsigned long long)h,
               (unsigned long long)state_hash);
    for (int i = 0; i < dump_count; i++)
        if (dumps[i].frame == f) write_ppm(dumps[i].path);
    if (wav) wav_frame();
    if (static_check) {
        /* The host renders every frame; replay the META policy on top and
         * check that a skipped frame never differs from the one on screen
         * for more than a single frame. */
        int shown = !static_skip_next;
        if (shown) memcpy(displayed, frame_buf, sizeof(displayed));
        else static_skipped++;
        if (memcmp(displayed, frame_buf, sizeof(displayed))) {
            static_stale++;
            if (++static_stale_run > static_stale_max) static_stale_max = static_stale_run;
        } else {
            static_stale_run = 0;
        }
        static_skip_next = (uint8_t)gbl_frame_unchanged_next(shown);
    }
    if (agent && f >= agent_next) agent_step(f, h);
    while (input_pos < input_count && inputs[input_pos].frame <= f)
        gbl_set_buttons(inputs[input_pos++].buttons);
#ifdef GBL_SDL
    sdl_frame();
#endif
    if (max_frames && f >= max_frames) finish();
    if (max_seconds > 0 && (f & 15) == 0 && elapsed_seconds() >= max_seconds) finish();
}

static uint8_t parse_buttons(const char *s) {
    if (s[0] == '0' && s[1] == 'x') return (uint8_t)strtoul(s, 0, 16);
    static const char *names[8] = {"a", "b", "select", "start", "right", "left", "up", "down"};
    uint8_t m = 0;
    char buf[128];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(0, "+")) {
        if (!strcmp(tok, "none")) continue;
        int found = 0;
        for (int i = 0; i < 8; i++)
            if (!strcmp(tok, names[i])) { m |= (uint8_t)(1u << i); found = 1; }
        if (!found) { fprintf(stderr, "unknown button '%s'\n", tok); exit(2); }
    }
    return m;
}

static void load_inputs(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[256];
    while (fgets(line, sizeof(line), f) && input_count < MAX_INPUTS) {
        char btn[128];
        unsigned fr;
        if (line[0] == '#' || sscanf(line, "%u %127s", &fr, btn) != 2) continue;
        inputs[input_count].frame = fr;
        inputs[input_count].buttons = parse_buttons(btn);
        input_count++;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    unsigned stack_bytes = 256u * 1024u;
    clock_gettime(CLOCK_MONOTONIC, &run_start);
#ifdef GBL_ROM_TRACE
    for (int i = 0; i < 256; i++) trace_tag[i] = -1;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-test") && i + 2 < argc) {
            st_frame = (uint32_t)atoi(argv[++i]);
            st_len = (uint32_t)atoi(argv[++i]);
            st_end = st_frame + st_len;
            max_frames = 0;
        }
        else if (!strcmp(argv[i], "--max-seconds") && i + 1 < argc) max_seconds = atof(argv[++i]);
#if GBL_ERAM_SIZE > 0
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_path = argv[++i];
#else
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) i++; /* no cartridge RAM */
#endif
#ifdef GBL_ROM_TRACE
        else if (!strcmp(argv[i], "--rom-profile-out") && i + 1 < argc) trace_profile_out = argv[++i];
        else if (!strcmp(argv[i], "--rom-trace-out") && i + 1 < argc) {
            rom_trace_path = argv[++i];
            if (!m0_rom_trace_open(&rom_trace, rom_trace_path, gbl_rom_size, GBL_ERAM_SIZE)) { perror(rom_trace_path); return 2; }
        }
#else
        else if (!strcmp(argv[i], "--rom-profile-out") && i + 1 < argc) i++; /* nothing to record */
#endif
#ifdef GBL_ROM_TRACE
        else if (!strcmp(argv[i], "--rom-cache-sectors") && i + 1 < argc) {
            trace_cache_n = (unsigned)atoi(argv[++i]);
            if (trace_cache_n < 1 || trace_cache_n > 256) { fprintf(stderr, "1..256 sectors\n"); exit(2); }
        }
        else if (!strcmp(argv[i], "--rom-resident") && i + 2 < argc) {
            const char *path = argv[++i];
            trace_resident_kib = (unsigned)atoi(argv[++i]);
            trace_load_resident(path, trace_resident_kib);
        }
#endif
        else if (!strcmp(argv[i], "--interp")) gbl_interp_only = 1;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) load_inputs(argv[++i]);
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage_path = argv[++i];
        else if (!strcmp(argv[i], "--wram-hash-bytes") && i + 1 < argc) wram_hash_bytes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--static-skip-check")) static_check = 1;
        else if (!strcmp(argv[i], "--dump-wram") && i + 1 < argc) wram_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--mute") && i + 1 < argc) gbl_apu_mute = (uint8_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav = fopen(argv[++i], "wb");
            if (!wav) { perror(argv[i]); return 2; }
            wav_header();
        }
#ifdef GBL_SDL
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) record_path = argv[++i];
#endif
        else if (!strcmp(argv[i], "--agent")) agent = 1;
#ifdef GBL_COVERAGE
        // axis 9: the host no longer discovers code on its own — deepeel's
        // maps drive the compilation (--coverage game.cov / game.map.png);
        // the coverage-guided player of m0_explore stays in targets/common
        // for the cores that still explore, the meta core refuses it
        else if (!strcmp(argv[i], "--explore") && i + 1 < argc) {
            fprintf(stderr, "gbl_host: --explore is retired: deepeel explores "
                            "(deepeel explore ROM --play --cov game.cov), this host "
                            "consumes its maps (--coverage). Playing and recording "
                            "coverage (--coverage with --input or at the window) stays.\n");
            exit(2);
        }
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) explore_seed = (uint32_t)strtoul(argv[++i], 0, 0);
#endif
        else if (!strcmp(argv[i], "--hash-every") && i + 1 < argc) hash_every = (uint32_t)atoi(argv[++i]);
#ifdef GBL_TRACE
        else if (!strcmp(argv[i], "--trace") && i + 2 < argc) {
            trace_from_frame = (uint32_t)atoi(argv[++i]);
            trace_left = atol(argv[++i]);
        }
#endif
        else if (!strcmp(argv[i], "--stack-kib") && i + 1 < argc) stack_bytes = (unsigned)atoi(argv[++i]) * 1024u;
        else if (!strcmp(argv[i], "--stack-bytes") && i + 1 < argc) stack_bytes = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-frame") && i + 2 < argc && dump_count < MAX_DUMPS) {
            dumps[dump_count].frame = (uint32_t)atoi(argv[++i]);
            dumps[dump_count].path = argv[++i];
            dump_count++;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
#ifdef GBL_SDL
    sdl_init();
#endif
    char base;
    gbl_stack_limit = (uintptr_t)&base - (uintptr_t)stack_bytes;
    gbl_reset();
    gbl_apu_set_rate(WAV_RATE);
#ifdef GBL_COVERAGE
    if (explore_frames) explore_begin();
#endif
    setjmp(st_restart); /* a loaded state starts over from gbr.pc */
    gbl_run();
    return 0;
}
