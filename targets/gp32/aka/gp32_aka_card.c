/* The SmartMedia card of the AKA, on its SD card: either a folder presented as
   a card, or a card image whose sectors are read and written in place through a
   small write-back cache. */
#include <string.h>

#include <esp_rom_sys.h>

#include "gp32_aka_card.h"
#include "gp32_smc.h"

#ifdef GP32_AKA_CARD_FOLDER

#include <dirent.h>
#include <stdio.h>

#include <driver/sdmmc_host.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>

#include "esp32_config.h"
#include "gp32_vcard.h"

#define MOUNT "/sdcard"
/* Writes are sent to the folder once the program has stopped writing for this
   long, and at least this often while it does not stop. */
#define QUIET_US 1000000u
#define LATEST_US 30000000u

static uint32_t sent; /* the write count the folder is up to date with */

uint8_t *gp32_card_bytes(size_t offset, bool write) { return gp32_vcard_bytes(offset, write); }

void gp32_card_flush(void) { gp32_vcard_flush(); }

static void say(const char *message, const char *path)
{
    esp_rom_printf("gp32: card: %s %s\n", message, path);
}

static bool mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    host.max_freq_khz = 20000;
    slot.width = 4;
    slot.clk = (gpio_num_t)ESP32_SD_SCK;
    slot.cmd = (gpio_num_t)ESP32_SD_CMD;
    slot.d0 = (gpio_num_t)ESP32_SD_D0;
    slot.d1 = (gpio_num_t)ESP32_SD_D1;
    slot.d2 = (gpio_num_t)ESP32_SD_D2;
    slot.d3 = (gpio_num_t)ESP32_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t config = {.format_if_mount_failed = false,
                                                     .max_files = 4};
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &config, &card) == ESP_OK;
}

size_t gp32_aka_card_open(const char *name)
{
    static char folder[128];
    if (!mount() || (size_t)snprintf(folder, sizeof folder, MOUNT "/%s", name) >= sizeof folder)
        return 0;
    /* A missing folder is a mistake to report, not an empty card. */
    DIR *directory = opendir(folder);
    if (!directory)
        return 0;
    closedir(directory);
    sent = 0;
    return gp32_vcard_open(folder, MOUNT "/GP32SWAP.BIN", say);
}

static void send(void)
{
    uint32_t start = (uint32_t)esp_timer_get_time();
    sent = gp32_vcard_writes();
    bool done = gp32_vcard_sync();
    esp_rom_printf("gp32: card folder %s in %u ms\n", done ? "written" : "NOT written",
                   (unsigned)(((uint32_t)esp_timer_get_time() - start) / 1000));
}

void gp32_aka_card_service(uint32_t microseconds)
{
    static uint32_t seen, changed, last;
    uint32_t writes = gp32_vcard_writes();
    if (writes != seen) {
        seen = writes;
        changed = microseconds;
    }
    if (writes == sent) {
        last = microseconds;
    } else if (microseconds - changed >= QUIET_US || microseconds - last >= LATEST_US) {
        send();
        changed = last = (uint32_t)esp_timer_get_time();
    }
}

void gp32_aka_card_close(void)
{
    if (gp32_vcard_writes() != sent)
        send();
}

#else

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

void gp32_aka_card_service(uint32_t microseconds) { (void)microseconds; }

void gp32_aka_card_close(void) { gp32_card_flush(); }

#endif
