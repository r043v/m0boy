/* PCE host harness: a HuCard image in the machine, N frames out, the
 * display's window as a PPM, the sound as a WAV, and the run's
 * fingerprint (CPU, WRAM, VRAM, SATB, palette, frame — the same contract
 * shape as the other runtimes).  Built with the recompiled code
 * (PCE_NATIVE), --interp runs the interpreter instead: the reference a
 * precise project must match.  --coverage writes the interpreter's
 * executions per address, the MPR contexts they ran under (the bank
 * windows are part of an address's identity for the recompiler), and the
 * addresses a dynamic transfer (RTS, RTI, a jump through a pointer, an
 * interrupt) landed on.  --explore FRAMES --random SEED (needs
 * --coverage) plays the card like the other harnesses' explorer
 * (m0_explore.h): segments from the start or from a snapshot of the
 * machine that reached code never seen; no --input is read on that path,
 * and no sound is rendered. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/pce.h"
#include "../../consoles/pc/pc_input.h"
#include "m0_explore_host.h"
#ifdef PCE_EMBEDDED
#include "../src/pce_config.h"
extern const uint8_t pce_rom[];
#endif

#define AUDIO_RATE 11025u

#ifdef PCE_NATIVE
extern int pce_native;
#endif

static int state_hash; /* --state-hash: the fingerprint after every frame */
static const char *wram_dump; /* --dump-wram FRAME FILE: WRAM's bytes then */
static unsigned wram_dump_frame;
static uint16_t frame[262][512];
static void on_row(void *host, unsigned line, const uint16_t *pixels, unsigned width) {
    (void)host;
    if (line < 262 && width <= 512)
        memcpy(frame[line], pixels, width * sizeof frame[line][0]);
}

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the HuC6280 interpreter calls these every instruction when the build
 * defines PCE_TRACE (the generated host projects do); --coverage dumps
 * them.  The MPR bytes of the instruction (up to four contexts an
 * address) and the dynamic landings go with it: the recompiler keys its
 * instances by both. */
static uint32_t pc_hits[65536];
static uint8_t pc_dyn[65536];
static uint32_t pc_locations; /* distinct addresses played: the explorer's novelty */
#define PCE_CTX_MAX 16
static uint64_t pc_ctx[65536][PCE_CTX_MAX];
static uint8_t pc_ctx_n[65536];

static uint64_t mpr_now(void) {
    uint64_t m = 0;
    for (unsigned i = 0; i < 8; i++) m |= (uint64_t)machine.cpu.mpr[i] << (8 * i);
    return m;
}
static int pce_trace_on; /* --coverage turns the counters on: the hooks
    run on every instruction, the games that only play skip the writes */
void pce_trace_step(uint16_t pc) {
    if (!pce_trace_on) return;
    if (!pc_hits[pc]) pc_locations++;
    pc_hits[pc]++;
    const uint64_t m = mpr_now();
    const uint8_t n = pc_ctx_n[pc];
    for (uint8_t i = 0; i < n; i++)
        if (pc_ctx[pc][i] == m) return;
    if (n < PCE_CTX_MAX) {
        pc_ctx[pc][n] = m;
        pc_ctx_n[pc] = (uint8_t)(n + 1);
    }
}
void pce_trace_dyn(uint16_t pc) { if (pce_trace_on) pc_dyn[pc] = 1; }

/* --explore frames: the coverage-guided player of m0_explore.h (the glue
 * in m0_explore_host.h), from the start or from snapshots that reached
 * code never seen.  The machine is one flat struct around stable pointers
 * (the bus's windows land in its own wram and the ROM's bytes), so a
 * snapshot is the struct itself. */
static m0xh_t xh;
static void snapshot_save(void *ctx, void *state) {
    (void)ctx;
    memcpy(state, &machine, sizeof machine);
}
static void snapshot_load(void *ctx, const void *state) {
    (void)ctx;
    memcpy(&machine, state, sizeof machine);
}
static unsigned explore_keys(unsigned k) {
    unsigned keys = 0;
    if (k & M0X_RIGHT) keys |= PCE_BTN_RIGHT;
    if (k & M0X_LEFT) keys |= PCE_BTN_LEFT;
    if (k & M0X_UP) keys |= PCE_BTN_UP;
    if (k & M0X_DOWN) keys |= PCE_BTN_DOWN;
    if (k & M0X_B1) keys |= PCE_BTN_I;
    if (k & M0X_B2) keys |= PCE_BTN_II;
    if (k & M0X_START) keys |= PCE_BTN_RUN;
    return keys;
}
static uint32_t explore_locations(void) { return pc_locations; }
static void host_frame(unsigned f);
static void explore_play(unsigned count) {
    for (unsigned f = 0; !xh.stop && (xh.on || f < count); f++) host_frame(f);
}
static void explore(void) {
    const m0x_machine_t m = {0, sizeof(pce_t), snapshot_save, snapshot_load, 0};
    if (!m0xh_run(&xh, &m, explore_locations, explore_play, 0)) return;
    machine.frames = xh.x.frames; /* the snapshots rewound it */
}

static FILE *audio;
static uint32_t audio_bytes;
static pc_input_t script = {0};

/* the run fingerprint's fields, shared by --state-hash and the final
 * print (the struct's bytes would hash bus pointers and padding) */
static uint32_t state_fingerprint(void) {
    uint32_t h = 2166136261u;
    /* registers field by field, then the frame count: the struct carries
     * the bus's fast-path pointers, and hashing its bytes would hash
     * addresses and padding */
    const uint8_t regs[12] = {
        machine.cpu.a, machine.cpu.x, machine.cpu.y, machine.cpu.s, machine.cpu.p,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
        (uint8_t)machine.cpu.cycles, (uint8_t)(machine.cpu.cycles >> 8),
        (uint8_t)(machine.cpu.cycles >> 16), (uint8_t)(machine.cpu.cycles >> 24),
        (uint8_t)(machine.frames & 0xFFu),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(machine.wram, sizeof machine.wram, h);
    h = fnv(machine.vdc.vram, sizeof machine.vdc.vram, h);
    h = fnv(machine.vdc.sat, sizeof machine.vdc.sat, h);
    h = fnv(machine.vce_colors, sizeof machine.vce_colors, h);
    if (machine.have_frame) {
        for (unsigned y = 0; y < machine.vdc.active_height; y++)
            h = fnv(frame[machine.first_active_line + y],
                    machine.vdc.active_width * sizeof frame[0][0], h);
    }
    return h;
}

static void host_frame(unsigned f) {
    if (xh.on) {
        machine.input = (uint8_t)m0xh_keys(&xh, explore_keys);
        pce_frame();
        /* a discovery stops the frame run at this boundary: the machine
         * is checkpointed with the observation, the segment goes on */
        m0xh_seen(&xh, pc_locations);
        return;
    }
    machine.input = (uint8_t)pc_input_keys(&script, f);
    pce_frame();
    if (state_hash)
        printf("frame %u state=%08x pc=%04x cyc=%u "
               "wram=%08x vram=%08x sat=%08x vce=%08x\n", f,
               state_fingerprint(), machine.cpu.pc, (unsigned)machine.cpu.cycles,
               fnv(machine.wram, sizeof machine.wram, 2166136261u),
               fnv(machine.vdc.vram, sizeof machine.vdc.vram, 2166136261u),
               fnv(machine.vdc.sat, sizeof machine.vdc.sat, 2166136261u),
               fnv(machine.vce_colors, sizeof machine.vce_colors, 2166136261u));
    if (wram_dump && f == wram_dump_frame) {
        FILE *out = fopen(wram_dump, "wb");
        if (out) { fwrite(machine.wram, 1, sizeof machine.wram, out); fclose(out); }
    }
    if (audio) {
        uint8_t samples[512];
        /* 119210 CPU cycles per frame: the samples that covers */
        static unsigned carry;
        carry += AUDIO_RATE * 119210u;
        unsigned count = carry / 7159090u;
        carry %= 7159090u;
        if (count > sizeof samples / 2) count = sizeof samples / 2;
        psg_render_stereo(&machine.psg, samples, count, AUDIO_RATE);
        fwrite(samples, 2, count, audio);
        audio_bytes += count * 2;
    }
}

/* `count` frames off the explore path */
static void play(unsigned count) {
    for (unsigned f = 0; f < count; f++) host_frame(f);
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *ppm = 0, *wav = 0, *input_path = 0, *coverage = 0;
    unsigned frames = 60;
    int interp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--state-hash")) state_hash = 1;
        else if (!strcmp(argv[i], "--dump-wram") && i + 2 < argc) {
            wram_dump_frame = (unsigned)atoi(argv[++i]);
            wram_dump = argv[++i];
        }
        else if (m0xh_arg(&xh, argc, argv, &i)) { /* --explore, --random */ }
        else rom_path = argv[i];
    }
#ifdef PCE_NATIVE
    /* the profile counts the interpreter's steps: every instruction then */
    pce_native = !(interp || coverage);
#endif
    pce_trace_on = coverage != 0;
    (void)interp;
    const uint8_t *rom;
    uint32_t rom_bytes;
    if (!rom_path) {
#ifdef PCE_EMBEDDED
        rom = pce_rom;
        rom_bytes = PCE_ROM_SIZE;
#else
        fprintf(stderr, "usage: pce_host rom.pce [--frames n] [--ppm f] [--wav f] [--input f] "
                        "[--coverage f] [--explore frames --random seed]\n");
        return 2;
#endif
    } else {
        FILE *file = fopen(rom_path, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, size % 131072 == 128 ? 128 : 0, SEEK_SET); /* a 128-byte header, some collections carry one */
        rom_bytes = (uint32_t)((size - (size % 131072 == 128 ? 128 : 0) + 0x1FFFu) & ~0x1FFFu);
        uint8_t *bytes = calloc(1, rom_bytes);
        if (fread(bytes, 1, (size_t)(size - (size % 131072 == 128 ? 128 : 0)), file) == 0) {
            fprintf(stderr, "truncated image: %s\n", rom_path);
            return 2;
        }
        fclose(file);
        rom = bytes;
    }

    pce_init(rom, rom_bytes);
    machine.row = on_row;
    /* a momentary-input script: "<frame> buttons" lines held for that one
     * frame, buttons by whole name ("i+right" — strstr would read "right"
     * as "i"), '#' lines are comments */
    static const char *const pce_names[8] = {
        "i", "ii", "select", "run", "up", "right", "down", "left"};
    if (input_path && !pc_input_load(&script, input_path, pce_names, 8)) return 2;
    if (xh.total && !coverage) {
        fprintf(stderr, "--explore needs --coverage: the explorer's novelty is the trace's addresses\n");
        return 2;
    }
    audio = wav ? fopen(wav, "wb") : 0;
    if (audio) { /* the header is patched once the length is known */
        const char silence[44] = {0};
        fwrite(silence, 1, 44, audio);
    }
    if (xh.total && coverage) explore();
    else play(frames);
    if (audio) {
        fseek(audio, 0, SEEK_SET);
        fprintf(audio, "RIFF");
        const uint32_t riff = 36 + audio_bytes;
        fwrite(&riff, 4, 1, audio);
        fprintf(audio, "WAVEfmt ");
        const uint32_t fmt = 16;
        fwrite(&fmt, 4, 1, audio);
        const uint16_t pcm = 1, ch = 2;
        fwrite(&pcm, 2, 1, audio);
        fwrite(&ch, 2, 1, audio);
        const uint32_t rate = AUDIO_RATE, byte_rate = AUDIO_RATE * 2;
        fwrite(&rate, 4, 1, audio);
        fwrite(&byte_rate, 4, 1, audio);
        const uint16_t block = 2, bits = 8;
        fwrite(&block, 2, 1, audio);
        fwrite(&bits, 2, 1, audio);
        fprintf(audio, "data");
        fwrite(&audio_bytes, 4, 1, audio);
        fclose(audio);
    }
    if (ppm && machine.have_frame) {
        FILE *out = fopen(ppm, "wb");
        if (out) {
            const unsigned width = machine.vdc.active_width, height = machine.vdc.active_height;
            fprintf(out, "P6\n%u %u\n255\n", width, height);
            for (unsigned y = 0; y < height; y++)
                for (unsigned x = 0; x < width; x++) {
                    const uint16_t c = frame[machine.first_active_line + y][x];
                    const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8), (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                    fwrite(px, 1, 3, out);
                }
            fclose(out);
        }
    }
    uint32_t h = state_fingerprint();
    if (coverage) {
        FILE *out = fopen(coverage, "w");
        if (!out) { fprintf(stderr, "cannot write %s\n", coverage); return 2; }
        fprintf(out, "# m0boy PCE coverage: address, the MPR contexts it ran under"
                     " (MPR0 first, up to %d), HuC6280 instructions started,"
                     " d = reached by RTS/RTI/a jump through a pointer/an interrupt\n", PCE_CTX_MAX);
        for (unsigned pc = 0; pc < 65536; pc++)
            if (pc_hits[pc]) {
                fprintf(out, "%04x", pc);
                for (unsigned i = 0; i < pc_ctx_n[pc]; i++)
                    fprintf(out, " %016llx", (unsigned long long)pc_ctx[pc][i]);
                fprintf(out, " %u%s\n", (unsigned)pc_hits[pc], pc_dyn[pc] ? " d" : "");
            }
        fclose(out);
    }
    printf("frames=%u cycles=%u pc=%04x run_hash=%08x\n",
           machine.frames, (unsigned)machine.cpu.cycles, machine.cpu.pc, h);
    return 0;
}
