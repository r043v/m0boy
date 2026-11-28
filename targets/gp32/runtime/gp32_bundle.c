#include "gp32_bundle.h"
#include "gp32_lzma.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#define HEADER 80u

extern const uint8_t gp32_bundle_start[], gp32_bundle_end[];

static uint32_t word(const uint8_t *at)
{
    return at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static bool make_directory(const char *path)
{
#ifdef _WIN32
    return !_mkdir(path) || errno == EEXIST;
#else
    return !mkdir(path, 0777) || errno == EEXIST;
#endif
}

static bool same_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    uint8_t buffer[4096];
    size_t at = 0, count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0 &&
           count <= size - at && !memcmp(buffer, data + at, count))
        at += count;
    fclose(file);
    return !count && at == size;
}

static bool exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file)
        fclose(file);
    return file != NULL;
}

static bool writable(const char *directory)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s.probe", directory);
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    fclose(file);
    remove(path);
    return true;
}

/* `directory` receives a prefix for the file names, ending with a separator. */
static bool save_directory(const char *stem, char *directory, size_t capacity)
{
    for (int fallback = 0; fallback < 2; ++fallback) {
        char *base = fallback ? SDL_GetPrefPath("m0boy", stem) : SDL_GetBasePath();
        if (!base)
            continue;
#ifdef _WIN32
        /* The C file functions only take the ANSI code page: enter the
           directory with its Unicode name and use relative paths from there. */
        wchar_t wide[1024];
        bool entered = MultiByteToWideChar(CP_UTF8, 0, base, -1, wide, 1024) &&
                       SetCurrentDirectoryW(wide);
        SDL_free(base);
        if (!entered)
            continue;
        int length = fallback ? snprintf(directory, capacity, "./")
                              : snprintf(directory, capacity, "%s-save/", stem);
#else
        int length = fallback ? snprintf(directory, capacity, "%s", base)
                              : snprintf(directory, capacity, "%s%s-save/", base, stem);
        SDL_free(base);
#endif
        if (length > 0 && (size_t)length < capacity && make_directory(directory) &&
            writable(directory))
            return true;
    }
    return false;
}

static bool write_card(const char *path, const uint8_t *in, size_t size)
{
    uint8_t *card = malloc(size);
    bool ok = card != NULL &&
              gp32_lzma_decode(card, size, in, (size_t)(gp32_bundle_end - in)) &&
              gp32_host_save_file(path, card, size);
    free(card);
    return ok;
}

bool gp32_bundle_install(Gp32Host *host)
{
    static char app[1100], card[1100], eeprom[1100];
    const uint8_t *pack = gp32_bundle_start;
    size_t size = (size_t)(gp32_bundle_end - gp32_bundle_start);
    if (size < HEADER || memcmp(pack, "M0G32PK3", 8) || pack[71])
        return false;
    const char *name = (const char *)pack + 8;
    size_t app_size = word(pack + 72), card_size = word(pack + 76);
    if (app_size > size - HEADER)
        return false;
    char stem[64], directory[1024];
    snprintf(stem, sizeof(stem), "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = 0;
    if (!save_directory(stem, directory, sizeof(directory)))
        return false;
    snprintf(app, sizeof(app), "%s%s", directory, name);
    snprintf(card, sizeof(card), "%s%s.smc", directory, stem);
    snprintf(eeprom, sizeof(eeprom), "%sgp32.eeprom", directory);
    if (!same_file(app, pack + HEADER, app_size) &&
        !gp32_host_save_file(app, pack + HEADER, app_size))
        return false;
    host->exec_path = app;
    if (card_size && !host->card_path) {
        if (!exists(card) && !write_card(card, pack + HEADER + app_size, card_size))
            return false;
        host->card_path = card;
    }
    if (!host->eeprom_path)
        host->eeprom_path = eeprom;
    return true;
}
