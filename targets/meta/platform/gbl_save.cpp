/*
 * Cartridge RAM saves for the Gamebuino META.
 *
 * The save is a file prepared by compile.sh (GBL_SAVE_NAME, exactly the
 * size of the cartridge RAM) in the card's root directory or one directory
 * below.  It is located once at boot through the FAT; afterwards the
 * runtime reads and rewrites its 512-byte sectors in place, so the FAT is
 * never modified.  The runtime writes back when the game disables cartridge
 * RAM (games do after saving); gbl_save_tick also writes back two seconds
 * after the last change, and the frontend before returning to the loader.
 * Games that keep their variables in cartridge RAM (Super Mario Land 2,
 * Wario Land) change it every frame and never reach those two seconds: a
 * change also never waits more than META_SAVE_PERIOD frames, written back
 * one sector a frame so that the game does not stall.
 */
#include "gbl_save.h"

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
}

uint32_t gbl_save_reads, gbl_save_writes, gbl_save_errors; /* sectors (meta-usb.py read) */

#if GBL_SAVE_BYTES > 0 && (GBL_BATTERY || GBL_ERAM_ON_SD)

#include "m0_card.h"

#define MAX_EXTENTS 8

static meta_extent extents[MAX_EXTENTS];
static int n_extents;
static uint8_t have_file;

int gbl_save_init(int card_ok) {
    have_file = 0;
    uint32_t size = 0;
    if (card_ok && m0_fat_find(m0_card_sector, gbl_save_scratch(), GBL_SAVE_NAME83, extents,
                                 MAX_EXTENTS, &n_extents, &size) == M0_FAT_OK &&
        size >= GBL_SAVE_BYTES)
        have_file = 1;
    if (have_file) return META_SAVE_OK;
    return GBL_ERAM_ON_SD ? META_SAVE_MISSING : META_SAVE_NO_FILE;
}

extern "C" int gbl_platform_save_read(uint32_t sector, uint8_t *dst) {
    if (!have_file) return 0;
    if (m0_card_read(extents, n_extents, sector, dst)) {
        gbl_save_reads++;
        return 1;
    }
    gbl_save_errors++;
    return 0;
}

extern "C" int gbl_platform_save_write(uint32_t sector, const uint8_t *src) {
    if (!have_file) return 0;
    if (m0_card_write(extents, n_extents, sector, src)) {
        gbl_save_writes++;
        return 1;
    }
    gbl_save_errors++;
    return 0;
}

static uint32_t pending_since; /* gbl_frame when the oldest unsaved change was seen */
static int round_next = -1;    /* periodic round in progress: next way or sector */

void gbl_save_tick(void) {
    if (!gbl_save_pending()) {
        pending_since = gbl_frame;
        round_next = -1;
        return;
    }
    if (gbl_frame - gbl_save_last_change >= 120) {
        gbl_save_flush();
        round_next = -1;
        return;
    }
    if (round_next < 0) {
        if (!META_SAVE_PERIOD || gbl_frame - pending_since < META_SAVE_PERIOD) return;
        round_next = 0;
        pending_since = gbl_frame; /* changes from now on wait for the next round */
    }
    round_next = gbl_save_flush_next(round_next);
}

#else

int gbl_save_init(int card_ok) {
    (void)card_ok;
    return META_SAVE_OK;
}

void gbl_save_tick(void) {}

#if GBL_SAVE_BYTES > 0
/* Cartridge RAM without a battery: nothing to keep. */
extern "C" int gbl_platform_save_read(uint32_t sector, uint8_t *dst) { (void)sector; (void)dst; return 0; }
extern "C" int gbl_platform_save_write(uint32_t sector, const uint8_t *src) { (void)sector; (void)src; return 0; }
#endif

#endif
