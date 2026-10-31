/*
 * Gamebuino META frontend for gbl.
 *
 * Video: the display pipeline (meta_display.c) renders scanlines into strips
 * sent to the ST7735 by DMA while emulation continues.  The DMG's 144 lines
 * become the META's 128 with one of seven modes (HOME held one second
 * cycles them): TBLEND (default: TILE, with the last line of each tile row
 * merged into the next instead of dropped), SMOOTH (every row blends two
 * lines by area), TILE (drop the last line of each tile row), BLEND, DARK,
 * CROP, SKIP.
 *
 * Input:  D-pad, A = Game Boy B and B = Game Boy A (META_SWAP_AB), MENU =
 *         Start, HOME tap = Select, HOME held half a second (alone) =
 *         option menu (display mode, palette, frame skip, CPU clock,
 *         display SPI clock, back to the Gamebuino loader); any other key
 *         while HOME is held is the GBC's power-on palette combo instead
 *         (one of the boot ROM's generic palettes, live) and keeps the
 *         menu off.
 *
 * Pacing: 59.73 Hz, kept by an accumulator of elapsed time against emulated
 * frames (Gdl's rate).  Frame skip: automatic (when emulation falls behind
 * by a frame, rendering of up to META_MAX_SKIP consecutive frames is
 * skipped), none, or one frame drawn in two, three or four.  Frames whose
 * picture cannot differ from the one on screen are not rendered at all
 * (META_STATIC_SKIP).
 *
 * The same file is the Pokitto frontend (meta_target.h, --target pokitto):
 * the picture 1:1 in the middle of
 * the 220x176 panel (or enlarged to 200x176 as gm0 does, menu "Ecran"),
 * A = A, B = B, C tap = Start, C held half a second =
 * option menu, C + B = Select, C held at power-on = the Pokitto loader.
 * The META-only parts are under !M0_TARGET_POKITTO / M0_SCALED_MODES;
 * each console's drivers are in platform/meta and platform/pokitto, both
 * bare metal.
 */
#include "m0_target.h"

#include <string.h>
#if M0_TARGET_ESPBOY
#include "espboy_hw.h"
#elif M0_TARGET_POKITTO
#include "pokitto_hw.h"
#elif M0_TARGET_PC
#include "pc_hw.h"
#include <stdlib.h>
#elif M0_TARGET_ESP32
#if __has_include("odroid_hw.h")
#include "odroid_hw.h"
#else
#include "aka_hw.h"
#endif
#include <stdlib.h>
#else
#include "meta_hw.h"
#if META_USB_SERIAL
#include "meta_usb.h"
#endif
#if META_USB_SD && !M0_TARGET_POKITTO && !M0_TARGET_ESPBOY
#include "meta_usb_msc.h"
#endif
#endif
#include <setjmp.h>

#include "m0_audio.h"
#include "m0_clock.h"
#include "gbl_display.h"
#include "m0_lcd.h"
#include "m0_menu.h"
#include "m0_pad.h"
#include "m0_sd.h"
#include "gbl_save.h"
#include "m0_state.h"
#include "gbl_stream.h"
#include "m0_fat.h"

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
}
#if GBL_CACHE_DYN
#include "m0_stack_pool.h"
#endif

/* Switches (gbl_profile.h): META_MAX_SKIP, frames skipped in a row at
 * most by the automatic frame skip; META_STATIC_SKIP, no rendering nor
 * transfer while the picture cannot have changed (see
 * gbl_frame_unchanged_next; a change then appears one frame late). */

#define FRAME_US 16743u   /* 70224 cycles at 4.194304 MHz */

#ifndef GBL_GEN_VERSION
#define GBL_GEN_VERSION "unknown"
#endif
/* One contiguous string, kept in flash: compile.sh reads it back from
 * firmware.bin to tell which gbrecomp produced this firmware
 * ("gbrecomp-meta x.y.z", "gbrecomp-pokitto x.y.z"). */
static const char gbl_gen_version[] __attribute__((used)) =
    "gbrecomp-" M0_TARGET_NAME " " GBL_GEN_VERSION;

static uint8_t input_deferred;

/* Hand the last strip to the DMA and, when the bus is free (or input was
 * already deferred once), release it so the buttons can be read. */
static int close_frame(void) {
    gbl_display_flush();
    if (m0_lcd_busy() && !input_deferred) {
        input_deferred = 1;
        return 0;
    }
    m0_lcd_end_frame();
    input_deferred = 0;
    return 1;
}

/* ---- buttons -------------------------------------------------------------- */

/* META A gives Game Boy B and META B gives Game Boy A; 0 maps them straight
 * (the Pokitto's default: its A is already the right-hand button). */
#ifndef META_SWAP_AB
#define META_SWAP_AB M0_SWAP_AB_DEFAULT
#endif

#if M0_TARGET_ESPBOY
static void enter_loader(void) { gbl_save_flush(); espboy_restart(); }
static uint8_t meta_buttons(void) { return M0_PAD(espboy_buttons()); }
#elif M0_TARGET_PC
/* PC: "Quitter" leaves the emulator; unsaved cartridge RAM is written to
 * the card image first, if there is one. */
static void enter_loader(void) { gbl_save_flush(); exit(0); }
static uint8_t meta_buttons(void) { return M0_PAD(pc_buttons()); }
#elif M0_TARGET_ESP32
/* ESP32 consoles: "Quitter" restarts into the firmware's start, saves on
 * the card first. */
static void enter_loader(void) { gbl_save_flush(); esp32_restart(); }
static uint8_t meta_buttons(void) { return M0_PAD(esp32_buttons()); }
#elif !M0_TARGET_POKITTO

/* Hand the console back to the Gamebuino loader in the state it expects:
 * display idle, our audio and DMA interrupts off, SD card deselected and in
 * SPI mode. */
static void enter_loader(void) {
    m0_lcd_end_frame();
    m0_clock_set_lcd_spi(24); /* the loader expects the core's clocks */
    m0_clock_set(48);
    gbl_save_flush(); /* unsaved cartridge RAM changes */
    m0_audio_stop();
    NVIC_DisableIRQ(DMAC_IRQn);
    m0_watchdog_stop(); /* the loader does not feed it */
#if META_USB_SERIAL
    meta_usb_stop();
#endif
    m0_sd_deselect();
    ((void (*)(void))(*((uint32_t *)0x3FF4)))();
}

/* Pressed buttons in the menu's M0_MB_* order (meta_pad.h): the pad is
 * read by the bus driver (meta_pad_read, meta_spi.c) at
 * META_BUTTONS_SPI_HZ (gbl_board.h); the frontends only map the bits. */
static uint8_t meta_buttons(void) { return M0_PAD(meta_pad_read()); }

#else /* Pokitto */

/* Hand the console to the Pokitto loader: saves written back, sound off,
 * card deselected.  Without a loader in flash this returns. */
static void enter_loader(void) {
    gbl_save_flush();
    m0_audio_stop();
    m0_sd_deselect();
    pokitto_enter_loader();
    m0_audio_init();
}

/* Pokitto buttons as M0_MB_* bits (meta_pad.h): C plays the META's HOME. */
static uint8_t meta_buttons(void) { return M0_PAD(pokitto_buttons()); }

#endif /* !M0_TARGET_POKITTO */

#if !M0_TARGET_POKITTO && META_USB_SERIAL
/* The upload tools' 1200-baud touch (meta_usb.c): cartridge RAM saved,
 * then the bootloader.  playing: 0 on the boot error screens, where
 * neither the cartridge RAM nor the audio has been set up (stopping the
 * audio timer before m0_clock_init routes its clock would hang). */
static void usb_poll(int playing) {
    if (!meta_usb_bootloader_request) return;
    m0_lcd_end_frame();
    if (playing) {
        gbl_save_flush();
        m0_audio_stop();
    }
    meta_reset_to_bootloader();
}
#else
static void usb_poll(int playing) { (void)playing; }
#endif

static uint32_t home_since;
static uint8_t home_held, home_stage, select_frames, menu_request;
static uint8_t combo_seen; /* keys seen with HOME held (GBC palette combos) */
#if M0_TARGET_POKITTO
static uint8_t start_frames;
#endif

static void poll_input(void) {
    uint8_t m = meta_buttons();
    uint8_t gb = 0;
    if (m & M0_MB_A) gb |= 0x01;
    if (m & M0_MB_B) gb |= 0x02;
    if (m & M0_MB_MENU) gb |= 0x08; /* Start */
    if (m & M0_MB_RIGHT) gb |= 0x10;
    if (m & M0_MB_LEFT) gb |= 0x20;
    if (m & M0_MB_UP) gb |= 0x40;
    if (m & M0_MB_DOWN) gb |= 0x80;

#if META_SWAP_AB
    /* The META's A is the right-hand button: Game Boy B, and vice versa. */
    gb = (uint8_t)((gb & ~0x03u) | ((gb & 0x01) << 1) | ((gb & 0x02) >> 1));
#endif

    uint32_t now = millis();
#if M0_TARGET_POKITTO
    /* C: a tap is Start, held half a second (alone) the option menu, with
     * B Select (B itself is then not pressed).  Any other key while C is
     * held is the GBC's power-on palette combo instead: it picks one of
     * the boot ROM's generic palettes live and keeps the menu off — C
     * stays exclusive. */
    if (m & M0_MB_HOME) {
        if (!home_held) {
            home_held = 1;
            home_stage = 0;
            home_since = now;
        }
        if (home_stage != 1 && (m & M0_MB_B)) home_stage = 2;
        if (home_stage == 2) {
            gb = (uint8_t)((gb & ~0x02u) | 0x04);
        } else if (home_stage == 0 && now - home_since >= 500) {
            home_stage = 1;
            menu_request = 1;
        } else if (home_stage == 0 && (m & ~M0_MB_HOME)) {
            home_stage = 3;
        }
        if (home_stage == 3) {
            gb = 0;
            uint8_t combo = m & ~M0_MB_HOME;
            if (combo != combo_seen) {
                combo_seen = combo;
                const uint32_t *pal = gbl_palette_boot(m);
                if (pal) gbl_display_set_palette_colors(pal);
            }
        }
    } else if (home_held) {
        home_held = 0;
        combo_seen = 0;
        if (home_stage == 0) start_frames = 6; /* a tap becomes a Start press */
    }
    if (start_frames) { gb |= 0x08; start_frames--; }
#else
    /* HOME: a tap is Select, held half a second (alone) the option menu.
     * Any other key while HOME is held is the GBC's power-on palette
     * combo instead: it picks one of the boot ROM's generic palettes live
     * and keeps the menu off — HOME stays exclusive. */
    if (m & M0_MB_HOME) {
        if (!home_held) {
            home_held = 1;
            home_stage = 0;
            home_since = now;
        } else if (home_stage == 0 && now - home_since >= 500) {
            home_stage = 1; /* no Select tap on release */
            menu_request = 1;
        }
        if (home_stage == 0 && (m & ~M0_MB_HOME)) home_stage = 3;
        if (home_stage == 3) {
            gb = 0;
            uint8_t combo = m & ~M0_MB_HOME;
            if (combo != combo_seen) {
                combo_seen = combo;
                const uint32_t *pal = gbl_palette_boot(m);
                if (pal) gbl_display_set_palette_colors(pal);
            }
        }
    } else if (home_held) {
        home_held = 0;
        combo_seen = 0;
        if (home_stage == 0) select_frames = 6; /* a tap becomes a Select press */
    }
#endif
    if (select_frames) { gb |= 0x04; select_frames--; }
    gbl_set_buttons(gb);
}

/* ---- pacing, frame skip and options --------------------------------------- */

/* Frame skip of the option menu: automatic, none, or one frame drawn in
 * 2, 3 or 4. */
enum { FSKIP_AUTO, FSKIP_NONE, FSKIP_HALF, FSKIP_THIRD, FSKIP_QUARTER, FSKIP_MODES };
static uint8_t skip_mode = FSKIP_AUTO;
static uint8_t fast_mode; /* menu "Vitesse": no frame limit */
static uint16_t last_frames, last_shown; /* previous second, for the menu */
static jmp_buf restart_point;             /* menu "Redemarrer", loaded states */
#ifdef GBL_STATE_SLOTS
#define GBL_STATE_SLOTS_OR_1 GBL_STATE_SLOTS
#else
#define GBL_STATE_SLOTS_OR_1 1
#endif

static uint32_t stat_start_ms;
static uint16_t frames, shown, skipped_run;

/* Time owed to the emulation, like Gdl's rate: elapsed time comes in, each
 * emulated frame takes FRAME_US out.  Below zero the frame was early and
 * pacing waits; a frame or more ahead, the automatic frame skip renders
 * nothing until it has caught up. */
static uint32_t pace_last;
static int32_t pace_owed;

static void pace_reset(void) {
    pace_last = m0_micros();
    pace_owed = 0;
}

static void pace_frame(void) {
    uint32_t now = m0_micros();
    if (fast_mode) {
        pace_last = now;
        pace_owed = 0;
        return;
    }
    pace_owed += (int32_t)(now - pace_last);
    pace_last = now;
    pace_owed -= (int32_t)FRAME_US;
    if (pace_owed < 0) {
        uint32_t until = now + (uint32_t)-pace_owed;
        while ((int32_t)(m0_micros() - until) < 0) m0_audio_idle();
        now = m0_micros();
        pace_owed += (int32_t)(now - pace_last);
        pace_last = now;
    }
    /* Long stall (LCD off loops, USB, the menu), or no catching up wanted:
     * forget the debt. */
    if (pace_owed > 200000 || (skip_mode != FSKIP_AUTO && pace_owed > (int32_t)FRAME_US))
        pace_owed = (int32_t)FRAME_US;
}

#if M0_SCALED_MODES
#if defined(GBL_TARGET_PC) && GBL_TARGET_PC
/* the PC plays the DMG's own 160x144, no scaling: the menu carries no
 * screen entry and gbl_display_set_mode refuses every other mode */
static const char *const mode_names[1] = {"NATIVE"};
#define SCREEN_MODES 1
#else
static const char *const mode_names[META_MODE_COUNT] = {"SMOOTH", "TILE", "BLEND", "DARK",
                                                        "CROP", "SKIP", "TBLEND"};
#define SCREEN_MODES META_MODE_COUNT
#endif
#else /* Pokitto: unscaled, gm0's 200x176, or 200x176 with every line */
static const char *const mode_names[POKITTO_MODE_COUNT] = {"1:1", "200x176", "200x176 entier"};
#define SCREEN_MODES POKITTO_MODE_COUNT
#endif
static const char *const skip_names[FSKIP_MODES] = {"Auto", "Aucun", "1 sur 2", "1 sur 3", "1 sur 4"};
static const char *const speed_names[2] = {"Normale", "Rapide"};
#if M0_TARGET_ESPBOY
static const char *const clock_names[2] = {"80 MHz", "160 MHz"};
#elif M0_TARGET_POKITTO
static const char *const clock_names[2] = {"48 MHz", "72 MHz"};
#elif M0_TARGET_PC
static const char *const clock_names[2] = {"-", "-"}; /* one rate: no line */
#elif M0_TARGET_ESP32
static const char *const clock_names[2] = {"80 MHz", "240 MHz"};
#else
static const char *const clock_names[2] = {"48 MHz", "70 MHz"}; /* meta_clock.cpp: 72 asked, 70 run */
#endif
#if !M0_TARGET_POKITTO && !M0_TARGET_ESPBOY
#endif

static char *put_uint(char *p, unsigned v) {
    char tmp[10];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10u); while ((v /= 10u) && n < 10);
    while (n) *p++ = tmp[--n];
    return p;
}

/* Save states: the menu picks the slot and the operation, which runs at
 * the next instruction boundary (gbl_platform_safe_point). */
enum { STATE_NONE, STATE_SAVE, STATE_LOAD };
static uint8_t state_slot, state_op;
static const char *state_msg; /* result, shown once by the next menu */
static char slot_text[GBL_STATE_SLOTS_OR_1][8];
static const char *slot_names[GBL_STATE_SLOTS_OR_1];
static const char *const volume_names[9] = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};

#if defined(GBL_ROM_STREAM) && GBL_ROM_STREAM && (GBL_CACHE_UNIFIED ? GBL_STREAM_SECTORS : GBL_STREAM_FIXED) >= 2 && \
    defined(__ARM_ARCH_6M__)
/* The option menu needs about 600 bytes of stack, and VBlank can come deep
 * in the game's calls (always, in Super Mario Land 2): it runs on the ROM
 * cache's buffers instead (the fixed part with GBL_CACHE_DYN, away from
 * the blocks lent to the stack), emptied afterwards (a few sectors read
 * again).
 * It reads no ROM; its interrupts use that stack too.  "Redemarrer" and
 * "Quitter" leave it by longjmp or reset. */
__attribute__((naked, noinline)) static void call_on_stack(void (*fn)(void), uintptr_t top) {
    __asm__ volatile("push {r4, lr}\n\t"
                     "mov r4, sp\n\t"
                     "mov sp, r1\n\t"
                     "blx r0\n\t"
                     "mov sp, r4\n\t"
                     "pop {r4, pc}\n\t");
}

static void option_menu(void);
static void run_menu(void) {
#if GBL_CACHE_UNIFIED
    uintptr_t top = gbl_cache_lend(); /* written back, kept out until gbl_stream_invalidate */
#else
    uintptr_t top = ((uintptr_t)gbl_stream_scratch() + GBL_STREAM_FIXED * 512u) & ~(uintptr_t)7;
#endif
    call_on_stack(option_menu, top);
    gbl_stream_invalidate();
}
#else
#define run_menu option_menu
#endif

#if M0_TARGET_POKITTO
extern "C" char __bss_end__;
#define DATA_END __bss_end__
#elif defined(__MINGW32__)
/* the PE linker script provides `end` from a discarded section; any stable
 * address does here — the PC bounds its own process stack with it */
static char mingw_data_end;
#define DATA_END mingw_data_end
#else
extern "C" char end; /* after .bss and .noinit (gamebuino_meta.ld) */
#define DATA_END end
#endif

#if GBL_CACHE_DYN
/* The cache's blocks after its fixed part and the C stack share the RAM
 * above .bss (m0_stack_pool.h); main fills this in. */
static m0_stack_pool stack_pool;

/* gbl_exec entered with the stack high enough (gbl_pool_sp): the blocks
 * the cache wants back. */
extern "C" void gbl_platform_pool(void) { m0_stack_pool_run(&stack_pool); }
#endif

/* Stack high-water mark: main paints the free RAM below its frame with
 * STACK_PAINT; the menu shows how much of it was never written since
 * (with GBL_CACHE_DYN, above the cache's last block). */
#define STACK_PAINT 0xC5AC5AC5u
static unsigned stack_never_used(void) {
#if M0_TARGET_ESPBOY
    return espboy_free_stack();
#elif M0_TARGET_PC
    return 0; /* the process stack is the machine's; the count means nothing */
#elif M0_TARGET_ESP32
    return 0; /* the Arduino task's stack; not painted */
#else
#if GBL_CACHE_DYN
    const uint32_t *p = (const uint32_t *)((gbl_cache_top() + 3u) & ~(uintptr_t)3u);
#else
    const uint32_t *p = (const uint32_t *)(((uintptr_t)&DATA_END + 3u) & ~(uintptr_t)3u);
#endif
    unsigned n = 0;
    while (p[n] == STACK_PAINT) n++;
    return n * 4u;
#endif
}

static void name_slots(void) {
    uint16_t used = m0_state_used();
    for (unsigned i = 0; i < GBL_STATE_SLOTS_OR_1; i++) {
        char *p = put_uint(slot_text[i], i + 1u);
        if (!(used >> i & 1u)) memcpy(p, " vide", 6);
        else *p = 0;
        slot_names[i] = slot_text[i];
    }
}

enum { M_RESUME, M_RESTART, M_SLOT, M_SAVE, M_LOAD, M_VOLUME, M_AUDIO, M_SCREEN, M_PALETTE, M_SKIP,
       M_SPEED, M_CLOCK, M_LCD_SPI, M_USB_SD, M_BOOTLOADER, M_QUIT };

static void option_menu(void) {
    uint8_t mode = gbl_display_mode(), pal = gbl_display_palette(), skip = skip_mode;
    uint8_t fast = fast_mode, mhz = m0_clock_mhz() == (M0_TARGET_ESPBOY ? 160 : M0_TARGET_ESP32 ? 240 : 72), vol = m0_audio_volume();
    uint8_t slot = state_slot;
    meta_menu_item items[14];
    uint8_t ids[14];
    unsigned n = 0;
#define ADD(id, label, type, value, count, names) \
    (ids[n] = (id), items[n++] = (meta_menu_item){label, type, value, count, names})
    ADD(M_RESUME, "Reprendre", META_MENU_ACTION, 0, 0, 0);
    ADD(M_RESTART, "Redemarrer", META_MENU_ACTION, 0, 0, 0);
    if (m0_state_available()) {
        name_slots();
        ADD(M_SLOT, "Etat", META_MENU_CHOICE, &slot, GBL_STATE_SLOTS_OR_1, slot_names);
        ADD(M_SAVE, "Sauver l'etat", META_MENU_ACTION, 0, 0, 0);
        ADD(M_LOAD, "Charger l'etat", META_MENU_ACTION, 0, 0, 0);
    }
    ADD(M_VOLUME, "Volume", META_MENU_CHOICE, &vol, 9, volume_names);
#if !(defined(GBL_TARGET_PC) && GBL_TARGET_PC)
    ADD(M_SCREEN, "Ecran", META_MENU_CHOICE, &mode, SCREEN_MODES, mode_names);
#endif
    ADD(M_PALETTE, "Palette", META_MENU_CHOICE, &pal, META_PALETTE_COUNT, gbl_palette_names);
    ADD(M_SKIP, "Images", META_MENU_CHOICE, &skip, FSKIP_MODES, skip_names);
    ADD(M_SPEED, "Vitesse", META_MENU_CHOICE, &fast, 2, speed_names);
#if !M0_TARGET_PC /* one rate on the PC: no "Processeur" line */
    ADD(M_CLOCK, "Processeur", META_MENU_CHOICE, &mhz, 2, clock_names);
#endif
#if M0_TARGET_META
#if META_USB_SD
    ADD(M_USB_SD, "Carte SD USB", META_MENU_ACTION, 0, 0, 0);
#endif
    ADD(M_BOOTLOADER, "Bootloader USB", META_MENU_ACTION, 0, 0, 0);
#endif
    ADD(M_QUIT, "Quitter", META_MENU_ACTION, 0, 0, 0);
#undef ADD
    /* "59/41 ips, pile 812": emulated and drawn frames of the last second
     * and the stack never used so far (bytes above the end of .bss), or the
     * result of the last save state operation. */
    char info[32];
    if (state_msg) {
        strcpy(info, state_msg);
        state_msg = 0;
    } else {
        char *p = put_uint(info, last_frames);
        *p++ = '/';
        p = put_uint(p, last_shown);
#if GBL_CACHE_DYN
        /* "59/41 pile 812 c36": and the cache's blocks, in the 20 columns */
        memcpy(p, " pile ", 6);
        p = put_uint(p + 6, stack_never_used());
        memcpy(p, " c", 2);
        p = put_uint(p + 2, gbl_cache_ways());
#else
        memcpy(p, " ips, pile ", 11);
        p = put_uint(p + 11, stack_never_used());
#endif
        *p = 0;
    }
#if M0_TARGET_POKITTO
    m0_lcd_fill(0); /* the menu covers the 160x144 area only */
#endif
    m0_audio_pause(1); /* the game's sound stops while the menu is open */
    /* the META's 16 bands take a title, fourteen lines and the hint: the
     * info line only when the menu is shorter */
    int line = m0_menu_run(GBL_TITLE, items, n, meta_buttons, n > 13 ? 0 : info);
    m0_audio_pause(0);
    int action = line >= 0 ? ids[line] : -1;
#if M0_TARGET_META
#if META_USB_SD
    if (action == M_USB_SD) {
        m0_lcd_end_frame();
        gbl_save_flush();
        m0_audio_stop();
        NVIC_DisableIRQ(DMAC_IRQn);
        m0_watchdog_stop();
        unsigned rows;
        uint8_t *sector = (uint8_t *)gbl_display_scratch(&rows);
        meta_usb_msc_enter(sector); /* full stack; game stays stopped until reset */
    }
#endif
    if (action == M_BOOTLOADER) {
        /* The USB bootloader (sam-ba) directly: its magic where the
         * bootloader looks after a software reset — with or without
         * META_USB (the double reset reaches the loader, not sam-ba). */
        m0_lcd_end_frame();
        gbl_save_flush();
        m0_audio_stop();
        NVIC_DisableIRQ(DMAC_IRQn);
        m0_watchdog_stop();
#if META_USB_SERIAL
        meta_usb_stop();
#endif
        m0_sd_deselect();
        *(volatile uint32_t *)0x20007FFC = 0x3aecfd88u;
        NVIC_SystemReset();
    }
#endif
    if (action == M_QUIT) enter_loader();
    skip_mode = skip;
    fast_mode = fast;
    state_slot = slot;
    m0_audio_set_volume(vol);
    m0_lcd_end_frame(); /* no transfer while the clocks change */
    m0_clock_set(M0_TARGET_ESPBOY ? (mhz ? 160 : 80) : M0_TARGET_ESP32 ? (mhz ? 240 : 80) : (mhz ? 72 : 48));
    if (pal != gbl_display_palette()) gbl_display_set_palette(pal);
    gbl_display_set_mode(mode); /* also redraws the whole picture */
    skipped_run = 0;
    pace_reset();
    if (action == M_SAVE || action == M_LOAD) {
        state_op = action == M_SAVE ? STATE_SAVE : STATE_LOAD;
        gbl_safe_request = 1;
        gbr.next_evt = gbr.cyc; /* reach an interrupt check soon */
    }
    if (action == M_RESTART) {
        gbl_save_flush(); /* gbl_reset reads the cartridge RAM back */
        longjmp(restart_point, 1);
    }
}

/* Between two instructions (gbl_irq_enter): run the menu's save state
 * operation.  The strip buffers serve as the sector buffer. */
extern "C" void gbl_platform_safe_point(void) {
    uint8_t op = state_op;
    state_op = STATE_NONE;
    if (op == STATE_NONE) return;
    unsigned rows;
    uint8_t *buf = (uint8_t *)gbl_display_scratch(&rows);
    m0_lcd_wait();
    m0_lcd_end_frame();
    if (op == STATE_SAVE) {
        state_msg = m0_state_save(state_slot, buf) ? "Etat sauve" : "Echec sauvegarde";
        return;
    }
    if (!m0_state_load(state_slot, buf)) {
        state_msg = "Echec chargement";
        return;
    }
    state_msg = "Etat charge";
    gbl_display_set_mode(gbl_display_mode());
    pace_reset();
    longjmp(restart_point, 2); /* the machine as loaded: start over from gbr.pc */
}

#ifndef META_DEBUG_TRACE
#define META_DEBUG_TRACE 0
#endif
#if META_DEBUG_TRACE
/* Debug: the machine at every VBlank in a ring that meta-usb.py trace reads
 * several times a second into a file.  top is the word at sp, read without
 * touching the cartridge RAM cache (0xEEEE when its sector is not mapped).
 * flags: ime, then the low 7 bits of the card's re-identifications. */
struct meta_trace_rec {
    uint32_t frame;
    uint16_t pc, sp, hl, top;
    uint8_t bank, a, flags, save_errors;
};
#define META_TRACE_LEN 16
extern "C" {
volatile uint32_t meta_trace_pos; /* records written */
meta_trace_rec meta_trace[META_TRACE_LEN];
extern uint32_t meta_sd_reinits;
}

static int trace_peek(uint16_t a) {
    if (a >= 0xC000 && a < 0xFE00) return gbl_wram[(a - 0xC000) & 0x1FFF];
    if (a >= 0xFF80 && a < 0xFFFF) return gbl_hram[a - 0xFF80];
#if GBL_ERAM_ON_SD
    if (a - 0xA000u < 0x2000u) {
        const uint8_t *e = gbl_eram_rmap[(a - 0xA000u) >> 9];
        return e ? e[a] : -1;
    }
#endif
    return -1;
}

static void trace_frame(void) {
    meta_trace_rec *r = &meta_trace[meta_trace_pos % META_TRACE_LEN];
    int lo = trace_peek(gbr.sp), hi = trace_peek((uint16_t)(gbr.sp + 1));
    r->frame = gbl_frame;
    r->pc = gbr.pc;
    r->sp = gbr.sp;
    r->hl = gbr.hl;
    r->top = lo < 0 || hi < 0 ? 0xEEEE : (uint16_t)(lo | hi << 8);
    r->bank = (uint8_t)gbr.rom_bank;
    r->a = gbr.a;
    r->flags = (uint8_t)((gbr.ime & 1u) | (meta_sd_reinits & 0x7Fu) << 1);
    r->save_errors = (uint8_t)gbl_save_errors;
    meta_trace_pos++;
}
#endif

extern "C" void gbl_platform_vblank(void) {
#if !M0_TARGET_POKITTO
    m0_watchdog_feed(); /* once per frame (meta_startup.c) */
#endif
#if META_DEBUG_TRACE
    trace_frame();
#endif
#if GBL_DEBUG_RESTART && GBL_ERAM_ON_SD
    gbl_dbg_tables_check(0);
#endif
    usb_poll(1);
    int bus_free = close_frame();
    int was_shown = !gbl_skip_render;
    if (was_shown) shown++;
    frames++;
    int static_next = META_STATIC_SKIP ? gbl_frame_unchanged_next(was_shown) : 0;
    if (bus_free) poll_input(); /* else keep last frame's buttons */
    if (menu_request) {
        menu_request = 0;
        run_menu();
        static_next = 0;
    }
    /* Audio first: a save flush can block on the SD card, and the DMA then
     * plays from a full ring (meta_audio.cpp). */
    m0_audio_frame();
    gbl_save_tick();
#if GBL_CACHE_DYN
    if (stack_pool.top) m0_stack_pool_frame(&stack_pool);
#endif
    pace_frame();

    static uint8_t phase;
    int skip = 0;
    if (skip_mode == FSKIP_AUTO)
        skip = pace_owed >= (int32_t)FRAME_US && skipped_run < META_MAX_SKIP;
    else if (skip_mode != FSKIP_NONE)
        skip = phase != 0; /* draw one frame in skip_mode (2, 3 or 4) */
    if (skip_mode >= FSKIP_HALF) phase = (uint8_t)(phase + 1 < skip_mode ? phase + 1 : 0);
    if (static_next) {
        gbl_skip_render = 1;
    } else if (skip) {
        gbl_skip_render = 1;
        skipped_run++;
    } else {
        gbl_skip_render = 0;
        skipped_run = 0;
    }

    uint32_t ms = millis();
    if (ms - stat_start_ms >= 1000) {
        gbl_interp_instrs = 0;
        last_frames = frames;
        last_shown = shown;
        frames = shown = 0;
        gbl_display_lcd_waits = gbl_display_rows_skipped = 0;
        stat_start_ms = ms;
    }
}


/* A file the game needs is missing or unusable: a coloured screen tells
 * which, HOME (Pokitto: C) held 3 s returns to the loader.  Red: ROM not
 * found, blue: card unreadable, orange: ROM fragmented, magenta: a
 * different ROM, yellow: save file missing. */
static void __attribute__((unused)) boot_error(uint16_t colour) {
    m0_lcd_fill(colour);
    uint32_t held = 0;
    for (;;) {
#if !M0_TARGET_POKITTO
        m0_watchdog_feed();
#if M0_TARGET_ESPBOY || M0_TARGET_PC || M0_TARGET_ESP32
        uint8_t m = meta_buttons();
#else
        uint8_t m = meta_pad_read();
#endif
#else
        uint8_t m = meta_buttons();
#endif
        usb_poll(0);
        uint32_t now = millis();
        if (m & 0x80) {
            if (!held) held = now;
            else if (now - held > 3000) enter_loader();
        } else {
            held = 0;
        }
        delay(20);
    }
}

#if GBL_ROM_SD
/* gbl_stream_init's errors: 1 card unreadable (FAT16/FAT32 expected), 2
 * GBL_STREAM_NAME not found (root or one folder), 3 the file is too
 * fragmented (copy it again), 4 it is not this firmware's ROM. */
static void stream_error_screen(int err) {
    static const uint16_t colour[] = M0_CARD_COLOURS(M0_COLOR);
    boot_error(colour[err]);
}
#endif

#if M0_TARGET_ESPBOY
int espboy_main(void) {
#elif M0_TARGET_ESP32
extern "C" int m0boy_console_main(void); /* the console file's setup() calls it */
int m0boy_console_main(void) {
#else
int main(void) {
#endif
#if !M0_TARGET_ESPBOY && !M0_TARGET_PC && !M0_TARGET_ESP32
    { /* stack_never_used: paint the free RAM, 128 bytes below this frame */
        char here;
        uint32_t *p = (uint32_t *)(((uintptr_t)&DATA_END + 3u) & ~(uintptr_t)3u);
        uint32_t *top = (uint32_t *)((uintptr_t)&here - 128u);
        while (p < top) *p++ = STACK_PAINT;
    }
#endif
#if !M0_TARGET_POKITTO
    m0_sd_deselect(); /* the card off the shared SPI bus from the start */
#if META_USB_SERIAL
    meta_usb_init();
#endif
#else
    pokitto_buttons_init();
#endif
    m0_lcd_init();
    /* Like the Gamebuino library's SD.begin at boot: the loader relies on
     * the card being in SPI mode when HOME brings it back. */
    int card_ok = m0_sd_init();
#if M0_TARGET_META
    /* The game's files (ROM, save, states) are the ones in the folder of
     * its own .BIN: other games' folders may hold files of the same name. */
    if (card_ok) {
        unsigned rows;
        m0_fat_dir = m0_fat_game_dir(m0_sd_read, (uint8_t *)gbl_display_scratch(&rows), GBL_BIN_NAME83,
                                         META_APP_START);
    }
#endif
    __asm__ volatile("" ::"r"(gbl_gen_version)); /* kept in the binary for compile.sh */
#if GBL_ROM_SD
    /* Before gbl_reset: the lookup borrows the stream cache. */
    int stream_err = gbl_stream_init(card_ok);
    if (stream_err) stream_error_screen(stream_err);
#endif
    /* Also before gbl_reset, which loads the saved cartridge RAM. */
    int save_state = gbl_save_init(card_ok);
    (void)save_state; /* only checked when the cartridge RAM is on the card */
    {
        unsigned rows;
        m0_state_init(card_ok, (uint8_t *)gbl_display_scratch(&rows));
    }
#if GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD
    if (save_state == META_SAVE_MISSING) /* GBL_SAVE_NAME not next to the ROM (compile.sh makes it) */
        boot_error(M0_COLOR(M0_CARD_RGB_NO_SAVE));
#endif
#if M0_TARGET_POKITTO
    m0_lcd_fill(0); /* black around the picture */
#else
    m0_lcd_fill(gbl_dmg_palette[0]);
#endif
    m0_clock_init();
#if !M0_TARGET_POKITTO && M0_CLOCK_DEFAULT != 48
    m0_clock_set(M0_CLOCK_DEFAULT); /* META_OVERCLOCK */
#endif
    m0_audio_init();

#if M0_SCALED_MODES
#if defined(GBL_TARGET_PC) && GBL_TARGET_PC
    gbl_display_set_mode(META_MODE_NATIVE); /* the PC window plays 160x144 */
#else
    gbl_display_set_mode(META_MODE_TBLEND); /* TILE without META_TBLEND */
#endif
#else
    gbl_display_set_mode(0);
#endif
    /* Palette at power-on, like the GBC boot ROM: a direction held, plus
     * A and/or B, picks one of its 12 generic palettes; without one, Vert.
     * The game's own GBC palette (GBL_PALETTE_GBC) is a menu choice. */
    const uint32_t *boot_palette = gbl_palette_boot(meta_buttons());
    if (boot_palette) gbl_display_set_palette_colors(boot_palette);
    else gbl_display_set_palette(0);
    /* Keep generated code within a fixed C stack budget; deeper nesting
     * unwinds to the nearest dispatcher loop. */
    char probe;
    uintptr_t limit = (uintptr_t)&probe - (M0_TARGET_ESPBOY ? 2048u : 3072u);
    #if M0_TARGET_ESPBOY
    uintptr_t floor_addr = espboy_stack_floor(); /* the continuation stack of setup() */
#else
    uintptr_t floor_addr = (uintptr_t)&DATA_END + META_STACK_RESERVE; /* gbl_profile.h */
#endif
    gbl_stack_limit = limit > floor_addr ? limit : floor_addr;
#if GBL_CACHE_DYN
    /* The cache's fixed part ends .bss (.cachepool, last before DATA_END):
     * its other blocks start there, as many as GBL_STREAM_SECTORS /
     * GBL_CACHE_BYTES asked while the stack keeps META_STACK_DYN_MIN.  Not
     * there (another linker script): the fixed part alone, as before. */
#ifndef META_STACK_DYN_MIN
#define META_STACK_DYN_MIN 1024u /* gbl_run's headroom and its own frames */
#endif
    if (gbl_cache_top() == (uintptr_t)&DATA_END) {
        stack_pool.limit = &gbl_stack_limit;
        stack_pool.ceil = (uintptr_t)&probe - META_STACK_DYN_MIN;
        stack_pool.top = gbl_cache_top;
        stack_pool.grow = gbl_cache_grow;
        stack_pool.shrink = gbl_cache_shrink;
        stack_pool.want_sp = &gbl_pool_sp;
        stack_pool.refused = &gbl_exec_refused;
        stack_pool.loads = &gbl_cache_loads;
        stack_pool.block = GBL_CACHE_DYN_BLOCK;
        stack_pool.reserve = META_STACK_RESERVE;
        stack_pool.headroom = GBL_EXEC_HEADROOM;
        m0_stack_pool_boot(&stack_pool, GBL_CACHE_DYN_TARGET - gbl_cache_ways());
    }
#endif
    /* Too little RAM left (caches, debug records): the unguarded calls
     * below the limit would run into .bss.  Cyan screen instead. */
#ifndef META_STACK_NEST_MIN
#define META_STACK_NEST_MIN 256
#endif
    if ((uintptr_t)&probe < floor_addr + META_STACK_NEST_MIN) boot_error(M0_COLOR(0x00FFFF));

    /* The menu's "Redemarrer" comes back here (from inside the emulation:
     * longjmp drops its C frames). */
#if defined(GBL_TARGET_PC) && GBL_TARGET_PC
    /* memscan --export-playable writes its snapshot as slot 0 of the .STA:
     * M0_STATE=<slot> boots straight into it — the first safe point performs
     * the load, execution then restarts from the state's gbr.pc.  After
     * gbl_reset: the request survives it, the early event check does not. */
    {
        const char *env = getenv("M0_STATE");
        if (env && m0_state_used()) {
            unsigned s = (unsigned)atoi(env);
            if (s < GBL_STATE_SLOTS_OR_1 && (m0_state_used() >> s & 1u)) {
                state_slot = (uint8_t)s;
                state_op = STATE_LOAD;
                gbl_safe_request = 1;
                gbr.next_evt = gbr.cyc; /* reach an instruction check now */
            }
        }
    }
#endif
    int resumed = setjmp(restart_point);
#if GBL_CACHE_UNIFIED
    gbl_stream_invalidate(); /* the menu's blocks, when it left by longjmp */
#endif
    if (resumed != 2) gbl_reset(); /* 2: a state was loaded */
    gbl_skip_render = 0;
    pace_reset();
    stat_start_ms = millis();
    gbl_run();
    for (;;) {}
}
