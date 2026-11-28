#ifndef M0BOY_GP32_MENU_H
#define M0BOY_GP32_MENU_H

#include "s3c2400.h"

/* The option menu of the m0boy frontends (sg_menu.h), for the GP32: SELECT
   held half a second alone opens it, a shorter press stays the game's. */

#define GP32_MENU_WIDTH 160u
#define GP32_MENU_HEIGHT 120u
#define GP32_MENU_SLOTS 4u

typedef enum {
    GP32_MENU_STAY, GP32_MENU_RESUME, GP32_MENU_RESTART, GP32_MENU_SAVE,
    GP32_MENU_LOAD, GP32_MENU_QUIT
} Gp32MenuAction;

/* Zero initialize, then set `volume` (0 to 8). */
typedef struct {
    bool open, changed; /* changed: the picture must be rendered again */
    uint8_t slot, volume, fast, fullscreen;
    unsigned used;      /* bit per slot holding a state */
    const char *info;   /* optional line under the items */
    /* private */
    unsigned line;
    uint16_t held;
    uint32_t select_since, pulse_until;
    uint8_t select_stage;
    bool select_down, release;
} Gp32Menu;

void gp32_menu_open(Gp32Menu *menu, uint16_t buttons);
/* While open: GP32_* buttons in, the chosen action out. The menu is closed
   on every action. */
Gp32MenuAction gp32_menu_update(Gp32Menu *menu, uint16_t buttons);
/* While closed: returns the buttons the game sees, and opens the menu when
   SELECT was held. */
uint16_t gp32_menu_filter(Gp32Menu *menu, uint16_t buttons, uint32_t milliseconds);
/* RGB565, rows from the top. */
void gp32_menu_render(const Gp32Menu *menu, uint16_t pixels[GP32_MENU_WIDTH * GP32_MENU_HEIGHT]);

#endif
