/* Neo Geo Pocket host harness: a cartridge image in the machine, N frames
 * out, and the run's fingerprint (CPU, the work RAM, the video RAMs, the
 * picture) — the same contract shape as the other runtimes.  The
 * interpreter is the reference a recompiled project must match; --coverage
 * writes the interpreter's executions per address for the generator. */
#include "../runtime/ngp.h"
#include "m0_host_util.h"
#include "ngp_hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t image[NGP_SCREEN_H][NGP_SCREEN_W];
static FILE *audio_file;
static uint32_t audio_samples;
static unsigned frames = 300;
static unsigned trace_count, trace_at;
static const char *input_script;
static m0_input_t input_file;
static const char *const input_names[] = { "up", "down", "left", "right", "a", "b", "option" };
static uint32_t random_state;

/* read by gm0 --record-input (found by its tag): the frame counter and the
 * buttons the game receives, so a gm0 session replays in the harness */
static const struct { char tag[8]; uint32_t version; const volatile void *frame, *buttons, *extra; }
    ngp_input_probe __attribute__((used, aligned(4))) = { "m0input", 1, &machine.frames, &machine.buttons, 0 };

static uint32_t run_hash(void) { return ngp_run_hash(&image[0][0]); }

static void image_grab(void) {
    memcpy(image, ngp_frame(), sizeof image);
}

static void ppm_write(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "ppm %s\n", path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", NGP_SCREEN_W, NGP_SCREEN_H);
    for (unsigned y = 0; y < NGP_SCREEN_H; y++)
        for (unsigned x = 0; x < NGP_SCREEN_W; x++) {
            const uint16_t p = image[y][x];
            const uint8_t rgb[3] = {
                (uint8_t)(((p >> 11) & 0x1F) * 255 / 31),
                (uint8_t)(((p >> 5) & 0x3F) * 255 / 63),
                (uint8_t)((p & 0x1F) * 255 / 31) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void wav_open(const char *path, unsigned rate) {
    uint8_t header[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0, 0, 0 };
    const uint32_t byte_rate = rate * 4;
    header[24] = (uint8_t)rate; header[25] = (uint8_t)(rate >> 8);
    header[26] = (uint8_t)(rate >> 16); header[27] = (uint8_t)(rate >> 24);
    header[28] = (uint8_t)byte_rate; header[29] = (uint8_t)(byte_rate >> 8);
    header[30] = (uint8_t)(byte_rate >> 16); header[31] = (uint8_t)(byte_rate >> 24);
    audio_file = fopen(path, "wb");
    if (!audio_file) { fprintf(stderr, "wav %s\n", path); exit(1); }
    fwrite(header, 1, sizeof header, audio_file);
}

static void wav_frame(unsigned rate) {
    int16_t samples[44100 * 2 / 60 + 16];
    const unsigned want = rate / 60 + 1;
    ngp_audio(samples, want, rate);
    audio_samples += want;
    fwrite(samples, 2, want * 2, audio_file);
}

static void wav_close(void) {
    if (!audio_file) return;
    uint32_t sizes[2];
    const uint32_t data_bytes = audio_samples * 4;
    const uint32_t riff = data_bytes + 36;
    sizes[0] = data_bytes; sizes[1] = riff;
    fseek(audio_file, 4, SEEK_SET);
    fwrite(&riff, 4, 1, audio_file);
    fseek(audio_file, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, audio_file);
    fclose(audio_file);
    (void)sizes;
}

/* the flash saves: <rom>.ngpflash beside the image, or --ram-out/--ram-in */
static void put_file(void *ctx, const void *data, uint32_t n) { fwrite(data, 1, n, (FILE *)ctx); }
static int get_file(void *ctx, void *data, uint32_t n) { return fread(data, 1, n, (FILE *)ctx) == n; }

static void flash_write(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "flash save %s\n", path); exit(1); }
    const uint32_t bytes = ngp_flash_bytes();
    fwrite(&bytes, 4, 1, f);
    ngp_flash_io(f, put_file, NULL);
    fclose(f);
}

static void flash_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t bytes = 0;
    if (fread(&bytes, 4, 1, f) != 1) { fclose(f); return; }
    if (bytes == ngp_flash_bytes()) ngp_flash_io(f, NULL, get_file);
    fclose(f);
}

/* --save-state / --load-state: the machine's stream through a file */
static void state_put(void *ctx, const void *data, uint32_t n) { fwrite(data, 1, n, (FILE *)ctx); }
static int state_get(void *ctx, void *data, uint32_t n) { return fread(data, 1, n, (FILE *)ctx) == n; }

static void state_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "state %s\n", path); exit(1); }
    const uint32_t bytes = ngp_state_bytes();
    if (!bytes) { fprintf(stderr, "state: the field table is broken\n"); exit(1); }
    fwrite(&bytes, 4, 1, f);
    {
        uint8_t *buf = malloc(bytes);
        ngp_state_io(buf, bytes, 0);
        fwrite(buf, 1, bytes, f);
        free(buf);
    }
    /* the flash dirty bits and the written blocks travel with it */
    const uint32_t fbytes = ngp_flash_bytes();
    fwrite(&fbytes, 4, 1, f);
    ngp_flash_io(f, state_put, NULL);
    fclose(f);
}

static void state_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "state %s\n", path); exit(1); }
    uint32_t bytes = 0, fbytes = 0;
    if (fread(&bytes, 4, 1, f) != 1 || bytes != ngp_state_bytes()) { fprintf(stderr, "state size mismatch\n"); exit(1); }
    {
        uint8_t *buf = malloc(bytes);
        if (fread(buf, 1, bytes, f) != bytes) { fprintf(stderr, "state short\n"); exit(1); }
        ngp_state_io(buf, bytes, 1);
        free(buf);
    }
    if (fread(&fbytes, 4, 1, f) == 1 && fbytes == ngp_flash_bytes())
        ngp_flash_io(f, NULL, state_get);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *shot = NULL, *wav_path = NULL;
    const char *ram_out = NULL, *ram_in = NULL, *save_state = NULL, *load_state = NULL;
    int gfx_dump = 0;
    unsigned wav_rate = 44100;
    uint8_t *rom = NULL;
    uint32_t rom_size = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--frames") && i + 1 < argc) frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--trace") && i + 1 < argc) trace_count = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--trace-at") && i + 1 < argc) trace_at = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--ppm") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(a, "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(a, "--wav-rate") && i + 1 < argc) wav_rate = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--input") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(a, "--random") && i + 1 < argc) random_state = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ram-out") && i + 1 < argc) ram_out = argv[++i];
        else if (!strcmp(a, "--ram-in") && i + 1 < argc) ram_in = argv[++i];
        else if (!strcmp(a, "--save-state") && i + 1 < argc) save_state = argv[++i];
        else if (!strcmp(a, "--gfx-dump")) gfx_dump = 1;
        else if (!strcmp(a, "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(a, "--interp")) { /* the interpreter is the reference path */ }
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); return 1; }
        else rom_path = a;
    }
#ifndef NGP_EMBEDDED
    if (!rom_path) {
        fprintf(stderr,
            "usage: ngp_host ROM.ngp [options]\n"
            "  --frames N        run N frames (default 300)\n"
            "  --ppm FILE        write the last frame as PPM\n"
            "  --wav FILE        write the audio as WAV (44100 Hz stereo)\n"
            "  --input FILE      replay a button script (\"frame mask\" lines)\n"
            "  --random SEED     press random buttons\n"
            "  --ram-out/--ram-in FILE   the flash saves\n"
            "  --save-state/--load-state FILE  the machine state\n");
        return 1;
    }
#endif
    {
#ifdef NGP_EMBEDDED
        (void)rom_path;
        extern const uint8_t ngp_rom_data[];
        extern const unsigned ngp_rom_size;
        rom_size = ngp_rom_size;
        rom = malloc(rom_size);
        memcpy(rom, ngp_rom_data, rom_size);
#else
        FILE *f = fopen(rom_path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", rom_path); return 1; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || (n & 0x1FF)) { fprintf(stderr, "%s: not a cartridge image\n", rom_path); fclose(f); return 1; }
        rom_size = (uint32_t)n;
        rom = malloc(rom_size);
        if (fread(rom, 1, rom_size, f) != rom_size) { fprintf(stderr, "short read\n"); fclose(f); return 1; }
        fclose(f);
#endif
    }
    /* the cartridge writes its flash saves into a private copy */
    {
        uint8_t *writable = malloc(rom_size);
        memcpy(writable, rom, rom_size);
        free(rom);
        rom = writable;
    }
    machine.rom = rom;
    machine.rom_size = rom_size;
    machine.bios = malloc(NGP_BIOS_BYTES);
    ngp_bios_install();
    ngp_load_rom(rom, rom_size);
    ngp_reset();
    if (!input_script && !random_state) machine.buttons = 0;
    if (input_script && !m0_input_load(&input_file, input_script, input_names, sizeof input_names / sizeof *input_names)) {
        fprintf(stderr, "input script %s\n", input_script);
        return 1;
    }
    {
        char default_flash[1024];
        if (!ram_in && !ram_out) {
            snprintf(default_flash, sizeof default_flash, "%s.ngpflash", rom_path);
            flash_load(default_flash);
        } else if (ram_in) flash_load(ram_in);
    }
    if (load_state) state_load(load_state);
    if (wav_path) wav_open(wav_path, wav_rate);
    if (trace_count) {
        for (unsigned f = 0; f < trace_at; f++) {
            machine.buttons = 0;
            ngp_run_frame();
        }
        for (unsigned i = 0; i < trace_count; i++) {
            const uint32_t pc = machine.cpu.pc;
            const unsigned b = (machine.cpu.sr >> 8) & 3;
            printf("%06u %06x sr=%04x sp=%06x b%u=[%08x %08x %08x %08x] g=[%08x %08x %08x]%s\n",
                   i, pc, machine.cpu.sr, machine.cpu.gpr[3], b,
                   machine.cpu.bank[b][0], machine.cpu.bank[b][1],
                   machine.cpu.bank[b][2], machine.cpu.bank[b][3],
                   machine.cpu.gpr[0], machine.cpu.gpr[1], machine.cpu.gpr[2],
                   machine.cpu.halted ? " HALT" : "");
            ngp_tick((uint32_t)ngp_step());
            machine.executed++;
        }
        return 0;
    }
    for (unsigned f = 0; f < frames; f++) {
        machine.buttons = 0;
        if (input_script) machine.buttons = (uint8_t)m0_input_keys(&input_file, f);
        else if (random_state) {
            random_state = random_state * 1103515245u + 12345u;
            if ((random_state >> 16) % 8 == 0) machine.buttons = (uint8_t)((random_state >> 8) & 0x7F);
        }
        ngp_run_frame();
        image_grab();
        if (wav_path) wav_frame(wav_rate);
    }
    if (save_state) state_save(save_state);
    if (gfx_dump) {
        const ngp_gfx_t *g = &machine.gfx;
        printf("gfx: ctl=%02x 2d=%02x period=%02x bgc=%02x mode=%02x pf=%02x win=[%02x %02x %02x %02x] "
               "scr1=[%02x %02x] scr2=[%02x %02x] spo=[%02x %02x] raster=%u\n",
               g->control_int, g->control_2d, g->screen_period, g->bg_col, g->k2ge_mode, g->p_f,
               g->wba_h, g->wba_v, g->wsi_h, g->wsi_v,
               g->s1so_h, g->s1so_v, g->s2so_h, g->s2so_v, g->po_h, g->po_v, g->raster_line);
        unsigned tiles = 0, map1 = 0, map2 = 0, spr = 0;
        for (unsigned t = 0; t < 512; t++)
            for (unsigned r = 0; r < 16; r++)
                if (g->character_ram[t * 16 + r]) { tiles++; break; }
        for (unsigned i = 0; i < 0x800; i += 2) if (g->scroll_vram[i] | g->scroll_vram[i + 1]) map1++;
        for (unsigned i = 0x800; i < 0x1000; i += 2) if (g->scroll_vram[i] | g->scroll_vram[i + 1]) map2++;
        for (unsigned i = 0; i < 64; i++) if (g->sprite_vram[i * 4] | g->sprite_vram[i * 4 + 1] | g->sprite_vram[i * 4 + 2] | g->sprite_vram[i * 4 + 3]) spr++;
        printf("gfx: tiles=%u map1=%u map2=%u sprites=%u pal=", tiles, map1, map2, spr);
        for (unsigned i = 0; i < 0x40; i += 2)
            printf("%02x%02x ", g->palette_ram[0x1E0 + i], g->palette_ram[0x1E1 + i]);
        printf("\nworkspace: 6F91=%02x 6F92=%02x 6C58=%02x 6C59=%02x 6C5B=%02x 6F95=%02x flash_saved=%02x\n",
               machine.ram[0x6F91 - 0x4000], machine.ram[0x6F92 - 0x4000],
               machine.ram[0x6C58 - 0x4000], machine.ram[0x6C59 - 0x4000],
               machine.ram[0x6C5B - 0x4000], machine.ram[0x6F95 - 0x4000],
               machine.ram[0x4021 - 0x4000]);
        printf("sprites:");
        for (unsigned i = 0; i < 16; i++)
            if (g->sprite_vram[i * 4] | g->sprite_vram[i * 4 + 1] | g->sprite_vram[i * 4 + 2] | g->sprite_vram[i * 4 + 3])
                printf(" [%u]=%02x%02x %02x%02x", i, g->sprite_vram[i * 4], g->sprite_vram[i * 4 + 1],
                       g->sprite_vram[i * 4 + 2], g->sprite_vram[i * 4 + 3]);
        printf("\n");
        /* the map row under the middle of the screen (line 76 -> tile row 9)
         * and the first words of the tiles it names, to compare with what
         * the renderer draws */
        for (unsigned pl = 0; pl < 2; pl++) {
            const unsigned base = pl * 0x800;
            printf("map%u row9:", pl + 1);
            for (unsigned i = 0; i < 20; i++) {
                const unsigned e = base + (9 * 32 + i) * 2;
                printf(" %02x%02x", g->scroll_vram[e], g->scroll_vram[e + 1]);
            }
            printf("\n");
        }
        printf("tile 0x01: ");
        for (unsigned r = 0; r < 4; r++)
            printf("%02x%02x ", g->character_ram[0x10 + r * 2], g->character_ram[0x10 + r * 2 + 1]);
        printf("\ntile 0x100: ");
        for (unsigned r = 0; r < 4; r++)
            printf("%02x%02x ", g->character_ram[0x1000 + r * 2], g->character_ram[0x1000 + r * 2 + 1]);
        printf("\n");
        /* what the renderer actually produced on the middle line: the raw
         * rgb565 words for x=56..104 (the tile area) */
        {
            const uint16_t *line = ngp_frame() + 76 * 160;
            printf("line76[56:104]:");
            for (unsigned x = 56; x < 104; x++) printf(" %04x", line[x]);
            printf("\n");
        }
        /* art dump of the tiles the visible map row names */
        for (unsigned t = 0x18; t <= 0x20; t++) {
            unsigned any = 0;
            for (unsigned r = 0; r < 16; r++) any |= g->character_ram[t * 16 + r];
            if (!any) continue;
            printf("tile %02x:\n", t);
            for (unsigned r = 0; r < 8; r++) {
                const unsigned w = g->character_ram[t * 16 + r * 2] | (g->character_ram[t * 16 + r * 2 + 1] << 8);
                printf("  %04x ", w);
                for (int c = 7; c >= 0; c--) putchar(" .+#"[(w >> (c * 2)) & 3]);
                putchar('\n');
            }
        }
    }
    if (ram_out) flash_write(ram_out);
    else {
        char default_flash[1024];
        snprintf(default_flash, sizeof default_flash, "%s.ngpflash", rom_path);
        flash_write(default_flash);
    }
    if (shot) ppm_write(shot);
    if (wav_path) wav_close();
    extern uint32_t ngp_irq_count, ngp_vint_count;
    printf("frames=%u executed=%u pc=%06x last=%06x good=%06x irq=%u vint=%u ctlint=%02x prio71=%02x vt5=%06x run_hash=%08x\n",
           machine.frames, machine.executed, machine.cpu.pc, machine.last_pc, machine.last_good_pc,
           ngp_irq_count, ngp_vint_count, machine.gfx.control_int, machine.int_prio[1],
           machine.ram[NGP_IRQ_TABLE - 0x4000 + 5 * 4] |
             (machine.ram[NGP_IRQ_TABLE - 0x4000 + 5 * 4 + 1] << 8) |
             (machine.ram[NGP_IRQ_TABLE - 0x4000 + 5 * 4 + 2] << 16),
           run_hash());
    return 0;
}
