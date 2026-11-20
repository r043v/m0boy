/* Runtime ROM loading for the PC executables built with `--rom-external` (md_pc, MD_ROM_EXTERNAL=1): the cartridge is
 * not embedded, so the binary and its C sources carry no copyrighted data.  The ROM comes from `--rom FILE`, or is
 * searched next to the executable (then in the current directory): the first file of a ROM extension whose size and
 * CRC-32 are the ones the project was generated for (md_rom_id.h).  A different ROM is refused: the recompiled code
 * belongs to one cartridge. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md_full.h"
#include "md_rom_id.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

const uint8_t *md_rom;
uint32_t md_rom_size;

static uint32_t crc32_of(const uint8_t *d, size_t n) {
    uint32_t t[256], c;
    for (uint32_t i = 0; i < 256; i++) { c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    c = 0xFFFFFFFFu; for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (16L << 20)) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}
static int accept(uint8_t *b, size_t n) {
    if (n != MD_ROM_EXPECT_SIZE || crc32_of(b, n) != MD_ROM_EXPECT_CRC32) return 0;
    md_rom = b; md_rom_size = (uint32_t)n; return 1;
}
static int has_ext(const char *name) {
    const char *dot = strrchr(name, '.'); if (!dot) return 0;
    static const char *const e[] = { ".md", ".gen", ".bin", ".smd", ".rom", NULL };
    for (int i = 0; e[i]; i++) {
#ifdef _WIN32
        if (!_stricmp(dot, e[i])) return 1;
#else
        if (!strcasecmp(dot, e[i])) return 1;
#endif
    }
    return 0;
}
static int search_dir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_ext(e->d_name)) continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t n; uint8_t *b = read_file(p, &n);
        if (b && accept(b, n)) { fprintf(stderr, "rom: %s\n", p); closedir(d); return 1; }
        free(b);
    }
    closedir(d); return 0;
}
static int exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap); if (!n || n >= cap) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1); if (n <= 0) return 0; out[n] = 0;
#endif
    char *s = strrchr(out, '/'); char *b = strrchr(out, '\\'); if (b && (!s || b > s)) s = b;
    if (!s) return 0; *s = 0; return 1;
}

int md_rom_load(const char *path) {
    if (path) {
        size_t n; uint8_t *b = read_file(path, &n);
        if (!b) { fprintf(stderr, "cannot read the ROM %s\n", path); return 0; }
        if (!accept(b, n)) { fprintf(stderr, "%s is not the ROM this executable was generated for (%u bytes, CRC-32 %08x expected)\n", path, (unsigned)MD_ROM_EXPECT_SIZE, (unsigned)MD_ROM_EXPECT_CRC32); free(b); return 0; }
        return 1;
    }
    char dir[4096];
    if (exe_dir(dir, sizeof dir) && search_dir(dir)) return 1;
    if (search_dir(".")) return 1;
    fprintf(stderr, "ROM not found: put the cartridge (%s, %u bytes, CRC-32 %08x) next to the executable or pass --rom FILE\n", MD_ROM_NAME, (unsigned)MD_ROM_EXPECT_SIZE, (unsigned)MD_ROM_EXPECT_CRC32);
    return 0;
}
