/* The SmartMedia card as a file of the AKA's SD card: its sectors are read and
   written in place through a small write-back cache. */
#include <string.h>

#include <esp_rom_sys.h>

#include "gp32_aka_card.h"
#include "gp32_smc.h"
#include "m0_fat.h"
#include "m0_sd.h"

#define RUNS 128
#define LINES 16 /* a power of two */

static meta_extent runs[RUNS];
static int run_count;
static uint32_t file_size;
static struct {
    uint32_t tag; /* file sector + 1, 0 when empty */
    uint8_t dirty;
} lines[LINES];
static uint8_t cache[LINES][512] __attribute__((aligned(32)));

static void flush_line(unsigned i)
{
    if (!lines[i].dirty)
        return;
    uint32_t lba = m0_fat_lba(runs, run_count, lines[i].tag - 1);
    if (!lba || !m0_sd_write(lba, cache[i]))
        esp_rom_printf("gp32: card write failed, sector %u\n", (unsigned)(lines[i].tag - 1));
    lines[i].dirty = 0;
}

uint8_t *gp32_card_bytes(size_t offset, bool write)
{
    uint32_t sector = (uint32_t)(offset >> 9);
    unsigned i = sector & (LINES - 1);
    if (lines[i].tag != sector + 1) {
        flush_line(i);
        uint32_t lba = m0_fat_lba(runs, run_count, sector);
        if (!lba || !m0_sd_read(lba, cache[i])) {
            esp_rom_printf("gp32: card read failed, sector %u\n", (unsigned)sector);
            memset(cache[i], 0xff, 512);
        }
        lines[i].tag = sector + 1;
    }
    lines[i].dirty |= write;
    return cache[i] + (offset & 511u);
}

void gp32_card_flush(void)
{
    for (unsigned i = 0; i < LINES; ++i)
        flush_line(i);
}

size_t gp32_aka_card_open(const char *name83)
{
    if (!m0_sd_init())
        return 0;
    memset(lines, 0, sizeof lines);
    /* The first cache line serves as the lookup's work sector. */
    if (m0_fat_find(m0_sd_read, cache[0], name83, runs, RUNS, &run_count, &file_size) != M0_FAT_OK)
        return 0;
    return file_size;
}
