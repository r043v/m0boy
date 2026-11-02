/* The option menu of the Sega and PICO-8 frontends (sg_main.cpp, gg_main.cpp,
 * p8_main.cpp), the Game Boy's (meta_main.cpp) adapted: HOME (Pokitto: C)
 * held half a second alone opens it, MENU keeps its game role (Start, Pause,
 * the cartridge's restart).  The lines differ per console; what they share is
 * here: the HOME hold, the buttons of the menu, the info line (frames of the
 * last second and the stack never used) and the shared lines (Reprendre,
 * Redemarrer, Volume, Vitesse, Processeur, Quitter, plus the save states
 * when the frontend defines SG_MENU_STATE_SLOTS and the USB bootloader when
 * it defines SG_MENU_BOOTLOADER).
 *
 * Each frontend defines before including:
 *   SG_MENU_TAP      statements for a HOME tap (the game's key, whose hold
 *                    must then not be the game's key: the Pokitto's C)
 *   SG_MENU_RESTART  statements restarting the cartridge
 *   SG_MENU_QUIT     statements handing the console back to its loader
 *   SG_MENU_BOOTLOADER()  (META) statements entering the USB bootloader: the
 *                    sam-ba magic at 0x20007FFC, then a software reset —
 *                    works without META_USB, where the double reset would
 *                    reach the loader instead
 *   SG_MENU_RESUME   statements once the menu is closed (the pacing again)
 * optional:
 *   SG_MENU_ON_STACK_TOP
 *                    an expression: run the menu on the stack ending there
 *                    instead of the C stack (the Game Gear's lent card
 *                    cache; the menu needs about 600 bytes and the
 *                    dispatcher calls frame() deep in the game's calls),
 *                    with SG_MENU_UNLEND() statements giving the blocks
 *                    back once it has run
 *   SG_MENU_TITLE    the menu's title (default "Options")
 *   SG_MENU_STATE_SLOTS, SG_MENU_STATE_BUF(), SG_MENU_STATE_LOADED()
 *                    save states: the slot count (when the console has a
 *                    state file on the card), 512 bytes of scratch for the
 *                    card sectors, and what the frame loop re-synchronises
 *                    after a load
 *   SG_MENU_SCREEN_ENTER(), SG_MENU_SCREEN_LEAVE()
 *                    statements around the menu's drawing (the PICO-8 on
 *                    the META narrows the panel's columns to its picture)
 * and implements m0_menu_scratch (meta_menu.h) over its own buffers.
 */
#ifndef SG_MENU_H
#define SG_MENU_H

#include "m0_menu.h"
#include "m0_pad.h"
#include "m0_audio.h"
#include "m0_clock.h"

#include <string.h>

#ifndef SG_MENU_TITLE
#define SG_MENU_TITLE "Options"
#endif

static const char *const sg_volume_names[9] = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const sg_speed_names[2] = {"Normale", "Rapide"};
#ifdef SG_MENU_HZ /* the frame's rate, a frontend hook away (the NES's) */
static const char *const sg_hz_names[2] = {"50 Hz", "60 Hz"};
#endif
#if !M0_TARGET_PC /* one rate on the PC: no clock line */
#if M0_TARGET_ESPBOY
static const char *const sg_clock_names[2] = {"80 MHz", "160 MHz"};
#elif M0_TARGET_POKITTO
static const char *const sg_clock_names[2] = {"48 MHz", "72 MHz"};
#elif M0_TARGET_ESP32
static const char *const sg_clock_names[2] = {"80 MHz", "240 MHz"};
#else
static const char *const sg_clock_names[2] = {"48 MHz", "70 MHz"}; /* meta_clock.cpp: 72 asked, 70 run */
#endif
#endif

static char *sg_put_uint(char *p, unsigned v);

#ifdef SG_MENU_STATE_SLOTS
#include "m0_state.h"
/* The slots' names, the used ones without " vide". */
static char sg_slot_text[SG_MENU_STATE_SLOTS][12];
static const char *sg_slot_names[SG_MENU_STATE_SLOTS];
static const char *sg_state_msg; /* the last save state operation's result */
static uint8_t sg_state_slot;
static void sg_name_slots(void) {
    uint16_t used = m0_state_used();
    for (unsigned i = 0; i < SG_MENU_STATE_SLOTS; i++) {
        char *p = sg_put_uint(sg_slot_text[i], i + 1u);
        if (!(used >> i & 1u)) memcpy(p, " vide", 6);
        else *p = 0;
        sg_slot_names[i] = sg_slot_text[i];
    }
}
#endif

static char *sg_put_uint(char *p, unsigned v) {
    char tmp[10];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 10);
    while (n) *p++ = tmp[--n];
    return p;
}

/* Frames of the last second, for the info line (sg_menu_frame per frame). */
static uint32_t sg_stat_ms;
static uint16_t sg_frames, sg_ips;

static void sg_menu_frame(void) {
    sg_frames++;
    uint32_t ms = millis();
    if (ms - sg_stat_ms >= 1000) {
        sg_ips = sg_frames;
        sg_frames = 0;
        sg_stat_ms = ms;
    }
}

#if !M0_TARGET_ESPBOY && !M0_TARGET_PC
/* Free RAM painted at boot (sg_frontend.h), as the Game Boy's menu shows it. */
static unsigned sg_stack_never_used(void) {
    const uint32_t *p = (const uint32_t *)(((uintptr_t)&end + 3u) & ~(uintptr_t)3u);
    unsigned n = 0;
    while (p[n] == SG_STACK_PAINT_WORD) n++;
    return n * 4u;
}
#endif

/* HOME held half a second (alone) asks for the menu; any other key while it
 * is held cancels it (HOME stays exclusive).  On release without the hold,
 * SG_MENU_TAP (the game's key on the Pokitto, whose C is HOME).  The caller
 * passes the pad through META_PAD: the bits are the M0_MB_* ones on every
 * console. */
static uint8_t sg_menu_was_down, sg_menu_stage, sg_menu_request;
static uint32_t sg_menu_since;

#define SG_MENU_HOME_BIT M0_MB_HOME

static void sg_menu_poll(uint8_t mb) {
    const uint8_t home = mb & SG_MENU_HOME_BIT, others = mb & (uint8_t)~SG_MENU_HOME_BIT;
    uint32_t now = millis();
    if (home) {
        if (!sg_menu_was_down) {
            sg_menu_was_down = 1;
            sg_menu_stage = 0;
            sg_menu_since = now;
        } else if (sg_menu_stage == 0 && now - sg_menu_since >= 500) {
            sg_menu_stage = 1; /* no tap on release */
            sg_menu_request = 1;
        }
        if (sg_menu_stage == 0 && others) sg_menu_stage = 3;
    } else if (sg_menu_was_down) {
        sg_menu_was_down = 0;
        if (sg_menu_stage == 0) { SG_MENU_TAP(); }
    }
}

/* The menu's keys as the M0_MB_* bits m0_menu_run reads them. */
static uint8_t sg_menu_buttons(void) {
#if M0_TARGET_POKITTO
    /* C stays out: held, it opened the menu (meta_pad.h maps it to HOME) */
    return (uint8_t)(M0_PAD(SG_PAD_RAW()) & ~M0_MB_HOME);
#else
    return M0_PAD(SG_PAD_RAW()); /* the library's order, the M0_MB_* values */
#endif
}

static void sg_option_menu(void);

#ifdef SG_MENU_ON_STACK_TOP
/* Calls fn on the stack ending at top (the lent cache); sp restored. */
__attribute__((naked, noinline)) static void sg_call_on_stack(void (*fn)(void), uintptr_t top) {
    __asm__ volatile("push {r4, lr}\n\t"
                     "mov r4, sp\n\t"
                     "mov sp, r1\n\t"
                     "blx r0\n\t"
                     "mov sp, r4\n\t"
                     "pop {r4, pc}\n\t");
}
static void sg_menu_run(void) { sg_call_on_stack(sg_option_menu, SG_MENU_ON_STACK_TOP); }
#else
static void sg_menu_run(void) { sg_option_menu(); }
#endif

enum { SGM_RESUME, SGM_RESTART, SGM_SLOT, SGM_SAVE, SGM_LOAD, SGM_VOLUME, SGM_SPEED, SGM_HZ, SGM_CLOCK,
       SGM_USB_SD, SGM_BOOTLOADER, SGM_QUIT };

static void sg_option_menu(void) {
    uint8_t vol = m0_audio_volume();
    uint8_t fast = sg_menu_fast;
#if !M0_TARGET_PC
    uint8_t mhz = m0_clock_mhz() == (M0_TARGET_ESPBOY ? 160 : M0_TARGET_ESP32 ? 240 : 72);
#else
    uint8_t mhz = 0;
#endif
#ifdef SG_MENU_STATE_SLOTS
    uint8_t slot = sg_state_slot;
#endif
    meta_menu_item items[12];
    uint8_t ids[12];
    unsigned n = 0;
#define SG_ADD(id, label, type, value, count, names) \
    (ids[n] = (id), items[n++] = (meta_menu_item){label, type, value, count, names})
    SG_ADD(SGM_RESUME, "Reprendre", META_MENU_ACTION, 0, 0, 0);
    SG_ADD(SGM_RESTART, "Redemarrer", META_MENU_ACTION, 0, 0, 0);
#ifdef SG_MENU_STATE_SLOTS
    if (m0_state_available()) {
        sg_name_slots();
        SG_ADD(SGM_SLOT, "Etat", META_MENU_CHOICE, &slot, SG_MENU_STATE_SLOTS, sg_slot_names);
        SG_ADD(SGM_SAVE, "Sauver l'etat", META_MENU_ACTION, 0, 0, 0);
        SG_ADD(SGM_LOAD, "Charger l'etat", META_MENU_ACTION, 0, 0, 0);
    }
#endif
    SG_ADD(SGM_VOLUME, "Volume", META_MENU_CHOICE, &vol, 9, sg_volume_names);
    SG_ADD(SGM_SPEED, "Vitesse", META_MENU_CHOICE, &fast, 2, sg_speed_names);
#ifdef SG_MENU_HZ
    uint8_t hz = sg_menu_hz;
    SG_ADD(SGM_HZ, "Ecran", META_MENU_CHOICE, &hz, 2, sg_hz_names);
#endif
#if !M0_TARGET_PC /* the PC has one rate: no "Processeur" line */
    SG_ADD(SGM_CLOCK, "Processeur", META_MENU_CHOICE, &mhz, 2, sg_clock_names);
#endif
#ifdef SG_MENU_BOOTLOADER
#ifdef SG_MENU_USB_SD
    SG_ADD(SGM_USB_SD, "Carte SD USB", META_MENU_ACTION, 0, 0, 0);
#endif
    SG_ADD(SGM_BOOTLOADER, "Bootloader USB", META_MENU_ACTION, 0, 0, 0);
#endif
    SG_ADD(SGM_QUIT, "Quitter", META_MENU_ACTION, 0, 0, 0);
#undef SG_ADD
    /* "59 ips, pile 812": frames of the last second and the stack never
     * used so far (bytes above the end of .bss). */
    char info[32];
#ifdef SG_MENU_STATE_SLOTS
    if (sg_state_msg) {
        strcpy(info, sg_state_msg);
        sg_state_msg = 0;
    } else
#endif
    {
        char *p = sg_put_uint(info, sg_ips);
        memcpy(p, " ips, pile ", 11);
#if M0_TARGET_ESPBOY || M0_TARGET_PC
        p = sg_put_uint(p + 11, 0);
#else
        p = sg_put_uint(p + 11, sg_stack_never_used());
#endif
        *p = 0;
    }
    m0_audio_pause(1); /* the game's sound stops while the menu is open */
#ifdef SG_MENU_SCREEN_ENTER
    SG_MENU_SCREEN_ENTER();
#endif
    int line = m0_menu_run(SG_MENU_TITLE, items, n, sg_menu_buttons, info);
#ifdef SG_MENU_SCREEN_LEAVE
    SG_MENU_SCREEN_LEAVE();
#endif
    m0_audio_pause(0);
    int action = line >= 0 ? ids[line] : -1;
    m0_audio_set_volume(vol);
    sg_menu_fast = fast;
#ifdef SG_MENU_HZ
    SG_MENU_SET_HZ(hz == 0);
#endif
    m0_lcd_end_frame(); /* no transfer while the clocks change */
    m0_clock_set(M0_TARGET_ESPBOY ? (mhz ? 160 : 80) : M0_TARGET_ESP32 ? (mhz ? 240 : 80) : (mhz ? 72 : 48));
#ifdef SG_MENU_STATE_SLOTS
    sg_state_slot = slot;
    if (action == SGM_SAVE || action == SGM_LOAD) {
        m0_lcd_end_frame();
        if (action == SGM_SAVE)
            sg_state_msg = m0_state_save(slot, SG_MENU_STATE_BUF()) ? "Etat sauve" : "Echec sauvegarde";
        else if (!m0_state_load(slot, SG_MENU_STATE_BUF()))
            sg_state_msg = "Echec chargement";
        else {
            SG_MENU_STATE_LOADED();
            sg_state_msg = "Etat charge";
        }
    }
#endif
    SG_MENU_RESUME();
    if (action == SGM_RESTART) SG_MENU_RESTART();
#ifdef SG_MENU_BOOTLOADER
#ifdef SG_MENU_USB_SD
    if (action == SGM_USB_SD) SG_MENU_USB_SD();
#endif
    if (action == SGM_BOOTLOADER) SG_MENU_BOOTLOADER();
#endif
    if (action == SGM_QUIT) SG_MENU_QUIT();
}

/* Once per frame, from frame(): the menu opens here when HOME was held. */
static void sg_menu_tick(void) {
    if (!sg_menu_request) return;
    sg_menu_request = 0;
    sg_menu_run();
#ifdef SG_MENU_ON_STACK_TOP
    SG_MENU_UNLEND();
#endif
}

#endif
