/*
 * The option menu's palettes, shared by the consoles' display pipelines
 * (meta_display.h).
 *
 * Vert (the DMG green) is palette 0 and the default.  When m0recomp found
 * the title in the Game Boy Color boot ROM's compatibility table it
 * carries that palette in gbl_config.h (GBL_PALETTE_GBC, resolved at
 * conversion time — the boot ROM's own colouring, four colours) and it
 * comes second in the menu.  The boot ROM's 12 generic palettes (the ones
 * its key combinations select at power-on) are resolved from the emitted
 * base table and triplets for the direction held at power-on
 * (gbl_palette_boot); the triplets keep OBJ0 and OBJ1 for the day
 * sprites get palettes of their own.  The per-title table itself lives in
 * the converter: the firmware never carries it, only what it plays.
 */
#include "gbl_display.h"
#include "m0_menu.h" /* M0_MB_*: the boot combos read the same bits */

#ifdef GBL_PALETTE_GBC
const uint32_t gbl_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {GBL_PALETTE_GBC},                        /* the boot ROM's, for this game */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const gbl_palette_names[META_PALETTE_COUNT] = {
    "Vert", "GBC", "Gris", "Pocket", "Ambre", "Bleu",
};
#else
/* Lightest shade first (0xRRGGBB). */
const uint32_t gbl_palettes[META_PALETTE_COUNT][4] = {
    {0xE0F8D0, 0x88C070, 0x346856, 0x081820}, /* green (DMG-like) */
    {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000}, /* grey */
    {0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F}, /* Pocket */
    {0xF8E8C8, 0xD8A060, 0x985020, 0x301808}, /* amber */
    {0xE0F0FF, 0x80A8E0, 0x3050A0, 0x101830}, /* blue */
};
const char *const gbl_palette_names[META_PALETTE_COUNT] = {"Vert", "Gris", "Pocket", "Ambre", "Bleu"};
#endif

#ifdef GBL_GBC_BASE
/* The boot ROM's 30 base palettes and the 12 key combinations as
 * (OBJ0, OBJ1, BG) base indices, emitted by gbrecomp. */
static const uint32_t gbc_base[30][4] = {GBL_GBC_BASE};
static const uint8_t gbc_combos[12][3] = {GBL_GBC_COMBOS};
#endif

const uint32_t *gbl_palette_boot(uint8_t buttons) {
#ifdef GBL_GBC_BASE
    /* The boot ROM's rule: the first pressed direction in Right, Left,
     * Up, Down order, +4 with A and +8 with B; nothing without one. */
    unsigned dir = buttons & M0_MB_RIGHT ? 1
                 : buttons & M0_MB_LEFT  ? 2
                 : buttons & M0_MB_UP    ? 3
                 : buttons & M0_MB_DOWN  ? 4
                                           : 0;
    if (!dir) return 0;
    const uint8_t *combo = gbc_combos[dir - 1 + (buttons & M0_MB_A ? 4 : 0) +
                                       (buttons & M0_MB_B ? 8 : 0)];
    return gbc_base[combo[2]]; /* the BG third; OBJ0/OBJ1 wait for sprite palettes */
#else
    (void)buttons;
    return 0;
#endif
}
