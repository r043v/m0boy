#pragma once
/* ROM loaded at run time for the PC executables generated with `--rom-external`: the project then carries no cartridge
 * bytes.  The ROM is `--rom FILE`, else searched next to the executable and in the current directory.  A file is
 * accepted when the image the generator would have embedded (the file minus a copier/iNES header, padded with $FF or
 * truncated to the expected size) has the project's size and CRC-32: the recompiled code belongs to one cartridge. */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <strings.h>
#include <unistd.h>
#endif

static const char *m0_romx_path_arg;

/* strips `--rom FILE` from argv (call first in main, before the program's own argument parsing) */
static inline void m0_romx_args(int *argc, char **argv) {
    for (int i = 1; i + 1 < *argc; ++i)
        if (!strcmp(argv[i], "--rom")) {
            m0_romx_path_arg = argv[i + 1];
            for (int k = i; k + 2 < *argc + 1; ++k) argv[k] = argv[k + 2];
            *argc -= 2;
            return;
        }
}
static inline uint32_t m0_romx_crc32(const uint8_t *d, size_t n) {
    uint32_t t[256], c;
    for (uint32_t i = 0; i < 256; i++) { c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static inline uint8_t *m0_romx_accept(const uint8_t *f, size_t n, size_t size, uint32_t crc) {
    static const size_t skips[] = { 0, 512, 128, 16, 528 };
    uint8_t *blob = (uint8_t *)malloc(size);
    if (!blob) return NULL;
    for (unsigned s = 0; s < sizeof skips / sizeof *skips; ++s) {
        if (skips[s] >= n) continue;
        const size_t have = n - skips[s];
        if (have > size + 0x10000u) continue; /* far longer than the image: another file */
        memset(blob, 0xFF, size);
        memcpy(blob, f + skips[s], have < size ? have : size);
        if (m0_romx_crc32(blob, size) == crc) return blob;
    }
    free(blob);
    return NULL;
}
static inline uint8_t *m0_romx_read(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (64L << 20)) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}
static inline int m0_romx_dir(char *out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap); if (!n || n >= cap) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1); if (n <= 0) return 0; out[n] = 0;
#endif
    char *s = strrchr(out, '/'); char *b = strrchr(out, '\\'); if (b && (!s || b > s)) s = b;
    if (!s) return 0; *s = 0; return 1;
}
static inline uint8_t *m0_romx_scan(const char *dir, size_t size, uint32_t crc) {
    DIR *d = opendir(dir); if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t n; uint8_t *f = m0_romx_read(p, &n); if (!f) continue;
        uint8_t *blob = m0_romx_accept(f, n, size, crc); free(f);
        if (blob) { fprintf(stderr, "rom: %s\n", p); closedir(d); return blob; }
    }
    closedir(d);
    return NULL;
}
/* mains without argc/argv (the console-style PC shells): `--rom FILE` read from the process's own command line, then
 * the M0_ROM environment variable */
static inline const char *m0_romx_cmdline(void) {
    static char buf[8192];
#ifdef _WIN32
    const char *c = GetCommandLineA(); const char *k = c ? strstr(c, "--rom ") : NULL;
    if (!k) return NULL;
    k += 6; while (*k == ' ') k++;
    size_t n = 0; int q = *k == '"'; if (q) k++;
    while (*k && (q ? *k != '"' : *k != ' ') && n + 1 < sizeof buf) buf[n++] = *k++;
    buf[n] = 0; return n ? buf : NULL;
#else
    FILE *f = fopen("/proc/self/cmdline", "rb"); if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    for (size_t i = 0; i < n;) {
        size_t l = strlen(buf + i);
        if (!strcmp(buf + i, "--rom") && i + l + 1 < n) return buf + i + l + 1;
        i += l + 1;
    }
    return NULL;
#endif
}
/* the project's ROM image, or NULL (the reason is printed) */
static inline const uint8_t *m0_romx_load(size_t size, uint32_t crc, const char *name) {
    uint8_t *blob = NULL;
    if (!m0_romx_path_arg) m0_romx_path_arg = m0_romx_cmdline();
    if (!m0_romx_path_arg) m0_romx_path_arg = getenv("M0_ROM");
    if (m0_romx_path_arg) {
        size_t n; uint8_t *f = m0_romx_read(m0_romx_path_arg, &n);
        if (!f) { fprintf(stderr, "cannot read the ROM %s\n", m0_romx_path_arg); return NULL; }
        blob = m0_romx_accept(f, n, size, crc); free(f);
        if (!blob) fprintf(stderr, "%s is not the ROM this executable was generated for (%u bytes, CRC-32 %08x expected)\n", m0_romx_path_arg, (unsigned)size, (unsigned)crc);
        return blob;
    }
    char dir[4096];
    if (m0_romx_dir(dir, sizeof dir)) blob = m0_romx_scan(dir, size, crc);
    if (!blob) blob = m0_romx_scan(".", size, crc);
    if (!blob) fprintf(stderr, "ROM not found: put the cartridge (%s, %u bytes, CRC-32 %08x) next to the executable or pass --rom FILE\n", name, (unsigned)size, (unsigned)crc);
    return blob;
}
