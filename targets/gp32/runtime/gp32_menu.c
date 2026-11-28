#include "gp32_menu.h"

#include "m0_font.h"
#include <stdio.h>
#include <string.h>

enum {
    LINE_RESUME, LINE_RESTART, LINE_SLOT, LINE_SAVE, LINE_LOAD, LINE_VOLUME,
    LINE_SPEED, LINE_SCREEN, LINE_QUIT, LINES
};

enum { SELECT_PENDING, SELECT_OPENED, SELECT_PASSED };

enum { HOLD_MS = 500, TAP_MS = 100 };

void gp32_menu_open(Gp32Menu *menu, uint16_t buttons)
{
    menu->open = menu->changed = true;
    menu->line = LINE_RESUME;
    menu->held = buttons; /* ignore what is already down */
    menu->select_down = false;
    menu->pulse_until = 0;
}

static Gp32MenuAction close_menu(Gp32Menu *menu, Gp32MenuAction action)
{
    menu->open = false;
    menu->release = true; /* the game must not see the keys of the menu */
    menu->info = NULL;
    return action;
}

Gp32MenuAction gp32_menu_update(Gp32Menu *menu, uint16_t buttons)
{
    uint16_t pressed = buttons & (uint16_t)~menu->held;
    menu->held = buttons;
    if (!pressed)
        return GP32_MENU_STAY;
    if (pressed & (GP32_B | GP32_SELECT))
        return close_menu(menu, GP32_MENU_RESUME);
    menu->changed = true;
    if (pressed & GP32_UP)
        menu->line = menu->line ? menu->line - 1 : LINES - 1;
    if (pressed & GP32_DOWN)
        menu->line = menu->line + 1 < LINES ? menu->line + 1 : 0;
    uint8_t *value = menu->line == LINE_SLOT ? &menu->slot
                   : menu->line == LINE_VOLUME ? &menu->volume
                   : menu->line == LINE_SPEED ? &menu->fast
                   : menu->line == LINE_SCREEN ? &menu->fullscreen : NULL;
    bool confirm = pressed & (GP32_A | GP32_START);
    if (value) {
        unsigned count = menu->line == LINE_SLOT ? GP32_MENU_SLOTS
                       : menu->line == LINE_VOLUME ? 9u : 2u;
        if ((pressed & GP32_LEFT) && *value)
            --*value;
        if ((pressed & GP32_RIGHT) && *value + 1u < count)
            ++*value;
        if (confirm)
            *value = (uint8_t)((*value + 1u) % count);
    } else if (confirm) {
        static const Gp32MenuAction actions[LINES] = {
            [LINE_RESUME] = GP32_MENU_RESUME, [LINE_RESTART] = GP32_MENU_RESTART,
            [LINE_SAVE] = GP32_MENU_SAVE, [LINE_LOAD] = GP32_MENU_LOAD,
            [LINE_QUIT] = GP32_MENU_QUIT};
        return close_menu(menu, actions[menu->line]);
    }
    return GP32_MENU_STAY;
}

uint16_t gp32_menu_filter(Gp32Menu *menu, uint16_t buttons, uint32_t milliseconds)
{
    if (menu->release) {
        if (buttons)
            return 0;
        menu->release = false;
    }
    uint16_t others = buttons & (uint16_t)~GP32_SELECT;
    if (buttons & GP32_SELECT) {
        if (!menu->select_down) {
            menu->select_down = true;
            menu->select_stage = SELECT_PENDING;
            menu->select_since = milliseconds;
        }
        if (menu->select_stage == SELECT_PENDING) {
            if (others) {
                menu->select_stage = SELECT_PASSED;
            } else if (milliseconds - menu->select_since >= HOLD_MS) {
                menu->select_stage = SELECT_OPENED;
                gp32_menu_open(menu, buttons);
                return 0;
            }
        }
        return menu->select_stage == SELECT_PASSED ? buttons : others;
    }
    if (menu->select_down) {
        menu->select_down = false;
        if (menu->select_stage == SELECT_PENDING)
            menu->pulse_until = milliseconds + TAP_MS; /* the game's press */
    }
    if (menu->pulse_until) {
        if ((int32_t)(menu->pulse_until - milliseconds) > 0)
            buttons |= GP32_SELECT;
        else
            menu->pulse_until = 0;
    }
    return buttons;
}

static uint16_t rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xf800u) | ((rgb >> 5) & 0x07e0u) | ((rgb >> 3) & 0x001fu));
}

static int text_width(const char *text)
{
    int width = 0;
    for (; *text; ++text) {
        unsigned c = (uint8_t)*text - M0_FONT_FIRST;
        if (c < 96 && m0_font_map[c] != 0xff)
            width += m0_font_advance[m0_font_map[c]];
        else
            width += 4;
    }
    return width;
}

static void draw_text(uint16_t *band, int x, const char *text)
{
    for (; *text; ++text) {
        unsigned c = (uint8_t)*text - M0_FONT_FIRST;
        if (c >= 96 || m0_font_map[c] == 0xff) {
            x += 4; /* not in the font */
            continue;
        }
        unsigned glyph = m0_font_map[c], width = m0_font_w[glyph], stride = (width + 3) / 4;
        for (unsigned y = 0; y < M0_FONT_HEIGHT; ++y) {
            const uint8_t *row = m0_font_rows + m0_font_off[glyph] + y * stride;
            for (unsigned i = 0; i < width && x + (int)i < (int)GP32_MENU_WIDTH; ++i) {
                unsigned v = (row[i >> 2] >> (6 - 2 * (i & 3))) & 3;
                if (v)
                    band[y * GP32_MENU_WIDTH + x + i] = rgb565(m0_font_colours[v - 1]);
            }
        }
        x += m0_font_advance[glyph];
    }
}

static void draw_centered(uint16_t *band, const char *text)
{
    int x = ((int)GP32_MENU_WIDTH - text_width(text)) / 2;
    draw_text(band, x > 0 ? x : 0, text);
}

void gp32_menu_render(const Gp32Menu *menu, uint16_t pixels[GP32_MENU_WIDTH * GP32_MENU_HEIGHT])
{
    static const char *const labels[LINES] = {
        "Reprendre", "Redemarrer", "Etat", "Sauver l'etat", "Charger l'etat",
        "Volume", "Vitesse", "Ecran", "Quitter"};
    static const char *const speeds[2] = {"Normale", "Rapide"};
    static const char *const screens[2] = {"Fenetre", "Plein"};
    /* Bands of 8 rows: the title, one per item, then the information and the
       hint at the bottom. */
    const unsigned bands = GP32_MENU_HEIGHT / 8;
    for (unsigned b = 0; b < bands; ++b) {
        uint16_t *band = pixels + b * 8 * GP32_MENU_WIDTH;
        int line = b >= 1 && b <= LINES ? (int)b - 1 : -1;
        uint16_t fill = rgb565(b == 0 ? 0x346856
                               : line == (int)menu->line ? 0x24524A : 0x081820);
        for (unsigned i = 0; i < 8 * GP32_MENU_WIDTH; ++i)
            band[i] = fill;
        if (b == 0)
            draw_centered(band, "Options");
        if (b == bands - 2 && menu->info)
            draw_centered(band, menu->info);
        if (b == bands - 1)
            draw_text(band, 4, "A:ok   B:retour");
        if (line < 0)
            continue;
        draw_text(band, 4, labels[line]);
        char slot[16];
        const char *value = NULL;
        if (line == LINE_SLOT) {
            snprintf(slot, sizeof(slot), "%u%s", menu->slot + 1u,
                     menu->used >> menu->slot & 1u ? "" : " vide");
            value = slot;
        } else if (line == LINE_VOLUME) {
            snprintf(slot, sizeof(slot), "%u", menu->volume);
            value = slot;
        } else if (line == LINE_SPEED) {
            value = speeds[menu->fast & 1];
        } else if (line == LINE_SCREEN) {
            value = screens[menu->fullscreen & 1];
        }
        if (value) {
            int width = text_width(value);
            if (line == (int)menu->line) {
                draw_text(band, (int)GP32_MENU_WIDTH - 4 - width - 16, "<");
                draw_text(band, (int)GP32_MENU_WIDTH - 4 - 8, ">");
            }
            draw_text(band, (int)GP32_MENU_WIDTH - 4 - 8 - 4 - width, value);
        }
    }
}
