/* SNES host harness: a cartridge image in the machine, N frames out, and
 * the run's fingerprint (CPU, the 128 KiB of WRAM, the frame counter — the
 * same contract shape as the other runtimes).  Built with the recompiled
 * code (SNES_NATIVE), --interp runs the interpreter instead: the reference
 * a precise project must match.  --coverage writes the interpreter's
 * executions per address, the contexts (K, DBR, the direct page, the M/X/E
 * flags) they ran under, and the addresses a dynamic transfer (RTS, RTL,
 * RTI, a jump through a pointer, an interrupt) landed on. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_input.h"

#include "../runtime/snes.h"
#include "snes_hash.h"
#ifdef SNES_EMBEDDED
#include "snes_config.h"
extern const uint8_t snes_rom[];
#endif

#ifdef SNES_NATIVE
extern int snes_native;
#endif

/* the 65816 interpreter calls these every instruction when the build
 * defines SNES_TRACE (the generated host projects do); --coverage dumps
 * them.  The context bytes of the instruction (up to four an address) and
 * the dynamic landings go with it: the recompiler keys its instances by
 * both.  The addresses land in an open-addressing table, half full at
 * most: a 24-bit K:PC would need 16 Mi entries flat. */
#define COV_BITS 20
#define COV_SLOTS (1u << COV_BITS)
#define SNES_CTX_MAX 4
typedef struct {
    uint32_t pc;
    uint32_t hits;
    uint64_t ctx[SNES_CTX_MAX];
    uint8_t ctx_n;
    uint8_t dyn;
} cov_ent;
static cov_ent *cov;

static cov_ent *cov_at(uint32_t pc) {
    if (!cov) cov = calloc(COV_SLOTS, sizeof *cov);
    uint32_t h = (pc * 0x9E3779B1u) >> (32 - COV_BITS);
    for (;;) {
        if (!cov[h].pc) { cov[h].pc = pc; return &cov[h]; }
        if (cov[h].pc == pc) return &cov[h];
        h = (h + 1) & (COV_SLOTS - 1);
    }
}

void snes_trace_step(uint32_t pc24) {
    cov_ent *e = cov_at(pc24 & 0xFFFFFFu);
    if (e->hits < 0xFFFFFFFFu) e->hits++;
    const uint64_t m = snes_ctx_key(&machine.cpu);
    for (uint8_t i = 0; i < e->ctx_n; i++)
        if (e->ctx[i] == m) return;
    if (e->ctx_n < SNES_CTX_MAX) e->ctx[e->ctx_n++] = m;
}
void snes_trace_dyn(uint32_t pc24) { cov_at(pc24 & 0xFFFFFFu)->dyn = 1; }

/* the header's map nibble, as the generator reads it: a paired checksum
 * complement or a known map byte, HiROM's $21/$25 winning over LoROM's
 * only when LoROM's own header does not answer */
static int header_map(const uint8_t *rom, uint32_t bytes, size_t at) {
    if (at + 0x40 > bytes) return -1;
    const unsigned comp = rom[at + 0x3C] | rom[at + 0x3D] << 8;
    const unsigned cksum = rom[at + 0x3E] | rom[at + 0x3F] << 8;
    const unsigned map = rom[at + 0x15] & 0x0F;
    if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) return (int)map;
    if (map == 0 || map == 1 || map == 2 || map == 3 || map == 5) return (int)map;
    return -1;
}
static int detect_hirom(const uint8_t *rom, uint32_t bytes) {
    /* ExHiROM: the header at $40FFC0, a paired checksum and a map nibble 5 */
    if (bytes > 0x40FFC0u + 0x40) {
        const int ex = header_map(rom, bytes, 0x40FFC0);
        if (ex >= 0 && (ex & 0x0F) == 5) return 2;
    }
    const int hi = header_map(rom, bytes, 0xFFC0);
    const int lo = header_map(rom, bytes, 0x7FC0);
    if (hi == 1 || hi == 5) return 1;
    if (lo == 0 || lo == 2 || lo == 3) return 0;
    return hi >= 0 && lo < 0 ? 1 : 0;
}
/* the header's RAM byte, as the generator reads it: $01-$03 are 2/8/32 KiB,
 * the runtime's window mirrors 32 KiB at most */
static uint32_t header_sram(const uint8_t *rom, uint32_t bytes, int map) {
    static const uint32_t table[4] = {0, 2048, 8192, 32768};
    const size_t at = (size_t)(map == 0 ? 0x7FC0u : (map == 1 ? 0xFFC0u : 0x40FFC0u)) + 0x18;
    if (at >= bytes) return 0;
    return rom[at] < 4 ? table[rom[at]] : 0;
}
/* the header's DSP-1 mark: the map byte's high nibble $3 (LoROM/HiROM +
 * DSP-1) or a low nibble of 3 (LoROM + SRAM + DSP-1, $23) */
static int header_dsp1(const uint8_t *rom, uint32_t bytes, int map) {
    const size_t at = (size_t)(map == 0 ? 0x7FC0u : (map == 1 ? 0xFFC0u : 0x40FFC0u)) + 0x15;
    if (at + 1 > bytes) return 0;
    const unsigned map_byte = rom[at];
    return ((map_byte & 0xF0) == 0x30 || (map_byte & 0x0F) == 3) ? 1 : 0;
}
/* the header's SuperFX mark: type $15, or the map byte $13/$14 */
static int header_fx(const uint8_t *rom, uint32_t bytes, int map) {
    const size_t at = (size_t)(map == 0 ? 0x7FC0u : (map == 1 ? 0xFFC0u : 0x40FFC0u)) + 0x16;
    if (at + 1 > bytes) return 0;
    const unsigned map_byte = rom[at];
    return map_byte == 0x15 || rom[at - 1] == 0x13 || rom[at - 1] == 0x14 ? 1 : 0;
}
static int header_sa1(const uint8_t *rom, uint32_t bytes, int map) {
    (void)bytes; (void)map;
    const uint8_t type = rom[0x16];
    return type == 0x33 || type == 0x34;
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *input_path = 0, *coverage = 0, *capture = 0, *wav_path = 0;
    unsigned frames = 60;
    int interp = 0, dump_ppu = 0; unsigned wram_dump = 0xFFFFFFFFu;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--ppu")) dump_ppu = 1;
        else if (!strcmp(argv[i], "--wram") && i + 1 < argc) wram_dump = (unsigned)strtoul(argv[++i], 0, 16);
        else rom_path = argv[i];
    }
#ifdef SNES_NATIVE
    /* the profile counts the interpreter's steps: every instruction then */
    snes_native = !(interp || coverage);
#endif
    (void)interp;
    const uint8_t *rom;
    uint32_t rom_bytes;
    int hirom = 0;
    uint32_t sram_bytes = 0;
    if (!rom_path) {
#ifdef SNES_EMBEDDED
        rom = snes_rom;
        rom_bytes = SNES_ROM_SIZE;
        hirom = SNES_MAP;
        sram_bytes = SNES_SRAM_BYTES;
#else
        fprintf(stderr, "usage: snes_host rom.sfc [--frames n] [--input f] [--coverage f] [--interp]\n");
        return 2;
#endif
    } else {
        FILE *file = fopen(rom_path, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, size % 32768 == 512 ? 512 : 0, SEEK_SET); /* a 512-byte header, some collections carry one */
        rom_bytes = (uint32_t)(size - (size % 32768 == 512 ? 512 : 0));
        uint8_t *bytes = malloc(rom_bytes);
        if (!bytes || fread(bytes, 1, (size_t)rom_bytes, file) != rom_bytes) {
            fprintf(stderr, "truncated image: %s\n", rom_path);
            return 2;
        }
        fclose(file);
        rom = bytes;
        hirom = detect_hirom(rom, rom_bytes);
        sram_bytes = header_sram(rom, rom_bytes, hirom);
    }

    snes_init(rom, rom_bytes, hirom, sram_bytes, header_dsp1(rom, rom_bytes, hirom),
              header_sa1(rom, rom_bytes, hirom), header_fx(rom, rom_bytes, hirom));
    static const char *const names[12] = {
        "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r"
    };
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, names, 12)) return 2;
    /* the DSP's ring drained once per frame: the samples the S-DSP
     * rendered, 32000 Hz stereo, for --wav and the audio_hash */
    int16_t *audio = 0;
    size_t audio_n = 0, audio_cap = 0;
    for (unsigned f = 0; f < frames; f++) {
        unsigned keys = pc_input_keys(&script, f);
        uint16_t report = 0;
        for (unsigned i = 0; i < 12; i++) if (keys & (1u << i)) report |= (uint16_t)(0x8000u >> i);
        snes_set_joypad(report);
        snes_frame();
        const unsigned fill = (unsigned)((machine.dsp.ring_w - machine.dsp.ring_r) & 8191);
        if (audio_n + fill > audio_cap) {
            audio_cap = (audio_n + fill + 8192u) & ~(size_t)8191u;
            audio = realloc(audio, audio_cap * sizeof *audio);
        }
        for (unsigned i = 0; i < fill; i++) {
            audio[audio_n++] = machine.dsp.ring[machine.dsp.ring_r];
            machine.dsp.ring_r = (machine.dsp.ring_r + 1) & 8191;
        }
    }
    pc_input_free(&script);
    if (dump_ppu) { /* the PPU's registers $2100-$213F as the game left them */
        for (unsigned r = 0; r < 0x40; r++)
            fprintf(stderr, "%s%02x", (r & 15) ? " " : "\n21%02x:", machine.ppu_regs[r]);
        fprintf(stderr, "\n");
    }
    if (wram_dump < 0x20000u) {
        for (unsigned a = wram_dump; a < wram_dump + 64 && a < 0x20000u; a++)
            fprintf(stderr, "%s%02x", (a & 15) ? " " : "\nwram ", machine.wram[a]);
        fprintf(stderr, "\n");
    }
    uint8_t *image = malloc(256u * 224u * 3u);
    if (!image) { fprintf(stderr, "cannot allocate image\n"); return 2; }
    memcpy(image, machine.screen, 256u * 224u * 3u); /* the frame as displayed */
    if (capture) {
        FILE *out = fopen(capture, "wb");
        if (!out) { fprintf(stderr, "cannot write %s\n", capture); free(image); return 2; }
        fprintf(out, "P6\n256 224\n255\n");
        if (fwrite(image, 1, 256u * 224u * 3u, out) != 256u * 224u * 3u || fclose(out)) {
            fprintf(stderr, "cannot write capture\n"); free(image); return 2;
        }
    }
    free(image);

    const uint32_t h = snes_run_hash();
    const uint32_t image_hash = snes_image_hash();
    const uint32_t audio_hash = snes_audio_hash(audio, audio_n);
    if (wav_path && audio_n) { /* 32000 Hz, 16-bit stereo */
        FILE *out = fopen(wav_path, "wb");
        if (!out) { fprintf(stderr, "cannot write %s\n", wav_path); return 2; }
        const uint32_t data_bytes = (uint32_t)(audio_n * sizeof *audio);
        const uint8_t header[] = {
            'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
            16,0,0,0, 1,0, 2,0, 0x80,0x7d,0,0, 0,0x25,3,0, 4,0, 16,0,
            'd','a','t','a', (uint8_t)data_bytes, (uint8_t)(data_bytes >> 8),
            (uint8_t)(data_bytes >> 16), (uint8_t)(data_bytes >> 24),
        };
        uint8_t hdr[44];
        memcpy(hdr, header, sizeof header);
        const uint32_t riff = data_bytes + 36;
        hdr[4] = (uint8_t)riff; hdr[5] = (uint8_t)(riff >> 8);
        hdr[6] = (uint8_t)(riff >> 16); hdr[7] = (uint8_t)(riff >> 24);
        if (fwrite(hdr, 1, 44, out) != 44 ||
            fwrite(audio, sizeof *audio, audio_n, out) != audio_n || fclose(out)) {
            fprintf(stderr, "cannot write the wav\n");
            return 2;
        }
    }
    free(audio);
    if (coverage) {
        FILE *out = fopen(coverage, "w");
        if (!out) { fprintf(stderr, "cannot write %s\n", coverage); return 2; }
        fprintf(out, "# m0boy SNES coverage: K:PC address, the contexts it ran under"
                     " (K, DBR, direct page, M/X/E flags; K lowest, up to %d),"
                     " 65816 instructions started,"
                     " d = reached by RTS/RTL/RTI/a jump through a pointer/an interrupt\n", SNES_CTX_MAX);
        for (uint32_t i = 0; i < COV_SLOTS; i++) {
            if (!cov || !cov[i].pc) continue;
            fprintf(out, "%06x", cov[i].pc);
            for (unsigned c = 0; c < cov[i].ctx_n; c++)
                fprintf(out, " %010llx", (unsigned long long)cov[i].ctx[c]);
            fprintf(out, " %u%s\n", (unsigned)cov[i].hits, cov[i].dyn ? " d" : "");
        }
        fclose(out);
    }
    printf("frames=%u cycles=%u k=%02x pc=%04x run_hash=%08x image_hash=%08x audio_hash=%08x sram_dirty=%u\n",
           machine.frames, (unsigned)machine.cpu.cycles, machine.cpu.k, machine.cpu.pc, h, image_hash,
           audio_hash, machine.sram_dirty);
    return 0;
}
