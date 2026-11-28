/* Drawing, after femto8's p8_lua_helper.h (same pixels), on a screen that is
 * 8 KiB of RAM and a cartridge image that stays in flash. */
#include "p8gfx.h"
#include "p8rt.h"
#include <string.h>

P8Gfx p8g;
uint8_t p8_screen[(P8_W * P8_H) / 2] __attribute__((aligned(4)));
uint16_t p8_buttons, p8_btnp;
uint32_t p8_frames;
unsigned p8_fps = 30;

/* 256 bits: the sprite sheet's 8x8 cells with no colour 0 pixel.  With the
 * default draw palette those cells are fully opaque, so a row can be copied
 * as one word instead of four byte merges.  The sheet is constant; scan it
 * when the graphics state resets instead of testing every row again. */
static uint8_t sprite_opaque[32];

static inline int nibbles_nonzero(uint32_t w) {
    const uint32_t lo = w & 0x0f0f0f0fu, hi = (w >> 4) & 0x0f0f0f0fu;
    /* each nibble is 0..15: adding 15 sets bit 4 of that byte exactly when it
     * was not zero (the sums do not carry into the next nibble) */
    return ((lo + 0x0f0f0f0fu) & 0x10101010u) == 0x10101010u &&
           ((hi + 0x0f0f0f0fu) & 0x10101010u) == 0x10101010u;
}

static void scan_sprite_opaque(void) {
    memset(sprite_opaque, 0, sizeof sprite_opaque);
    for (int n = 0; n < 256; n++) {
        const uint8_t *s = &p8_cart[((n >> 4) & 15) * 8 * 64 + (n & 15) * 4];
        int opaque = 1;
        for (int y = 0; y < 8 && opaque; y++)
            opaque = nibbles_nonzero(p8_rom_u32(s + y * 64));
        if (opaque) sprite_opaque[n >> 3] |= (uint8_t)(1u << (n & 7));
    }
}

static inline int sprite_is_opaque(int n) {
    n &= 255;
    return (sprite_opaque[n >> 3] >> (n & 7)) & 1;
}

/* the same fill for a run that may start at any byte: align, then words */
static void fill_pair_run(uint8_t *p, unsigned n, uint8_t pair) {
    const uint32_t v = (uint32_t)pair * 0x01010101u;
    while (n && ((uintptr_t)p & 3u)) { *p++ = pair; n--; }
    unsigned words = n >> 2;
    uint32_t *w = (uint32_t *)p;
    while (words >= 4) { w[0] = v; w[1] = v; w[2] = v; w[3] = v; w += 4; words -= 4; }
    while (words--) *w++ = v;
    p = (uint8_t *)w;
    n &= 3;
    while (n--) *p++ = pair;
}

/* aligned, whole screen or whole rows */
static inline void fill_pairs(uint8_t *p, unsigned bytes, uint8_t pair) {
    fill_pair_run(p, bytes, pair);
}

const uint32_t p8_palette_rgb[16] = {
    0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
    0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA,
};

void p8_gfx_reset(void) {
    memset(&p8g, 0, sizeof p8g);
    p8g.pen = 6;
    for (int i = 0; i < 16; i++) {
        p8g.pal_draw[i] = (uint8_t)(i == 0 ? 0x10 : i);
        p8g.pal_screen[i] = (uint8_t)i;
    }
    p8g.clip_x1 = P8_W;
    p8g.clip_y1 = P8_H;
    p8_gfx_pal_changed();
    scan_sprite_opaque();
}

void p8_gfx_pal_changed(void) {
    unsigned simple = p8g.pal_draw[0] == 0x10;
    for (unsigned i = 1; i < 16; i++) simple &= p8g.pal_draw[i] == i;
    p8g.pal_simple = (uint8_t)simple;
}

/* ---- pixels ---- */
static inline void put(int x, int y, unsigned col) { /* screen coordinates, colour 0-15 */
    uint8_t *b = &p8_screen[y * 64 + (x >> 1)];
    *b = (x & 1) ? (uint8_t)((col << 4) | (*b & 0x0f)) : (uint8_t)((*b & 0xf0) | col);
}

static inline void pset_world(int x, int y, unsigned col) { /* camera and clip */
    x -= p8g.cam_x;
    y -= p8g.cam_y;
    if (x >= p8g.clip_x0 && x < p8g.clip_x1 && y >= p8g.clip_y0 && y < p8g.clip_y1) put(x, y, col & 15);
}

static inline unsigned draw_color(int col) { return p8g.pal_draw[col & 15] & 15u; }

void p8_gfx_cls(int color) {
    const unsigned c = draw_color(color);
    fill_pairs(p8_screen, sizeof p8_screen, (uint8_t)(c | (c << 4)));
    p8g.clip_x0 = 0; p8g.clip_y0 = 0; p8g.clip_x1 = P8_W; p8g.clip_y1 = P8_H;
    p8g.cur_x = 0; p8g.cur_y = 0;
}

int p8_gfx_pget(int x, int y) {
    if (x >= p8g.clip_x0 && y >= p8g.clip_y0 && x < p8g.clip_x1 && y < p8g.clip_y1) {
        const uint8_t b = p8_screen[y * 64 + (x >> 1)];
        return (x & 1) ? b >> 4 : b & 15;
    }
    return 0;
}

void p8_gfx_pset(int x, int y, int col) { pset_world(x, y, draw_color(col)); }

void p8_gfx_rectfill(int x0, int y0, int x1, int y1, int col) {
    const unsigned c = draw_color(col);
    x0 -= p8g.cam_x; x1 -= p8g.cam_x;
    y0 -= p8g.cam_y; y1 -= p8g.cam_y;
    if (x0 < p8g.clip_x0) x0 = p8g.clip_x0;
    if (y0 < p8g.clip_y0) y0 = p8g.clip_y0;
    if (x1 >= p8g.clip_x1) x1 = p8g.clip_x1 - 1;
    if (y1 >= p8g.clip_y1) y1 = p8g.clip_y1 - 1;
    if (x0 > x1 || y0 > y1) return;
    const uint8_t pair = (uint8_t)(c | (c << 4));
    if (x0 == 0 && x1 == P8_W - 1) { /* whole rows: one fill */
        fill_pairs(&p8_screen[y0 * 64], (unsigned)(y1 - y0 + 1) * 64, pair);
        return;
    }
    for (int y = y0; y <= y1; y++) {
        uint8_t *row = &p8_screen[y * 64];
        int x = x0;
        if (x & 1) { row[x >> 1] = (uint8_t)((row[x >> 1] & 0x0f) | (c << 4)); x++; } /* odd first pixel */
        if (x <= x1) {
            const int pairs = (x1 - x + 1) >> 1;
            fill_pair_run(&row[x >> 1], (unsigned)pairs, pair);
            x += pairs * 2;
        }
        if (x <= x1) row[x >> 1] = (uint8_t)((row[x >> 1] & 0xf0) | c); /* the last one, even */
    }
}

static void hline(int x0, int x1, int y, unsigned c) {
    for (int x = x0; x <= x1; x++) pset_world(x, y, c);
}
static void vline(int x, int y0, int y1, unsigned c) {
    for (int y = y0; y <= y1; y++) pset_world(x, y, c);
}

void p8_gfx_rect(int x0, int y0, int x1, int y1, int col) {
    const unsigned c = draw_color(col);
    hline(x0, x1, y0, c);
    hline(x0, x1, y1, c);
    vline(x0, y0, y1, c);
    vline(x1, y0, y1, c);
}

void p8_gfx_line(int x0, int y0, int x1, int y1, int col) {
    const unsigned c = draw_color(col);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pset_world(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* circles and ovals: femto8's segment algorithm */
/* v * k / r, truncated toward zero, for a v that moves one step at a time. */
typedef struct { int q, rem, step_q, step_rem, sign; } OvalScale;

static OvalScale oval_scale(int k, int r, int at_r) {
    OvalScale s = { 0, 0, 0, k < 0 ? -k : k, k < 0 ? -1 : 1 };
    if (at_r) s.q = s.step_rem; /* v == r */
    while (s.step_rem >= r) { s.step_rem -= r; s.step_q++; }
    return s;
}
static inline void oval_up(OvalScale *s, int r) {
    s->q += s->step_q;
    s->rem += s->step_rem;
    if (s->rem >= r) { s->rem -= r; s->q++; }
}
static inline void oval_down(OvalScale *s, int r) {
    s->q -= s->step_q;
    s->rem -= s->step_rem;
    if (s->rem < 0) { s->rem += r; s->q--; }
}

/* x and y scaled by each radius: ax = x * xr / r, by = y * yr / r, and so on */
static void oval_segment(int xc, int yc, int ax, int by, int ay, int bx, unsigned c, int mask) {
    if (mask & 1) pset_world(xc + ax, yc + by, c);
    if (mask & 2) pset_world(xc - ax, yc + by, c);
    if (mask & 4) pset_world(xc + ax, yc - by, c);
    if (mask & 8) pset_world(xc - ax, yc - by, c);
    if (mask & 16) pset_world(xc + ay, yc + bx, c);
    if (mask & 32) pset_world(xc - ay, yc + bx, c);
    if (mask & 64) pset_world(xc + ay, yc - bx, c);
    if (mask & 128) pset_world(xc - ay, yc - bx, c);
}

static void oval_fill_segment(int xc, int yc, int ax, int by, int ay, int bx, unsigned c, int mask) {
    if (mask & 1) hline(xc, xc + ax, yc + by, c);
    if (mask & 2) hline(xc - ax, xc, yc + by, c);
    if (mask & 4) hline(xc, xc + ax, yc - by, c);
    if (mask & 8) hline(xc - ax, xc, yc - by, c);
    if (mask & 16) hline(xc, xc + ay, yc + bx, c);
    if (mask & 32) hline(xc - ay, xc, yc + bx, c);
    if (mask & 64) hline(xc, xc + ay, yc - bx, c);
    if (mask & 128) hline(xc - ay, xc, yc - bx, c);
}

static void oval(int xc, int yc, int xr, int yr, int col, int fill) {
    const int r = xr > yr ? xr : yr;
    if (r <= 0) return;
    const unsigned c = draw_color(col);
    int x = 0, y = r, d = 3 - 2 * r;
    OvalScale xx = oval_scale(xr, r, 0), xy = oval_scale(yr, r, 0); /* x by each radius */
    OvalScale yx = oval_scale(xr, r, 1), yy = oval_scale(yr, r, 1); /* y by each radius */
    void (*seg)(int, int, int, int, int, int, unsigned, int) = fill ? oval_fill_segment : oval_segment;
    seg(xc, yc, xx.sign * xx.q, yy.sign * yy.q, yx.sign * yx.q, xy.sign * xy.q, c, 0xff);
    while (y >= x) {
        x++;
        oval_up(&xx, r);
        oval_up(&xy, r);
        if (d > 0) { y--; oval_down(&yx, r); oval_down(&yy, r); d = d + 4 * (x - y) + 10; }
        else d = d + 4 * x + 6;
        seg(xc, yc, xx.sign * xx.q, yy.sign * yy.q, yx.sign * yx.q, xy.sign * xy.q, c, 0xff);
    }
}

void p8_gfx_circ(int x, int y, int r, int col) { oval(x, y, r, r, col, 0); }

/* oval(x0, y0, x1, y1): the centre and radii of the box, as femto8 does */
void p8_gfx_oval(int x0, int y0, int x1, int y1, int col, int fill) { oval((x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2, col, fill); }
void p8_gfx_circfill(int x, int y, int r, int col) { oval(x, y, r, r, col, 1); }

/* ---- sprites ---- */
static inline unsigned sheet_pixel(int x, int y) { /* 0 <= x, y < 128 */
    const uint8_t b = p8_rom_u8(&p8_cart[y * 64 + (x >> 1)]);
    return (x & 1) ? b >> 4 : b & 15u;
}

int p8_gfx_sget(int x, int y) { return x >= 0 && y >= 0 && x < 128 && y < 128 ? (int)sheet_pixel(x, y) : 0; }

void p8_gfx_sprite(int n, int left, int top, int flip_x, int flip_y) {
    const int dx = left - p8g.cam_x, dy = top - p8g.cam_y;
    if (dx >= p8g.clip_x1 || dx + 8 <= p8g.clip_x0 || dy >= p8g.clip_y1 || dy + 8 <= p8g.clip_y0) return;
    const uint8_t *sheet = &p8_cart[((n >> 4) & 15) * 8 * 64 + (n & 15) * 4];
    const uint8_t *pal = p8g.pal_draw;
    const int inside = dx >= p8g.clip_x0 && dx + 8 <= p8g.clip_x1;
    const int opaque = p8g.pal_simple && inside && !(dx & 1) && sprite_is_opaque(n);
    const int aligned = !(dx & 7);
    for (int y = 0; y < 8; y++) {
        const int py = dy + y;
        if (py < p8g.clip_y0 || py >= p8g.clip_y1) continue;
        const uint8_t *src = sheet + (flip_y ? 7 - y : y) * 64;
        uint8_t *row = &p8_screen[py * 64];
        /* one flash word per row; flip_x reverses the four bytes and swaps
         * the two pixels in each of them */
        uint32_t w = p8_rom_u32(src);
        if (flip_x) w = __builtin_bswap32(((w & 0x0f0f0f0fu) << 4) | ((w >> 4) & 0x0f0f0f0fu));
        if (opaque) { /* no colour 0: the bytes go out as they are */
            uint8_t *d = &row[dx >> 1];
            if (aligned) *(uint32_t *)d = w;
            else { d[0] = (uint8_t)w; d[1] = (uint8_t)(w >> 8); d[2] = (uint8_t)(w >> 16); d[3] = (uint8_t)(w >> 24); }
            continue;
        }
        if (p8g.pal_simple && inside && !(dx & 1)) { /* default palette, whole bytes: no table at all */
            uint8_t *d = &row[dx >> 1];
            for (int i = 0; i < 4; i++, d++) {
                const unsigned b = (w >> (8 * i)) & 0xffu;
                if ((b & 0x0f) && (b & 0xf0)) *d = (uint8_t)b;
                else if (b & 0x0f) *d = (uint8_t)((*d & 0xf0) | b);
                else if (b) *d = (uint8_t)((*d & 0x0f) | b);
            }
            continue;
        }
        uint8_t c[8]; /* the row through the palette; 0xff: transparent */
        for (int i = 0; i < 4; i++) {
            const unsigned b = (w >> (8 * i)) & 0xffu, lo = pal[b & 15], hi = pal[b >> 4];
            c[2 * i] = (lo & 0x10) ? 0xff : (uint8_t)(lo & 15);
            c[2 * i + 1] = (hi & 0x10) ? 0xff : (uint8_t)(hi & 15);
        }
        if (inside && !(dx & 1)) { /* whole bytes */
            uint8_t *d = &row[dx >> 1];
            for (int i = 0; i < 8; i += 2, d++) {
                const unsigned l = c[i], h = c[i + 1];
                if (l != 0xff && h != 0xff) *d = (uint8_t)(l | (h << 4));
                else if (l != 0xff) *d = (uint8_t)((*d & 0xf0) | l);
                else if (h != 0xff) *d = (uint8_t)((*d & 0x0f) | (h << 4));
            }
        } else {
            for (int i = 0; i < 8; i++) {
                const int px = dx + i;
                if (c[i] != 0xff && px >= p8g.clip_x0 && px < p8g.clip_x1) put(px, py, c[i]);
            }
        }
    }
}

void p8_gfx_sprites(int n, int x, int y, int w, int h, int flip_x, int flip_y) {
    for (int sy = 0; sy < h; sy++)
        for (int sx = 0; sx < w; sx++)
            p8_gfx_sprite((n == -1 ? 0 : n) + sx + sy * 16, x + (flip_x ? w - 1 - sx : sx) * 8,
                          y + (flip_y ? h - 1 - sy : sy) * 8, flip_x, flip_y);
}

void p8_gfx_sspr(int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, int flip_x, int flip_y) {
    if (dw <= 0 || dh <= 0) return;
    /* Bresenham in the source: x * sw / dw, truncated toward zero, without a
     * division per pixel; a negative source size walks the other way */
    const int width = sw < 0 ? -sw : sw, height = sh < 0 ? -sh : sh;
    const int step_x = (flip_x != 0) != (sw < 0) ? -1 : 1, step_y = (flip_y != 0) != (sh < 0) ? -1 : 1;
    int src_y = flip_y ? sy + sh - 1 : sy, acc_y = 0;
    for (int y = 0; y < dh; y++) {
        int src_x = flip_x ? sx + sw - 1 : sx, acc_x = 0;
        for (int x = 0; x < dw; x++) {
            if (src_x >= 0 && src_y >= 0 && src_x <= 127 && src_y <= 127) {
                const unsigned color = p8g.pal_draw[sheet_pixel(src_x, src_y)];
                if (!(color & 0xf0)) pset_world(dx + x, dy + y, color & 15u);
            }
            acc_x += width;
            while (acc_x >= dw) { acc_x -= dw; src_x += step_x; }
        }
        acc_y += height;
        while (acc_y >= dh) { acc_y -= dh; src_y += step_y; }
    }
}

/* ---- map ---- */
static int map_addr(int celx, int cely) {
    if (celx < 0 || cely < 0) return 0;
    int base = 0x2000;
    if (cely >= 32) { base = 0x1000; cely -= 32; }
    const int address = base + celx + cely * 128;
    if (address < 0x1000 || address >= 0x3000) return 0;
    return address;
}

int p8_gfx_mget(int celx, int cely) {
    const int a = map_addr(celx, cely);
    return a ? p8_rom_u8(&p8_cart[a]) : 0;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

void p8_gfx_map(int celx, int cely, int sx, int sy, int celw, int celh, int layer) {
    /* cells whose 8x8 box meets the clip rectangle */
    int x0 = floor_div(p8g.clip_x0 + p8g.cam_x - sx, 8), x1 = floor_div(p8g.clip_x1 - 1 + p8g.cam_x - sx, 8) + 1;
    int y0 = floor_div(p8g.clip_y0 + p8g.cam_y - sy, 8), y1 = floor_div(p8g.clip_y1 - 1 + p8g.cam_y - sy, 8) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > celw) x1 = celw;
    if (y1 > celh) y1 = celh;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const int index = p8_gfx_mget(celx + x, cely + y);
            const int flags = p8_rom_u8(&p8_cart[0x3000 + index]);
            if (index != 0 && (layer == 0 || (layer & flags) == layer))
                p8_gfx_sprite(index, sx + x * 8, sy + y * 8, 0, 0);
        }
}

/* ---- text ---- */
#include "p8_font.h"

int p8_gfx_print(const char *s, unsigned len, int x, int y, int col, int margin, int *out_x, int *out_y) {
    const unsigned c = draw_color(col);
    int right = x;
    for (unsigned i = 0; i < len; i++) {
        const uint8_t ch = (uint8_t)s[i];
        if (ch == '\n') { x = margin; y += 6; right = x > right ? x : right; continue; }
        if (ch == 0) break;
        const int sx = (ch % 16) * 8, sy = (ch / 16) * 8;
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                const int fx = sx + gx, fy = sy + gy;
                const uint8_t b = p8_rom_u8(&font_map[fy * 64 + (fx >> 1)]);
                if (((fx & 1) ? b >> 4 : b & 15) == 7) pset_world(x + gx, y + gy, c);
            }
        x += ch >= 0x80 ? 8 : 4;
        if (x > right) right = x;
    }
    *out_x = x;
    *out_y = y;
    return right;
}

/* ---- buttons ---- */
static uint32_t down_time[6];
static uint8_t first_repeat;

void p8_input_reset(void) {
    for (int i = 0; i < 6; i++) down_time[i] = 0xffffffffu;
    first_repeat = 0xff;
    p8_buttons = 0;
    p8_btnp = 0;
    p8_frames = 0;
}

void p8_input_update(uint16_t mask) {
    const unsigned delay = 15, interval = 4;
    p8_buttons = mask;
    p8_btnp = 0;
    for (unsigned i = 0; i < 6; i++) {
        if (mask & (1u << i)) {
            if (down_time[i] == 0xffffffffu) {
                /* held since the start: ignored until released */
            } else if (!down_time[i]) {
                down_time[i] = p8_frames;
                p8_btnp |= 1u << i;
            } else if (!(first_repeat & (1u << i)) && p8_frames - down_time[i] >= delay) {
                down_time[i] = p8_frames;
                first_repeat |= (uint8_t)(1u << i);
                p8_btnp |= 1u << i;
            } else if ((first_repeat & (1u << i)) && p8_frames - down_time[i] >= interval) {
                down_time[i] = p8_frames;
                p8_btnp |= 1u << i;
            }
        } else if (down_time[i]) {
            down_time[i] = 0;
            first_repeat &= (uint8_t)~(1u << i);
        }
    }
}

void p8_gfx_state_io(p8_state_fn io, void *ctx) {
    io(&p8g, sizeof p8g, ctx);
    io(p8_screen, sizeof p8_screen, ctx);
    io(&p8_buttons, sizeof p8_buttons, ctx);
    io(&p8_btnp, sizeof p8_btnp, ctx);
    io(&p8_frames, sizeof p8_frames, ctx);
    io(&p8_fps, sizeof p8_fps, ctx);
    io(down_time, sizeof down_time, ctx);
    io(&first_repeat, sizeof first_repeat, ctx);
}
