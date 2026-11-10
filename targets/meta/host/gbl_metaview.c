/*
 * Host view of the META screen: runs the generated game with the real META
 * display pipeline (gbl_display.c) and a simulated 160x128 ST7735, and
 * writes the panel as it would appear on the console.
 *
 *   <name>_metaview [--frames N] [--input FILE] [--mode smooth|tile|tblend|blend|dark|crop|skip]
 *                   [--dump N file.ppm]...
 */
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_display.h"
#include "m0_lcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t panel[128][160];
static uint32_t sent;

uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned r = 0; r < rows && row0 + r < 128; r++) memcpy(panel[row0 + r], p + r * 160, 320);
    return ++sent;
}
uint32_t m0_lcd_done_seq(void) { return sent; }

static uint32_t max_frames = 600;
static struct { uint32_t frame; const char *path; } dumps[256];
static int ndumps;
static struct { uint32_t frame; uint8_t buttons; } inputs[4096];
static int ninputs, input_pos;

static void write_panel(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n160 128\n255\n");
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 160; x++) {
            uint16_t be = panel[y][x], v = (uint16_t)((be >> 8) | (be << 8));
            uint8_t px[3] = {(uint8_t)((v >> 8) & 0xF8), (uint8_t)((v >> 3) & 0xFC), (uint8_t)(v << 3)};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

#if GBL_ROM_STREAM
extern const uint8_t gbl_rom_full[];
int gbl_platform_rom_read(uint32_t sector, uint8_t *dst) {
    uint32_t off = sector * 512u;
    for (uint32_t i = 0; i < 512u; i++) dst[i] = off + i < gbl_rom_size ? gbl_rom_full[off + i] : 0xFF;
    return 1;
}
#endif

#if GBL_SAVE_BYTES > 0
/* In memory, a fresh card's .SAV (0xFF) at the start, as gbl_host.c. */
static uint8_t save_mem[GBL_SAVE_BYTES];
static uint8_t save_valid[(GBL_SAVE_BYTES + 511) / 512];
int gbl_platform_save_read(uint32_t sector, uint8_t *dst) {
    if (sector * 512u >= GBL_SAVE_BYTES) return 0;
    if (save_valid[sector]) memcpy(dst, save_mem + sector * 512u, 512);
    else memset(dst, 0xFF, 512);
    return 1;
}
int gbl_platform_save_write(uint32_t sector, const uint8_t *src) {
    if (sector * 512u >= GBL_SAVE_BYTES) return 0;
    memcpy(save_mem + sector * 512u, src, 512);
    save_valid[sector] = 1;
    return 1;
}
#endif

void gbl_platform_vblank(void) {
    gbl_display_flush();
    uint32_t f = gbl_frame;
    for (int i = 0; i < ndumps; i++)
        if (dumps[i].frame == f) write_panel(dumps[i].path);
    while (input_pos < ninputs && inputs[input_pos].frame <= f) gbl_set_buttons(inputs[input_pos++].buttons);
    if (f >= max_frames) exit(0);
}

static uint8_t parse_buttons(const char *s) {
    static const char *names[8] = {"a", "b", "select", "start", "right", "left", "up", "down"};
    char buf[128];
    uint8_t m = 0;
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *t = strtok(buf, "+"); t; t = strtok(0, "+"))
        for (int i = 0; i < 8; i++)
            if (!strcmp(t, names[i])) m |= (uint8_t)(1u << i);
    return m;
}

int main(int argc, char **argv) {
    uint8_t mode = META_MODE_TBLEND;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            mode = !strcmp(m, "skip") ? META_MODE_SKIP : !strcmp(m, "dark") ? META_MODE_DARK
                 : !strcmp(m, "crop") ? META_MODE_CROP : !strcmp(m, "blend") ? META_MODE_BLEND
                 : !strcmp(m, "tile") ? META_MODE_TILE : !strcmp(m, "smooth") ? META_MODE_SMOOTH
                 : META_MODE_TBLEND;
        } else if (!strcmp(argv[i], "--dump") && i + 2 < argc && ndumps < 256) {
            dumps[ndumps].frame = (uint32_t)atoi(argv[++i]);
            dumps[ndumps++].path = argv[++i];
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "r");
            char line[256], btn[128];
            unsigned fr;
            while (f && fgets(line, sizeof(line), f) && ninputs < 4096)
                if (line[0] != '#' && sscanf(line, "%u %127s", &fr, btn) == 2) {
                    inputs[ninputs].frame = fr;
                    inputs[ninputs++].buttons = parse_buttons(btn);
                }
            if (f) fclose(f);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    char probe;
    gbl_stack_limit = (uintptr_t)&probe - 256u * 1024u;
    gbl_reset();
    gbl_display_set_mode(mode);
    gbl_run();
    return 0;
}
