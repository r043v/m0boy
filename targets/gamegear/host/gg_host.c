/* Game Gear and Master System host harness (.sms files, or GG_SMS projects): coverage profiling (interpreter), native/interpreter
 * comparison (run_hash over CPU, memory, devices and the picture), captures
 * and sound. */
#include "gg.h"
#include "m0_host_util.h"
#include "m0_rom_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SG_GENERATED
#include "sg_config.h"
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#endif

static uint16_t image[GG_ACTIVE][GG_WIDTH]; /* Game Gear: the first 144 rows */
static uint16_t row_buffer[GG_ROW_STRIDE] __attribute__((aligned(4))), tmp_buffer[GG_ROW_STRIDE] __attribute__((aligned(4)));
static uint8_t cart_ram[GG_CART_RAM_SIZE];
static FILE *audio;
static uint32_t audio_samples;
static int audio_error;
static unsigned frames = 300, buttons, start_frame = ~0u;
static const char *save_state_file, *load_state_file;
static uint32_t *hits;
static uint8_t *entries;
static uint32_t *coverage_order;
#ifdef GG_ROM_TRACE
/* Profiling: frames in which each 512-byte ROM sector was read as data
 * (the generator keeps the most read pages in flash). */
static uint32_t *read_frames, *read_last, rom_sectors, trace_clock, new_sectors; /* trace_clock: frames run (the explorer rewinds machine.frames) */
static m0_rom_trace_t rom_trace; /* --rom-trace-out: every access, in order */
static const char *rom_trace_path, *rom_trace_skip_path;
/* --rom-trace-skip FILE: instructions a firmware compiled (the generator's
 * compiled.txt), by ROM offset: their fetches never reach its ROM cache. */
static uint8_t *rom_trace_skip, fetch_skipped;
void gg_rom_trace_insn(uint32_t offset) {
    fetch_skipped = rom_trace_skip && offset < rom_sectors * 512u && rom_trace_skip[offset];
}
void gg_rom_trace_fetch(uint32_t offset) {
    if (!fetch_skipped) m0_rom_trace_access(&rom_trace, offset, 1);
}
void gg_cart_trace(uint32_t offset, unsigned write) { m0_rom_trace_cart(&rom_trace, offset, write); }
static int rom_trace_skip_load(const char *path, uint32_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    rom_trace_skip = calloc(size, 1);
    char line[64];
    while (rom_trace_skip && fgets(line, sizeof line, f)) {
        unsigned loc;
        if (line[0] == '#' || sscanf(line, "%x", &loc) != 1) continue;
        const uint32_t pc = loc & 0xffff, offset = ((loc >> 16) << GG_BANK_SHIFT) + (pc & GG_BANK_MASK);
        if (pc < 0xc000 && offset < size) rom_trace_skip[offset] = 1;
    }
    fclose(f);
    return rom_trace_skip != 0;
}
void gg_rom_trace(uint32_t offset) {
    m0_rom_trace_access(&rom_trace, offset, 0);
    const uint32_t s = offset >> 9;
    if (!read_frames || s >= rom_sectors || read_last[s] == trace_clock + 1) return;
    read_last[s] = trace_clock + 1;
    if (!read_frames[s]++) new_sectors++;
}
#endif

/* --input / --random: parsing and RNG live in m0_host_util.h, these are the
 * harness's own knobs.  START joins the random pad on Game Gear only (bits:
 * up, down, left, right, 1, 2, START). */
static const char *input_script;
static m0_input_t input_file; /* --input file: bits as gg_buttons */
static const char *const input_names[] = { "up", "down", "left", "right", "1", "2", "start", "pause" };
static uint32_t random_state;
/* --save-state / --load-state: the machine's stream (gg_state_io) through a
 * file, the byte count first. */
static int load_state_file_done;
static void state_file_io(void *data, uint32_t bytes, void *ctx) {
    FILE *f = (FILE *)ctx;
    if (load_state_file_done) {
        if (fread(data, 1, bytes, f) != bytes) { fprintf(stderr, "state file short\\n"); exit(1); }
    } else if (fwrite(data, 1, bytes, f) != bytes) { fprintf(stderr, "state file write\\n"); exit(1); }
}
static int load_state_file_done;

static void state_file(const char *path, int load) {
    FILE *f = fopen(path, load ? "rb" : "wb");
    if (!f) { fprintf(stderr, "state file %s\\n", path); exit(1); }
    uint32_t n = gg_state_bytes();
    if (load) {
        uint32_t stored;
        if (fread(&stored, 4, 1, f) != 1 || stored != n) { fprintf(stderr, "state size mismatch\\n"); exit(1); }
    } else if (fwrite(&n, 4, 1, f) != 1) { fprintf(stderr, "state file write\\n"); exit(1); }
    load_state_file_done = load;
    gg_state_io(state_file_io, f, load);
    fclose(f);
    machine.stop = 0;
}


static void inputs(void) {
    unsigned keys = buttons | m0_scripted(input_script, machine.frames) | m0_input_keys(&input_file, machine.frames) |
                    m0_randomized(&random_state, machine.frames, GG_IS_SMS ? 0u : (unsigned)GG_START);
    if (machine.frames >= start_frame && machine.frames < start_frame + 10) keys |= GG_IS_SMS ? GG_B1 : GG_START;
    gg_buttons((uint8_t)keys);
}
#ifdef GG_ROM_TRACE
/* --explore frames: the coverage-guided player of m0_explore.h (the glue
 * in m0_explore_host.h), from the start or from snapshots of the machine
 * (and cartridge RAM) that reached code or ROM data never seen.
 * Segments plus longs que le défaut partagé : sur six jeux GG, 120 000 images
 * d'exploration et huit graines, 1920 + 3840 gagne 1 à 4 % de « locations »
 * en moyenne (2880 + 5760 ne fait pas mieux ; voir
 * docs/GG_RISTAR_VITESSE_2026-10-14.fr.md). */
#define M0X_SEGMENT_MIN 1920
#define M0X_SEGMENT_SPAN 3840
#include "m0_explore_host.h"
typedef struct { gg_t machine; uint8_t cart[GG_CART_RAM_SIZE]; } snapshot_t;
static m0xh_t xh;
static void snapshot_save(void *ctx, void *state) {
    snapshot_t *s = state;
    (void)ctx;
    memcpy(&s->machine, &machine, sizeof machine);
    memcpy(s->cart, cart_ram, sizeof cart_ram);
}
static void snapshot_load(void *ctx, const void *state) {
    const snapshot_t *s = state;
    (void)ctx;
    memcpy(&machine, &s->machine, sizeof machine);
    memcpy(cart_ram, s->cart, sizeof cart_ram);
    gg_vdp_restore();
}
static unsigned explore_map(unsigned k) {
    unsigned keys = 0;
    if (k & M0X_RIGHT) keys |= GG_RIGHT_KEY;
    if (k & M0X_LEFT) keys |= GG_LEFT_KEY;
    if (k & M0X_UP) keys |= GG_UP;
    if (k & M0X_DOWN) keys |= GG_DOWN;
    if (k & M0X_B1) keys |= GG_B1;
    if (k & M0X_B2) keys |= GG_B2;
    if (k & M0X_START) keys |= GG_START;
    return keys;
}
static uint32_t explore_novelty(void) { return gg_new_locations + new_sectors; }
static void explore_play(unsigned count) {
    (void)count; /* the segments bound themselves: m0x_keys ends them */
    machine.stop = 0;
    gg_run();
}
static int explore_again(void) { return !machine.cpu.fault; } /* a crashed game restarts from the pool */
static void explore(void) {
    const m0x_machine_t m = { 0, sizeof(snapshot_t), snapshot_save, snapshot_load, GG_IS_SMS ? M0X_NO_START : 0 };
    machine.row = 0; /* nothing is drawn: the game does not need it */
    m0xh_run(&xh, &m, explore_novelty, explore_play, explore_again);
    machine.frames = xh.x.frames; /* the snapshots rewound it */
}
#endif
static void frame(void *ctx) {
    (void)ctx;
#ifdef GG_ROM_TRACE
    trace_clock++;
    m0_rom_trace_frame(&rom_trace);
    if (xh.on) {
        gg_buttons((uint8_t)explore_map(m0xh_keys(&xh, explore_map)));
        /* a discovery stops the frame run at this boundary: the machine
         * is checkpointed with the observation, the segment goes on */
        m0xh_seen(&xh, explore_novelty());
        machine.stop = xh.stop;
        return;
    }
#endif
    if (audio) {
        uint8_t buffer[512];
        unsigned n = gg_audio_available();
        gg_audio_render(buffer, n);
        if (fwrite(buffer, 1, n, audio) != n) audio_error = 1;
        audio_samples += n;
    }
    if (machine.frames >= frames) machine.stop = 1;
    else inputs();
}
static void row(void *ctx, unsigned y, uint16_t *pixels) {
    (void)ctx;
    for (unsigned x = 0; x < GG_WIDTH; x++) /* true RGB565 whatever the panel order */
#if defined(GG_PANEL_SWAP) && GG_PANEL_SWAP
        image[y - GG_WIN_TOP][x] = (uint16_t)(pixels[x] >> 8 | pixels[x] << 8);
#else
        image[y - GG_WIN_TOP][x] = pixels[x];
#endif
}
static int hash_r = 1; /* R is compared only when the ROM can read it (m0_reads_refresh) */
#define M0_OBSERVED_R(s) ((uint8_t)(hash_r ? ((s)->cpu.r & 127) | (s)->cpu.r7 : 0))
static uint32_t run_hash(void) {
    gg_t *s = &machine;
    uint32_t h = M0_RUN_HASH_INIT;
    uint16_t registers[] = { s->cpu.bc, s->cpu.de, s->cpu.hl, s->cpu.bc2, s->cpu.de2, s->cpu.hl2, s->cpu.af2,
                             s->cpu.ix, s->cpu.iy, s->cpu.sp, s->cpu.pc };
    h = m0_hash_u16s(h, registers, sizeof registers / sizeof *registers);
    uint8_t flags[] = { s->cpu.a, s->cpu.f, s->cpu.i, M0_OBSERVED_R(s), s->cpu.im, s->cpu.iff1, s->cpu.iff2, s->cpu.halted,
                        s->cpu.ei_delay, s->vdp_status, s->vdp_latch, s->vdp_pending, s->vdp_buffer, s->vdp_code,
                        s->cram_latch, s->line_irq, s->lc_count, s->cpu.irq, s->cpu.nmi, s->cpu.fault,
                        s->page[0], s->page[1], s->page[2], s->ram_ctl };
    h = m0_hash_bytes(h, flags, sizeof flags);
    if (GG_KOREAN150) {
        h = m0_hash_bytes(h, s->page + 3, 3);
        h = m0_hash_bytes(h, &s->page2, 1);
    }
    uint32_t counters[] = { s->cyc, s->frames, s->render_line, s->vdp_address };
    h = m0_hash_bytes(h, counters, sizeof counters);
    h = m0_hash_bytes(h, s->ram, sizeof s->ram);
    h = m0_hash_bytes(h, s->vram, sizeof s->vram);
    h = m0_hash_bytes(h, s->cram, sizeof s->cram);
    h = m0_hash_bytes(h, s->vdp, sizeof s->vdp);
    h = m0_hash_bytes(h, s->psg_tone, sizeof s->psg_tone);
    h = m0_hash_bytes(h, s->psg_volume, sizeof s->psg_volume);
    h = m0_hash_bytes(h, &s->psg_noise, 1);
    h = m0_hash_bytes(h, &s->psg_latch, 1);
    if (s->cart_ram_used) {
#if GG_CART_ON_CARD
        /* Hash the logical cartridge RAM, regardless of its backing store.
         * Reading through the cache also includes dirty, unflushed bytes. */
        for (uint32_t i = 0; i < GG_CART_RAM_SIZE; ++i) {
            uint8_t byte = gg_cart_read(i);
            h = m0_hash_bytes(h, &byte, 1);
        }
#else
        if (s->cart_ram) h = m0_hash_bytes(h, s->cart_ram, GG_CART_RAM_SIZE);
#endif
    }
    return m0_hash_bytes(h, image, sizeof image);
}

static int sms __attribute__((unused));
int main(int argc, char **argv) {
    const char *rom_path = 0, *shot = 0, *cov_path = 0, *wav_path = 0, *ram_out_path = 0;
    int interp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) cov_path = argv[++i];
        else if (!strcmp(argv[i], "--ram-rom-out") && i + 1 < argc) ram_out_path = argv[++i];
#ifdef GG_ROM_TRACE
        else if (!strcmp(argv[i], "--rom-trace-out") && i + 1 < argc) rom_trace_path = argv[++i];
        else if (!strcmp(argv[i], "--rom-trace-skip") && i + 1 < argc) rom_trace_skip_path = argv[++i];
#endif
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) buttons = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--start-frame") && i + 1 < argc) start_frame = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (strchr(argv[++i], ':')) input_script = argv[i];
            else if (!m0_input_load(&input_file, argv[i], input_names, 8)) { perror(argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) {
            random_state = m0_random_seed(argv[++i]);
#ifdef GG_ROM_TRACE
            xh.seed = random_state; /* the explorer draws from the same seed */
#endif
        }
#ifdef GG_ROM_TRACE
        // axis 9: this host no longer discovers code on its own — deepeel
        // explores and the recompiler consumes its maps
        else if (!strcmp(argv[i], "--explore") && i + 1 < argc) {
            fprintf(stderr, "gg_host: --explore is retired: deepeel explores and the recompiler consumes its maps; this host records coverage by playing (--input, --coverage).\n");
            return 2;
        }
        else if (m0xh_arg(&xh, argc, argv, &i)) { /* --explore frames */ }
#endif
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) save_state_file = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state_file = argv[++i];
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else { fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]); return 1; }
    }
#ifdef GG_ROM_TRACE
    if (xh.total && !cov_path) {
        fprintf(stderr, "--explore needs --coverage: the explorer's novelty is the trace\n");
        return 2;
    }
#endif
    uint8_t *loaded = 0;
    const uint8_t *rom;
    size_t size;
    uint8_t *cart = cart_ram;
#ifdef SG_GENERATED
    rom = sg_rom; size = sg_rom_size;
    if (rom_path) { fprintf(stderr, "Generated harness already contains its cartridge\n"); return 1; }
    if (!SG_CART_RAM) cart = 0;
    if (!interp) machine.lookup = sg_gen_lookup;
#else
    (void)interp;
    if (!rom_path) { fprintf(stderr, "Usage: gg_host cartridge.gg|.sms [--frames N] [--shot file.ppm] [--coverage file] [--ram-rom-out file]\n"); return 1; }
    const char *dot = strrchr(rom_path, '.');
    sms = dot && (!strcmp(dot, ".sms") || !strcmp(dot, ".SMS"));
    FILE *file = fopen(rom_path, "rb");
    if (!file) { perror(rom_path); return 1; }
    loaded = m0_rom_load(file, 0x400000, 1, &size); /* whole pages, padded with FF */
    fclose(file);
    if (!loaded || size < 0x400) { free(loaded); fprintf(stderr, "Unsupported cartridge size\n"); return 1; }
    rom = loaded;
#endif
    FILE *coverage = 0;
#ifdef GG_MAPPER
    machine.mapper = GG_MAPPER;
#else
    machine.mapper = (uint8_t)m0_sega_mapper(rom, size);
#endif
    const uint32_t locations = (uint32_t)((size + GG_BANK_MASK) >> GG_BANK_SHIFT) << 16;
    if (cov_path) {
        coverage = fopen(cov_path, "w");
        hits = calloc(locations, sizeof *hits);
        entries = calloc(locations, 1);
        if (!coverage || !hits || !entries) { perror(cov_path); free(loaded); return 1; }
        machine.coverage = hits;
        machine.entries = entries;
        machine.coverage_size = locations;
#ifdef GG_ROM_TRACE
        rom_sectors = (uint32_t)((size + 511) / 512);
        read_frames = calloc(rom_sectors, sizeof *read_frames);
        read_last = calloc(rom_sectors, sizeof *read_last);
        if (rom_trace_path && !m0_rom_trace_open(&rom_trace, rom_trace_path, (uint32_t)size, GG_CART_RAM_SIZE)) {
            perror(rom_trace_path); free(loaded); return 1;
        }
        if (rom_trace_skip_path && !rom_trace_skip_load(rom_trace_skip_path, rom_sectors * 512u)) {
            perror(rom_trace_skip_path); free(loaded); return 1;
        }
#endif
    }
#ifdef GG_ROM_TRACE
    if (rom_trace_path && !cov_path) { fprintf(stderr, "--rom-trace-out needs --coverage\n"); free(loaded); return 2; }
#endif

#if defined(SG_GENERATED) && defined(SG_ROM_READS_R)
    hash_r = SG_ROM_READS_R;
#else
    hash_r = m0_reads_refresh(rom, size);
#endif
#ifdef SG_GENERATED
    gg_init(rom, size, cart);
#else
    gg_init_system(rom, size, cart, sms);
#endif
    machine.row = row;
    machine.row_out = row_buffer + GG_ROW_PAD;
    machine.row_tmp = tmp_buffer + GG_ROW_PAD;
    machine.frame = frame;
    if (wav_path) {
        audio = fopen(wav_path, "wb");
        if (!audio) { perror(wav_path); if (coverage) fclose(coverage); free(loaded); return 1; }
        m0_wav_header(audio, 0);
        gg_audio_rate(22050);
    }
    if (load_state_file) state_file(load_state_file, 1);
    inputs();
#ifdef GG_ROM_TRACE
    if (xh.total) explore();
    else
#endif
    if (frames) gg_run();
    if (save_state_file) state_file(save_state_file, 0);
    if (audio && (m0_wav_finish(audio, audio_samples) || audio_error)) {
        perror("audio"); if (coverage) fclose(coverage); free(loaded); return 1;
    }
    if (coverage) {
        /* v2: every address line carries its execution count, so the
         * recompiler ranks heat across sessions instead of file order */
        fprintf(coverage, GG_IS_SMS ? "# m0boy.sms.coverage.v2\n" : "# m0boy.gg.coverage.v2\n");
        fprintf(coverage, "# bank-shift %u\n", GG_BANK_SHIFT);
        if (machine.cart_ram_used) fprintf(coverage, "# cart-ram\n");
        fprintf(coverage, "# frames %u\n", machine.frames);
        for (uint32_t loc = 0; loc < locations; loc++) /* dynamic jump targets: region entries */
            if (entries[loc]) fprintf(coverage, "# entry %06x\n", loc);
#ifdef GG_ROM_TRACE
        for (uint32_t sec = 0; sec < rom_sectors; sec++) /* data reads: sector, frames */
            if (read_frames[sec]) fprintf(coverage, "# read %04x %u\n", sec, read_frames[sec]);
#endif
        coverage_order = malloc(sizeof *coverage_order * locations);
        if (!coverage_order || m0_coverage_dump(coverage, hits, coverage_order, locations, 6) < 0) {
            fclose(coverage); free(loaded); return 1;
        }
        free(coverage_order);
        if (fclose(coverage)) { perror("coverage"); free(loaded); return 1; }
#ifdef GG_ROM_TRACE
        if (rom_trace.f) {
            fprintf(stderr, "rom_trace: %llu accesses, %llu written, %llu frames\n", (unsigned long long)rom_trace.events,
                    (unsigned long long)rom_trace.written, (unsigned long long)rom_trace.frames);
            if (m0_rom_trace_close(&rom_trace)) { perror(rom_trace_path); free(loaded); return 1; }
        }
#endif
    }
    if (shot) {
        FILE *f = fopen(shot, "wb");
        if (!f) { perror(shot); free(loaded); return 1; }
        fprintf(f, "P6\n%d %u\n255\n", GG_WIDTH, GG_WIN_LINES);
        for (unsigned y = 0; y < GG_WIN_LINES; y++)
            for (unsigned x = 0; x < GG_WIDTH; x++) {
                const unsigned c = image[y][x];
                const unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
                uint8_t p[] = { (uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2) };
                if (fwrite(p, 1, 3, f) != 3) { fclose(f); free(loaded); return 1; }
            }
        if (fclose(f)) { free(loaded); return 1; }
    }
    if (ram_out_path) { /* the session's C000-DFFF image: m0recomp --ram-image compiles it */
        FILE *rf = fopen(ram_out_path, "wb");
        if (!rf || fwrite(machine.ram, 1, sizeof machine.ram, rf) != sizeof machine.ram || fclose(rf)) {
            perror(ram_out_path); free(loaded); return 1;
        }
    }
    printf("frames=%u cycles=%u pc=%04x page=%u,%u,%u compiled=%u interpreted=%u run_hash=%08x\n",
           machine.frames, machine.cyc, machine.cpu.pc, machine.page[0], machine.page[1], machine.page[2],
           machine.compiled, machine.interpreted, run_hash());
    free(loaded);
    return machine.cpu.fault ? 1 : 0;
}
