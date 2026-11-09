#include "md_full.h"
#include "pc_frontend.h"
#include "m0_explore_host.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 224 };
typedef struct { unsigned frame, mask; } Event;
static int read_events(const char *path, Event **out, size_t *count) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    unsigned frame, previous = 0;
    int mask;
    while (fscanf(f, "%u %i", &frame, &mask) == 2) {
        if ((*count && frame <= previous) || mask < 0 || mask > 255) { fclose(f); return 0; }
        Event *p = realloc(*out, (*count + 1u) * sizeof **out);
        if (!p) { fclose(f); return 0; }
        *out = p; p[(*count)++] = (Event){frame, (unsigned)mask};
        previous = frame;
    }
    int okay = feof(f);
    fclose(f);
    return okay;
}
static int write_ppm(const char *path, const uint32_t *pixels) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        fputc((p >> 16) & 255u, f); fputc((p >> 8) & 255u, f); fputc(p & 255u, f);
    }
    return fclose(f) == 0;
}
static void wav_header(FILE *f, uint32_t frames) {
    const uint32_t bytes = frames * 4u;
    const uint8_t h[44] = {'R','I','F','F', (uint8_t)(bytes + 36u), (uint8_t)((bytes + 36u) >> 8),
        (uint8_t)((bytes + 36u) >> 16), (uint8_t)((bytes + 36u) >> 24), 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x44,0xac,0,0, 0x10,0xb1,2,0,
        4,0, 16,0, 'd','a','t','a', (uint8_t)bytes, (uint8_t)(bytes >> 8),
        (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24)};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}
static uint32_t image_hash(const uint32_t *pixels) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        for (unsigned b = 0; b < 3; ++b) { h = (h ^ (p & 255u)) * 16777619u; p >>= 8; }
    }
    return h;
}
/* The battery: --save FILE, else the cartridge's international name ($150)
 * as one word — a game keeping its progress in SRAM finds it next run. */
static void default_save_name(char *out, unsigned cap) {
    unsigned n = 0;
    for (unsigned i = 0; i < 48u && 0x150u + i < md_rom_size && n + 5u < cap; ++i) {
        const char c = (char)md_rom[0x150u + i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out[n++] = c;
    }
    if (!n) { out[n++] = 'm'; out[n++] = 'd'; }
    memcpy(out + n, ".sav", 5u);
}
static void load_sram(const char *path, MdFull *m) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    const size_t got = fread(m->sram, 1, sizeof m->sram, f);
    fclose(f);
    fprintf(stderr, "sram: %zu bytes from %s\n", got, path);
}
static int save_sram(const char *path, const MdFull *m) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    const int ok = fwrite(m->sram, 1, sizeof m->sram, f) == sizeof m->sram && fclose(f) == 0;
    if (!ok) fprintf(stderr, "%s: sram save failed\n", path);
    return ok;
}
static void ym_log(void *user, uint64_t sample, unsigned bank, uint8_t reg, uint8_t value) {
    fprintf((FILE *)user, "%llu %u %02x %02x\n", (unsigned long long)sample, bank, reg, value);
}
static int save_coverage(const char *path, const MdFull *m) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 0; }
    for (uint32_t pc = 0; pc < md_rom_size; pc += 2)
        if (m->coverage[pc >> 3] & (1u << (pc & 7u))) fprintf(f, "%06x\n", pc);
    return fclose(f) == 0;
}
/* --explore frames (m0_explore.h through m0_explore_host.h): the
 * coverage-guided player, from the start or from snapshots that reached
 * code never seen; needs --coverage, whose run is the interpreter's (it
 * holds the profile's counter), with no --input and no sound.  The
 * machine is one flat struct around a fixed allocation: a snapshot is
 * the struct itself, the harness's pointers (coverage, YM log, screen)
 * travelling with it. */
static m0xh_t xh;
static MdFull *g_machine;
#if defined(MD_PERF)
/* marks an MD_PERF build: each md_full_frame timed, every frame
 * rendered instead of only the last one; is enabled by the MDPERF_RENDER
 * environment variable */
static int bench_render;
static double bench_total, bench_max;
#endif
static void snapshot_save(void *ctx, void *state) {
    (void)ctx;
    memcpy(state, g_machine, sizeof *g_machine);
}
static void snapshot_load(void *ctx, const void *state) {
    (void)ctx;
    memcpy(g_machine, state, sizeof *g_machine);
}
static unsigned explore_keys(unsigned k) {
    unsigned keys = 0;
    if (k & M0X_UP) keys |= 1;
    if (k & M0X_DOWN) keys |= 2;
    if (k & M0X_LEFT) keys |= 4;
    if (k & M0X_RIGHT) keys |= 8;
    if (k & M0X_B1) keys |= 16;    /* A */
    if (k & M0X_B2) keys |= 32;    /* B */
    if (k & M0X_START) keys |= 128;
    return keys;
}
static uint32_t explore_locations(void) { return g_machine->coverage_locations; }
static void explore_play(unsigned count) {
    for (unsigned f = 0; !xh.stop && (xh.on || f < count); f++) {
        g_machine->pad = (uint8_t)explore_keys(m0xh_keys(&xh, explore_keys));
        g_machine->screen = NULL; /* the picture is not the explorer's business */
        md_full_frame(g_machine, 0);
        /* a discovery stops the frame run at this boundary: the machine
         * is checkpointed with the observation, the segment goes on */
        m0xh_seen(&xh, g_machine->coverage_locations);
    }
}
static int explore_again(void) { return !g_machine->fault; }
int main(int argc, char **argv) {
    unsigned limit = 0;
    int interp = 0;
    const char *ppm = NULL, *script = NULL, *coverage = NULL, *wav = NULL, *ym_path = NULL, *record_path = NULL;
    const char *save_arg = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--ym-log") && i + 1 < argc) ym_path = argv[++i];
        else if (!strcmp(argv[i], "--record-input") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_arg = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (m0xh_arg(&xh, argc, argv, &i)) { /* --explore, --random */ }
        else { fprintf(stderr, "usage: md_pc [--frames N] [--input SCRIPT] [--ppm FILE] [--wav FILE] [--ym-log FILE] [--coverage FILE] [--explore FRAMES --random SEED] [--record-input FILE] [--save FILE] [--interp]\n"); return 2; }
    }
    if (xh.total && !coverage) {
        fprintf(stderr, "--explore needs --coverage: the explorer's novelty is the trace (the run is the interpreter's)\n");
        return 2;
    }
    Event *events = NULL;
    size_t event_count = 0, event_at = 0;
#if defined(MD_PERF)
    bench_render = getenv("MDPERF_RENDER") ? 1 : 0;
#endif
    if (script && !read_events(script, &events, &event_count)) {
        fprintf(stderr, "invalid input script\n"); free(events); return 2;
    }
    MdFull *m = calloc(1, sizeof *m);
    uint32_t *pixels = calloc(W * H, sizeof *pixels);
    if (!m || !pixels) { free(m); free(pixels); free(events); return 1; }
    g_machine = m;
    md_full_reset(m);
    char save_name[64];
    if (save_arg) snprintf(save_name, sizeof save_name, "%s", save_arg);
    else default_save_name(save_name, sizeof save_name);
    load_sram(save_name, m);
    if (coverage) {
        m->coverage_bytes = (md_rom_size + 7u) / 8u;
        m->coverage = calloc(m->coverage_bytes, 1);
        if (!m->coverage) { free(m); free(pixels); free(events); return 1; }
    }
    FILE *ym_file = NULL;
    if (ym_path) {
        ym_file = fopen(ym_path, "w");
        if (!ym_file) { perror(ym_path); return 1; }
        m->ym_log = ym_log;
        m->ym_log_user = ym_file;
    }
    PcFrontend pc = {0};
    SDL_AudioDeviceID audio_device = 0;
    FILE *wav_file = NULL;
    uint32_t wav_samples = 0;
    if (wav) {
        wav_file = fopen(wav, "wb+");
        if (!wav_file) { perror(wav); return 1; }
        wav_header(wav_file, 0);
    }
    if (!limit && !xh.total) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
        }
        if (!pc_frontend_open(&pc, "m0boy Mega Drive", W, H)) return 1;
        SDL_AudioSpec wanted = {0};
        wanted.freq = 44100; wanted.format = AUDIO_S16SYS; wanted.channels = 2; wanted.samples = 1024;
        audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
        if (audio_device) SDL_PauseAudioDevice(audio_device, 0);
        else fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
    }
    unsigned scripted = 0;
    int running = 1;
    /* the live pad recorded "frame mask", the --input dialect itself, so a
     * played session replays as a profile session (only with a window) */
    FILE *record_file = NULL;
    unsigned record_last = ~0u;
    if (record_path) {
        if (limit)
            fprintf(stderr, "--record-input needs a window; ignoring it\n");
        else {
            record_file = fopen(record_path, "w");
            if (!record_file) { perror(record_path); return 1; }
        }
    }
    uint64_t deadline = SDL_GetTicks64();
    unsigned frame_fraction = 0;
    unsigned sram_flushed = 0;
    int okay = 1;
    if (xh.total) {
        /* the segments play outside the presentation loop: the
         * interpreter runs (the coverage's counter), no sound, no SRAM
         * flush until the end */
        const m0x_machine_t mm = {0, sizeof *m, snapshot_save, snapshot_load, 0};
        if (!m0xh_run(&xh, &mm, explore_locations, explore_play, explore_again))
            fprintf(stderr, "explore: out of memory for the first snapshot\n");
        if (m->sram_dirty) {
            if (save_sram(save_name, m)) m->sram_dirty = 0;
            else okay = 0;
            sram_flushed = m->frames;
        }
        running = 0; /* the presentation loop does not follow the explorer */
    }
    while (running && !m->fault && !xh.stop && (!limit || m->frames < limit)) {
        if (!limit) {
            pc_frontend_poll(&pc, 1);
            if (pc.quit) break;
        }
        while (event_at < event_count && events[event_at].frame == m->frames)
            scripted = events[event_at++].mask;
        const unsigned live = limit ? 0u : pc_frontend_buttons(&pc);
        if (record_file && (unsigned)(uint8_t)live != record_last) {
            record_last = (unsigned)(uint8_t)live;
            fprintf(record_file, "%u %u\n", m->frames, record_last);
        }
        m->pad = (uint8_t)(scripted | live);
        /* The VDP draws into pixels line by line; without a window only
         * the last frame is needed. */
        m->screen = !limit || m->frames + 1u >= limit ? pixels : NULL;
#if defined(MD_PERF)
        if (bench_render) m->screen = pixels;   /* bench what dry-run never pays */
#endif
        /* --coverage runs the interpreter: it holds the profile's
         * counter (native and interpreter agree on every hash) */
#if defined(MD_PERF)
        {
            const uint64_t t0 = SDL_GetPerformanceCounter();
            md_full_frame(m, !interp && !coverage);
            const double ms = 1000.0 * (double)(SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
            bench_total += ms;
            if (ms > bench_max) bench_max = ms;
        }
#else
        md_full_frame(m, !interp && !coverage);
#endif
        /* the battery back every two seconds of play, and once at the end */
        if (m->sram_dirty && m->frames - sram_flushed >= 120u) {
            if (save_sram(save_name, m)) m->sram_dirty = 0;
            sram_flushed = m->frames;
        }
        int16_t pcm[2 * 4096];
        const unsigned audio_frames = md_full_audio_read(m, pcm, 4096);
        if (wav_file) { fwrite(pcm, 2 * sizeof *pcm, audio_frames, wav_file); wav_samples += audio_frames; }
        /* Keep at most about 0.15 s queued so sound follows the picture. */
        if (audio_device && SDL_GetQueuedAudioSize(audio_device) < 44100u * 4u * 15u / 100u)
            SDL_QueueAudio(audio_device, pcm, audio_frames * 2u * sizeof *pcm);
        if (!limit && !m->fault) {
            if (!pc_frontend_present(&pc, pixels, W, H, W)) { running = 0; break; }
            deadline += 16u;
            frame_fraction += 2u;
            if (frame_fraction >= 3u) { ++deadline; frame_fraction -= 3u; }
            uint64_t now = SDL_GetTicks64();
            if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
            else if (now - deadline > 100u) { deadline = now; frame_fraction = 0; }
        }
    }
    okay = (!ppm || write_ppm(ppm, pixels)) && (!coverage || save_coverage(coverage, m));
    if (m->sram_dirty) { /* the battery once more: everything the game wrote */
        if (save_sram(save_name, m)) m->sram_dirty = 0;
        else okay = 0;
    }
    if (record_file && fclose(record_file)) okay = 0;
    if (wav_file) { wav_header(wav_file, wav_samples); if (fclose(wav_file)) okay = 0; }
    if (ym_file && fclose(ym_file)) okay = 0;
    printf("frames=%u pc=%06x fault=%u fault_pc=%06x opcode=%04x native_hits=%u fallback_hits=%u run_hash=%08x image_hash=%08x audio_hash=%08x sram_dirty=%u\n",
        m->frames, m->pc, m->fault, m->fault_pc, m->last_opcode,
        m->native_hits, m->fallback_hits, md_full_hash(m), image_hash(pixels), m->audio_hash,
        m->sram_dirty);
#if defined(MD_PERF)
    {
        const double n = m->frames ? (double)m->frames : 1.0;
        printf("bench_ms_avg=%.3f bench_ms_max=%.3f bench_render=%d p_z80=%.1f p_ym=%.1f p_psg=%.1f p_span=%.1f p_sat=%.1f p_sprite_px=%.1f instructions=%.1f\n",
            bench_total / n, bench_max, bench_render,
            (double)m->p_z80 / n, (double)m->p_ym / n, (double)m->p_psg / n,
            (double)m->p_span / n, (double)m->p_sat / n, (double)m->p_sprite_px / n,
            (double)m->instructions / n);
    }
#endif
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    pc_frontend_close(&pc);
    if (!limit) SDL_Quit();
    const int failed = !okay || m->fault;
    free(m->coverage); free(m); free(pixels); free(events);
    return failed ? 1 : 0;
}
