#include "gp32_host.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t rejected, fetches, lcd_reads, audio_frames, audio_nonzero;
    unsigned audio_rate;
    uint32_t first_rejected[24];
    unsigned distinct;
} BootStats;

static void access_event(void *context, const S3c2400Access *event)
{
    BootStats *stats = context;
    if (event->accepted) {
        stats->fetches += event->kind == S3C2400_FETCH;
        stats->lcd_reads += event->kind == S3C2400_LCD_LOAD;
        return;
    }
    ++stats->rejected;
    for (unsigned i = 0; i < stats->distinct; ++i)
        if (stats->first_rejected[i] == event->address)
            return;
    if (stats->distinct < sizeof(stats->first_rejected) / sizeof(stats->first_rejected[0]))
        stats->first_rejected[stats->distinct++] = event->address;
}

static void count_audio(void *context, int16_t left, int16_t right, unsigned rate)
{
    BootStats *stats = context;
    ++stats->audio_frames;
    stats->audio_nonzero += left != 0 || right != 0;
    stats->audio_rate = rate;
}

static bool write_screen(const S3c2400 *s, const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;

    /* The panel is mounted sideways on the GP32. */
    fprintf(file, "P6\n%u %u\n255\n", GP32_LCD_HEIGHT, GP32_LCD_WIDTH);
    for (unsigned y = 0; y < GP32_LCD_WIDTH; ++y) {
        for (unsigned x = 0; x < GP32_LCD_HEIGHT; ++x) {
            uint32_t pixel = s->pixels[x * GP32_LCD_WIDTH + GP32_LCD_WIDTH - 1 - y];
            const unsigned char rgb[] = {pixel >> 16, pixel >> 8, pixel};
            fwrite(rgb, 1, sizeof(rgb), file);
        }
    }
    bool ok = !ferror(file);
    return fclose(file) == 0 && ok;
}

int main(int argc, char **argv)
{
    char *end;
    if (argc == 3 && !strcmp(argv[1], "--identify"))
        return gp32_host_identify(argv[2]) ? EXIT_SUCCESS : EXIT_FAILURE;
    Gp32Host host = {0};
    if (argc < 4 || !gp32_host_options(&host, argc - 4, argv + 4)) {
        fprintf(stderr, "Usage: %s {BIOS.bin|--hle} steps screen.ppm [--smc image-or-directory] [--readonly] [--eeprom file] [--exec file] [--smcid hex] [--pduid hex] [--id-file GPS-or-GPM] [--ids]\n", argv[0]);
        fprintf(stderr, "       %s --identify executable-or-ID-record\n", argv[0]);
        return EXIT_FAILURE;
    }
    errno = 0;
    uint64_t limit = strtoull(argv[2], &end, 10);
    if (errno || *end || !limit || argv[2][0] == '-') {
        fprintf(stderr, "Invalid step count\n");
        return EXIT_FAILURE;
    }

    S3c2400 *soc = malloc(sizeof(*soc));
    host.hle = !strcmp(argv[1], "--hle");
    uint8_t *bios = host.hle ? NULL : malloc(GP32_BIOS_SIZE);
    FILE *file = host.hle ? NULL : fopen(argv[1], "rb");
    bool loaded = host.hle || (file && bios &&
                  fread(bios, 1, GP32_BIOS_SIZE, file) == GP32_BIOS_SIZE &&
                  fgetc(file) == EOF && !ferror(file));
    if (file)
        fclose(file);

    int status = EXIT_FAILURE;
    BootStats stats = {0};
    bool mounted = false;
    if (!loaded || !soc) {
        fprintf(stderr, "Expected a raw 512 KiB GP32 BIOS image and available memory\n");
    } else if (gp32_host_boot(&host, soc, &bios, (Arm920tConfig){0},
                              (S3c2400Hooks){.access = access_event, .context = &stats},
                              (S3c2400Video){0})) {
        mounted = true;
        soc->audio = (S3c2400Audio){count_audio, &stats};
        uint64_t exceptions = 0;
        for (uint64_t i = 0; i < limit; ++i)
            if (!gp32_host_step(&host, soc, &exceptions))
                break;
        fprintf(stderr, "steps=%" PRIu64 " pc=%08" PRIx32 " cpsr=%08" PRIx32
                " frames=%" PRIu64 " fetch=%" PRIu64 " lcd_reads=%" PRIu64
                " exceptions=%" PRIu64 " rejected=%" PRIu64 "\n",
                soc->steps, soc->cpu.r[15], soc->cpu.cpsr, soc->frames,
                stats.fetches, stats.lcd_reads, exceptions, stats.rejected);
        fprintf(stderr, "audio_frames=%" PRIu64 " audio_nonzero=%" PRIu64 " audio_rate=%u\n",
                stats.audio_frames, stats.audio_nonzero, stats.audio_rate);
        for (unsigned i = 0; i < stats.distinct; ++i)
            fprintf(stderr, "rejected address %08" PRIx32 "\n", stats.first_rejected[i]);
        if (write_screen(soc, argv[3]))
            status = host.hle && exceptions ? EXIT_FAILURE : EXIT_SUCCESS;
        else
            fprintf(stderr, "Unable to write %s\n", argv[3]);
    }
    if (!gp32_host_close(&host, mounted ? soc : NULL))
        status = EXIT_FAILURE;
    free(bios);
    free(soc);
    return status;
}
