/* SG-1000 host harness: coverage profiling (interpreter), native/interpreter
 * comparison (run_hash) and the device renderer checked against the
 * reference one on every displayed line. */
#include "sg1000.h"
#include "m0_host_util.h"
#if SG_MSX
#include "m0_msx_layout.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SG_GENERATED
#include "sg_config.h"
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#endif

static uint8_t image[SG_HEIGHT][SG_WIDTH];
static uint16_t out_row[SG_OUT_WIDTH];
static unsigned out_mismatch;
static uint8_t cart_ram[SG_CART_RAM_SIZE];
static FILE *audio;
static uint32_t audio_samples;
static int audio_error;
static unsigned frames = 300, buttons, start_frame = ~0u, pause_frame = ~0u;
#if SG_MSX
/* banked cartridges: locations are page << 16 | pc, allocated once the ROM is known */
#ifdef SG_MSX_TRACE
extern uint32_t *msx_trace_count, *msx_trace_last;
#endif
static uint16_t peek_at[64]; /* --peek ADDR: bytes of the address space printed after the run */
static unsigned peeks;
static uint32_t *hits, *coverage_order;
static uint8_t *entries;
#else
static uint32_t hits[65536];
static uint8_t entries[65536];
static uint32_t coverage_order[65536];
#endif

/* --input / --random: parsing and RNG live in m0_host_util.h, these are the
 * harness's own knobs */
static const char *input_script;
static m0_input_t input_file; /* --input file: bits as sg1000_buttons, 64 = pause */
static const char *const input_names[] = { "up", "down", "left", "right", "1", "2", "pause" };
static uint32_t random_state;
static void inputs(void) {
    const unsigned played = m0_input_keys(&input_file, machine.frames);
    unsigned keys = buttons | m0_scripted(input_script, machine.frames) | (played & 63) |
                    m0_randomized(&random_state, machine.frames, 0);
    if (machine.frames >= start_frame && machine.frames < start_frame + 10) keys |= 16;
    sg1000_buttons(0, (uint8_t)keys);
    sg1000_pause(machine.frames == pause_frame || (played & 64));
}
#ifndef SG_GENERATED
/* --explore frames: the coverage-guided player of m0_explore.h (the glue
 * in m0_explore_host.h), from the start or from snapshots of the machine
 * (and cartridge RAM) that reached code never seen.  No START: the
 * console's pause button is not on the pad.  The novelty is read at the
 * segment's ends only: the machine struct must stay byte-identical for
 * the console firmware, so it carries no live discovery count, and a
 * segment plays whole instead of stopping on a discovery. */
#include "m0_explore_host.h"
typedef struct { sg1000_t machine; uint8_t cart[SG_CART_RAM_SIZE]; } snapshot_t;
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
    machine.sprites_dirty = 1; /* sprite masks derived from the tables */
}
static uint32_t explore_novelty(void) { /* program counters reached so far */
    uint32_t n = 0;
#if SG_MSX
    for (uint32_t loc = 0; loc < machine.coverage_size; loc++) n += hits[loc] != 0;
#else
    for (unsigned pc = 0; pc < 65536; pc++) n += hits[pc] != 0;
#endif
    return n;
}
static void explore_play(unsigned count) {
    (void)count; /* the segments bound themselves: m0x_keys ends them */
    machine.stop = 0;
    sg1000_run();
}
static void explore(void) {
    const m0x_machine_t m = { 0, sizeof(snapshot_t), snapshot_save, snapshot_load, M0X_NO_START };
    machine.row = 0; /* nothing is drawn: the game does not need it */
    machine.ref_row = 0;
    m0xh_run(&xh, &m, explore_novelty, explore_play, 0);
    machine.frames = xh.x.frames; /* the snapshots rewound it */
}
static unsigned explore_keys(unsigned k) {
    return (unsigned)(k & M0X_UP ? 1 : 0) | (k & M0X_DOWN ? 2 : 0) | (k & M0X_LEFT ? 4 : 0) |
           (k & M0X_RIGHT ? 8 : 0) | (k & M0X_B1 ? 16 : 0) | (k & M0X_B2 ? 32 : 0);
}
#endif
static void frame(void *ctx) {
    (void)ctx;
#ifndef SG_GENERATED
    if (xh.on) {
        sg1000_buttons(0, (uint8_t)explore_keys(m0xh_keys(&xh, explore_keys)));
        machine.stop = xh.stop; /* M0X_END: the segment is spent */
        return;
    }
#endif
    if (audio) {
        uint8_t buffer[512];
        unsigned n = sg1000_audio_available();
        sg1000_audio_render(buffer, n);
        if (fwrite(buffer, 1, n, audio) != n) audio_error = 1;
        audio_samples += n;
    }
    if (machine.frames >= frames) machine.stop = 1;
    else inputs();
}
static void row(void *ctx, unsigned y, uint16_t *pixels) { (void)ctx; (void)y; (void)pixels; /* already in out_row */ }
static void ref_row(void *ctx, unsigned y, const uint8_t *pixels) {
    (void)ctx;
    memcpy(image[y], pixels, SG_WIDTH);
    uint16_t expected[SG_OUT_WIDTH];
    sg1000_filter_line(pixels, expected);
    if (memcmp(out_row, expected, sizeof expected)) out_mismatch++;
}
static int hash_r = 1; /* R is compared only when the ROM can read it (m0_reads_refresh) */
#define M0_OBSERVED_R(s) ((uint8_t)(hash_r ? ((s)->cpu.r & 127) | (s)->cpu.r7 : 0))
static uint32_t run_hash(void) {
    sg1000_t *s = &machine;
    uint32_t h = M0_RUN_HASH_INIT;
    uint16_t registers[] = { s->cpu.bc, s->cpu.de, s->cpu.hl, s->cpu.bc2, s->cpu.de2, s->cpu.hl2, s->cpu.af2,
                             s->cpu.ix, s->cpu.iy, s->cpu.sp, s->cpu.pc };
    h = m0_hash_u16s(h, registers, sizeof registers / sizeof *registers);
    uint8_t flags[] = { s->cpu.a, s->cpu.f, s->cpu.i, M0_OBSERVED_R(s), s->cpu.im, s->cpu.iff1, s->cpu.iff2, s->cpu.halted,
                        s->cpu.ei_delay, s->vdp_status, s->vdp_latch, s->vdp_pending, s->vdp_buffer, s->cpu.irq,
                        s->cpu.nmi, s->cpu.fault, s->pause };
    h = m0_hash_bytes(h, flags, sizeof flags);
    uint32_t counters[] = { s->cyc, s->frames, s->render_line, s->vdp_address };
    h = m0_hash_bytes(h, counters, sizeof counters);
    h = m0_hash_bytes(h, s->ram, sizeof s->ram);
    h = m0_hash_bytes(h, s->vram, sizeof s->vram);
    h = m0_hash_bytes(h, s->vdp, sizeof s->vdp);
    h = m0_hash_bytes(h, s->buttons, sizeof s->buttons);
    h = m0_hash_bytes(h, s->psg_tone, sizeof s->psg_tone);
    h = m0_hash_bytes(h, s->psg_volume, sizeof s->psg_volume);
    h = m0_hash_bytes(h, &s->psg_noise, 1);
    h = m0_hash_bytes(h, &s->psg_latch, 1);
#if SG_MSX
    h = m0_hash_bytes(h, s->ay_reg, sizeof s->ay_reg);
    h = m0_hash_bytes(h, &s->ay_latch, 1);
    h = m0_hash_bytes(h, &s->ppi_a, 1);
    h = m0_hash_bytes(h, &s->ppi_c, 1);
#endif
    /* cartridge RAM: the generic harness always maps it; generated projects
     * only when the profile saw it written */
#ifdef SG_GENERATED
    if (SG_CART_RAM) h = m0_hash_bytes(h, s->cart_ram, SG_CART_RAM_SIZE);
#else
    if (s->cart_ram_used) h = m0_hash_bytes(h, s->cart_ram, SG_CART_RAM_SIZE);
#endif
    return m0_hash_bytes(h, image, sizeof image);
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *shot = 0, *cov_path = 0, *wav_path = 0, *vram_path = 0;
    int interp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc) vram_path = argv[++i]; /* the 16 KiB of VRAM at the end, to compare with another emulator */
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) cov_path = argv[++i];
#if SG_MSX
        else if (!strcmp(argv[i], "--peek") && i + 1 < argc && peeks < 64) peek_at[peeks++] = (uint16_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--mapper") && i + 1 < argc) {
            int found = -1;
            for (int m = 0; m < 5; m++) if (!strcmp(argv[i + 1], m0_msx_mapper_name((unsigned)m))) found = m;
            if (found < 0) { fprintf(stderr, "Unknown mapper %s (plain, ascii8, ascii16, konami, konami-scc)\n", argv[i + 1]); return 1; }
            msx_set_mapper(found);
            i++;
        }
#endif
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) buttons = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--start-frame") && i + 1 < argc) start_frame = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--pause-frame") && i + 1 < argc) pause_frame = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (strchr(argv[++i], ':')) input_script = argv[i];
            else if (!m0_input_load(&input_file, argv[i], input_names, 7)) { perror(argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) {
            random_state = m0_random_seed(argv[++i]);
#ifndef SG_GENERATED
            xh.seed = random_state; /* the explorer draws from the same seed */
#endif
        }
#ifndef SG_GENERATED
        // axis 9: this host no longer discovers code on its own — deepeel
        // explores and the recompiler consumes its maps; the coverage-guided
        // player stays in targets/common for the cores that still explore
        else if (!strcmp(argv[i], "--explore") && i + 1 < argc) {
            fprintf(stderr, "sg_host: --explore is retired: deepeel explores "
                            "(deepeel z80 explore sg ROM ...), this host records "
                            "coverage by playing (--input, --coverage).\n");
            return 2;
        }
        else if (m0xh_arg(&xh, argc, argv, &i)) { /* other shared options */ }
#endif
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else { fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]); return 1; }
    }
#ifndef SG_GENERATED
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
    if (!rom_path) { fprintf(stderr, "Usage: sg1000_host cartridge.sg [--frames N] [--shot file.ppm] [--coverage file]\n"); return 1; }
    FILE *file = fopen(rom_path, "rb");
    if (!file) { perror(rom_path); return 1; }
#if SG_MSX
    loaded = m0_rom_load(file, M0_MSX_MAX_ROM, 1, &size); /* padded with FF to whole pages */
    if (loaded && !m0_msx_layout(loaded, size).valid) { fprintf(stderr, "Not a MSX ROM (\"AB\" header, INIT in the cartridge)\n"); free(loaded); return 1; }
#else
    loaded = m0_rom_load(file, 0xc000, 0, &size); /* unbanked: no page padding */
#endif
    fclose(file);
    if (!loaded) { fprintf(stderr, "Unsupported cartridge size\n"); return 1; }
    rom = loaded;
#endif
    FILE *coverage = 0;
    if (cov_path) {
        coverage = fopen(cov_path, "w");
        if (!coverage) { perror(cov_path); free(loaded); return 1; }
    }
#if defined(SG_GENERATED) && defined(SG_ROM_READS_R)
    hash_r = SG_ROM_READS_R;
#else
    hash_r = m0_reads_refresh(rom, size);
#endif
    sg1000_init(rom, size, cart);
    machine.row = row;
    machine.row_out = out_row;
    machine.ref_row = ref_row;
    machine.frame = frame;
#if SG_MSX
#ifdef SG_MSX_TRACE
    if (coverage) { /* ROM sectors read, per frame (the planner's weights) */
        msx_trace_count = calloc(size / 512 + 1, sizeof *msx_trace_count);
        msx_trace_last = calloc(size / 512 + 1, sizeof *msx_trace_last);
    }
#endif
    if (coverage) {
        machine.coverage_size = machine.msx_mapper ? (uint32_t)machine.rom_units << 16 : 65536u;
        hits = calloc(machine.coverage_size, sizeof *hits);
        entries = calloc(machine.coverage_size, 1);
        coverage_order = malloc(machine.coverage_size * sizeof *coverage_order);
        if (!hits || !entries || !coverage_order) { perror("coverage"); free(loaded); return 1; }
    }
#endif
    if (coverage) { machine.coverage = hits; machine.entries = entries; }
    if (wav_path) {
        audio = fopen(wav_path, "wb");
        if (!audio) { perror(wav_path); if (coverage) fclose(coverage); free(loaded); return 1; }
        m0_wav_header(audio, 0);
        sg1000_audio_rate(22050);
    }
    inputs();
#ifndef SG_GENERATED
    if (xh.total) explore();
    else
#endif
    if (frames) sg1000_run();
    if (audio && (m0_wav_finish(audio, audio_samples) || audio_error)) {
        perror("audio"); if (coverage) fclose(coverage); free(loaded); return 1;
    }
    if (coverage) {
        /* v2: every address line carries its execution count, so the
         * recompiler ranks heat across sessions instead of file order */
#if SG_MSX
        const unsigned width = machine.msx_mapper ? 6 : 4;
        fprintf(coverage, "# m0boy.msx.coverage.v2\n");
        if (machine.msx_mapper) fprintf(coverage, "# bank-shift 13\n# mapper %u\n", machine.msx_mapper);
        fprintf(coverage, "# frames %u\n", machine.frames);
#ifdef SG_MSX_TRACE
        for (uint32_t sec = 0; sec < size / 512; sec++) /* ROM sectors read by the program: sector, frames */
            if (msx_trace_count[sec]) fprintf(coverage, "# read %04x %u\n", sec, msx_trace_count[sec]);
#endif
        for (uint32_t loc = 0; loc < machine.coverage_size; loc++) /* dynamic jump targets: region entries */
            if (entries[loc]) fprintf(coverage, "# entry %0*x\n", (int)width, loc);
        if (m0_coverage_dump(coverage, hits, coverage_order, machine.coverage_size, width) < 0) { fclose(coverage); free(loaded); return 1; }
#else
        fprintf(coverage, "# m0boy.sg1000.coverage.v2\n");
        if (machine.cart_ram_used) fprintf(coverage, "# cart-ram\n");
        for (unsigned pc = 0; pc < 65536; pc++) /* dynamic jump targets: region entries */
            if (entries[pc]) fprintf(coverage, "# entry %04x\n", pc);
        if (m0_coverage_dump(coverage, hits, coverage_order, 65536, 4) < 0) { fclose(coverage); free(loaded); return 1; }
#endif
        if (fclose(coverage)) { perror("coverage"); free(loaded); return 1; }
    }
    if (shot) {
        FILE *f = fopen(shot, "wb");
        if (!f) { perror(shot); free(loaded); return 1; }
        fprintf(f, "P6\n256 192\n255\n");
        for (unsigned y = 0; y < SG_HEIGHT; y++)
            for (unsigned x = 0; x < SG_WIDTH; x++) {
                uint32_t rgb = sg1000_palette[image[y][x] & 15];
                uint8_t p[] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
                if (fwrite(p, 1, 3, f) != 3) { fclose(f); free(loaded); return 1; }
            }
        if (fclose(f)) { free(loaded); return 1; }
    }
#if SG_MSX
    for (unsigned k = 0; k < peeks; k++) printf("peek %04x=%02x\n", peek_at[k], sg1000_read(peek_at[k]));
#endif
    if (vram_path) {
        FILE *v = fopen(vram_path, "wb");
        if (!v || fwrite(machine.vram, 1, sizeof machine.vram, v) != sizeof machine.vram || fclose(v)) { perror(vram_path); free(loaded); return 1; }
    }
    printf("frames=%u cycles=%u pc=%04x compiled=%u interpreted=%u out_mismatch=%u run_hash=%08x\n",
           machine.frames, machine.cyc, machine.cpu.pc, machine.compiled, machine.interpreted, out_mismatch, run_hash());
    free(loaded);
    return machine.cpu.fault || out_mismatch ? 1 : 0;
}
