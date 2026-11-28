/* Headless speed measurement, also used to train profile guided builds. */
#define _POSIX_C_SOURCE 200809L
#include "gp32_host.h"
#if M0_GP32_PC_TOOLS
#include "gp32_shot.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void discard_audio(void *context, int16_t left, int16_t right, unsigned rate)
{
    (void)context; (void)left; (void)right; (void)rate;
}

int main(int argc, char **argv)
{
    Gp32Host host = {0};
    unsigned cpi = 2;
    int rest = 3;
    if (argc > 4 && !strcmp(argv[3], "--cpi")) {
        cpi = (unsigned)strtoul(argv[4], NULL, 10);
        rest = 5;
    }
    uint64_t limit = argc >= 3 ? strtoull(argv[2], NULL, 10) : 0;
#if M0_GP32_PC_TOOLS
    /* Resume from a shot, and take one at the end. */
    const char *from = NULL, *shot = NULL, *found = NULL;
#endif
    /* Another sequence of random buttons, and RAM kept at fixed values. */
    uint64_t seed = 12345;
    struct { uint32_t address, value; } freeze[8];
    unsigned freezes = 0;
    while (argc >= rest + 2) {
        const char *name = argv[argc - 2];
        if (!strcmp(name, "--seed")) {
            seed = strtoull(argv[argc - 1], NULL, 10) | 1;
            argc -= 2;
            continue;
        }
        if (!strcmp(name, "--freeze") && freezes < 8) {     /* ADDR=WORD */
            char *end;
            freeze[freezes].address = (uint32_t)strtoul(argv[argc - 1], &end, 16);
            freeze[freezes].value = *end == '=' ? (uint32_t)strtoul(end + 1, NULL, 0) : 0;
            if (freeze[freezes].address - 0x0c000000u > GP32_RAM_SIZE - 4)
                break;
            ++freezes;
            argc -= 2;
            continue;
        }
#if M0_GP32_PC_TOOLS
        const char **value = !strcmp(name, "--from") ? &from : !strcmp(name, "--shot") ? &shot
                             : !strcmp(name, "--found") ? &found : NULL;
        if (value) {
            *value = argv[argc - 1];
            argc -= 2;
            continue;
        }
#endif
        break;
    }
    if (!limit || !cpi || !gp32_host_options(&host, argc - rest, argv + rest)) {
        fprintf(stderr, "Usage: %s {BIOS.bin|--hle} steps [--cpi N] [--exec file] [--smc card]"
#if M0_GP32_PC_TOOLS
                        " [--from shot.png] [--shot shot.png] [--found map.code]"
#endif
                        "\n", argv[0]);
        return EXIT_FAILURE;
    }
    host.readonly = true;
    host.hle = !strcmp(argv[1], "--hle");
    S3c2400 *soc = malloc(sizeof(*soc));
    Gp32LcdCapture *capture = calloc(1, sizeof(*capture));
    uint8_t *bios = host.hle ? NULL : malloc(GP32_BIOS_SIZE);
    FILE *file = host.hle ? NULL : fopen(argv[1], "rb");
    bool loaded = host.hle || (file && bios &&
                  fread(bios, 1, GP32_BIOS_SIZE, file) == GP32_BIOS_SIZE);
    if (file)
        fclose(file);
    if (!soc || !capture || !loaded ||
        !gp32_host_boot(&host, soc, &bios, (Arm920tConfig){0}, (S3c2400Hooks){0},
                        (S3c2400Video){0})) {
        fprintf(stderr, "GP32 boot failed\n");
        return EXIT_FAILURE;
    }
    soc->cycles_per_instruction = cpi;
    soc->lcd_capture = capture;
    /* A game may need to write to its card (Her Knights hangs on its logo
       otherwise): writable in memory, never saved back to the file. */
    soc->smc.writable = soc->smc.image != NULL;
    soc->audio = (S3c2400Audio){discard_audio, NULL};
#if M0_GP32_PC_TOOLS
    uint64_t program = from || shot ? gp32_shot_program(host.exec_path) : 0;
    if (from && !gp32_shot_load(soc, &host, program, from)) {
        fprintf(stderr, "%s: not a shot of this program\n", from);
        return EXIT_FAILURE;
    }
#ifdef M0_GP32_AOT
    if (found)
        arm920t_aot_found(found);
#else
    (void)found; /* only a recompiled build knows what it lacks */
#endif
#endif
    uint64_t first = soc->steps, first_skipped = soc->skipped_steps;
    limit += first;

    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t exceptions = 0, random = seed, change = first;
    double emulated = 0;
    while (soc->steps < limit) {
        /* Held and released buttons, with START often enough to leave menus. */
        if (soc->steps >= change) {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            uint16_t held = (random & 7) == 7 ? 0x100
                : (random & 7) < 3 ? 0 : (uint16_t)((random >> 8) & (random >> 24) & 0x3ff);
            s3c2400_set_buttons(soc, held);
            change = soc->steps + 1500000;
        }
        for (unsigned i = 0; i < freezes; ++i)
            memcpy(soc->ram + (freeze[i].address - 0x0c000000u), &freeze[i].value, 4);
        uint64_t cycles = soc->cycles;
        if (!gp32_host_run(&host, soc, limit - soc->steps, &exceptions))
            break;
        uint32_t pll = soc->clock[1];
        double fclk = (((pll >> 12) & 255) + 8) * 12e6 /
                      ((((pll >> 4) & 63) + 2) << (pll & 3));
        emulated += (double)(soc->cycles - cycles) / fclk;
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);
    double wall = (double)(stop.tv_sec - start.tv_sec) +
                  (double)(stop.tv_nsec - start.tv_nsec) * 1e-9;
    printf("steps=%" PRIu64 " frames=%" PRIu64 " skipped=%.1f%% wall=%.2fs "
           "%.1f MIPS, %.1fs emulated (%.0f%% of real time)\n",
           soc->steps - first, soc->frames,
           100.0 * (soc->skipped_steps - first_skipped) / (soc->steps - first), wall,
           (soc->steps - first) / wall / 1e6, emulated, 100 * emulated / wall);
    /* Equal runs of two builds must end in the same machine state. */
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const uint64_t state[] = {soc->cycles, soc->frames, soc->skipped_steps, soc->cpu.cpsr};
    const struct { const void *data; size_t size; } parts[] = {
        {soc->ram, sizeof soc->ram}, {soc->cpu.r, sizeof soc->cpu.r}, {state, sizeof state}};
    for (unsigned part = 0; part < 3; ++part)
        for (size_t i = 0; i < parts[part].size; ++i)
            hash = (hash ^ ((const uint8_t *)parts[part].data)[i]) * UINT64_C(0x100000001b3);
    printf("state=%016" PRIx64, hash);
#ifdef M0_GP32_AOT
    printf(" interpreted=%.3f%% of %" PRIu64 " executed",
           100.0 * soc->interpreted_steps / (soc->steps - soc->skipped_steps),
           soc->steps - soc->skipped_steps);
#endif
    putchar('\n');
#if M0_GP32_PC_TOOLS
    if (shot && !gp32_shot_save(soc, &host, program, shot)) {
        fprintf(stderr, "%s: unable to write the shot\n", shot);
        return EXIT_FAILURE;
    }
#endif
    soc->smc.dirty = false;
    soc->eeprom_dirty = false;
    gp32_host_close(&host, soc);
    free(bios);
    free(capture);
    free(soc);
    return EXIT_SUCCESS;
}
