#include "ws.h"
#include <string.h>

#if WS_ENABLE_COLOR
typedef uint16_t Pixel;
enum { SCREEN2_PIXEL = 0x1000, SPRITE_PIXEL = 0x2000 };
#else
typedef uint8_t Pixel;
enum { SCREEN2_PIXEL = 0x10, SPRITE_PIXEL = 0x20 };
#endif

static unsigned mode(const Ws *ws)
{
#if WS_ENABLE_COLOR
    return ws->video_mode & 0x80 ? ws->video_mode >> 5 : 0;
#else
    (void)ws;
    return 0;
#endif
}

static uint8_t darkness(const Ws *ws, unsigned shade)
{
    unsigned packed = ws->display[0x1c + shade / 2];
    return (packed >> (4 * (shade & 1))) & 15;
}

static unsigned tile_pixel(const Ws *ws, unsigned attr, unsigned row, unsigned col,
                           bool sprite)
{
    row ^= (attr & 0x8000) ? 7 : 0;
    unsigned bit = (attr & 0x4000) ? col : 7 - col;
    unsigned bank = mode(ws) && !sprite && (attr & 0x2000) ? 1 : 0;
    if (mode(ws) & 2) {
        unsigned tile = 0x4000 + bank * 0x4000 + (attr & 0x1ff) * 32 + row * 4;
        if (mode(ws) & 1) {
            unsigned x = 7 - bit;
            return (ws->ram[tile + x / 2] >> ((x & 1) ? 0 : 4)) & 15;
        }
        unsigned color = 0;
        for (unsigned plane = 0; plane < 4; ++plane)
            color |= ((ws->ram[tile + plane] >> bit) & 1) << plane;
        return color;
    }
    unsigned tile = 0x2000 + bank * 0x2000 + (attr & 0x1ff) * 16 + row * 2;
    return ((ws->ram[tile] >> bit) & 1) |
           (((ws->ram[tile + 1] >> bit) & 1) << 1);
}

static uint8_t palette_darkness(const Ws *ws, unsigned palette, unsigned color)
{
    unsigned packed = ws->display[0x20 + palette * 2 + color / 2];
    return darkness(ws, (packed >> (4 * (color & 1))) & 7);
}

static Pixel palette_pixel(const Ws *ws, unsigned palette, unsigned color)
{
#if WS_ENABLE_COLOR
    if (mode(ws)) {
        unsigned address = 0xfe00 + (palette * 16 + color) * 2;
        return ws->ram[address] | ((ws->ram[address + 1] & 15) << 8);
    }
#endif
    return palette_darkness(ws, palette, color);
}

static void draw_screen(const Ws *ws, unsigned screen, unsigned line,
                        Pixel pixels[WS_WIDTH])
{
    const uint8_t *io = ws->display;
    unsigned map = ((io[7] >> (screen * 4)) & (mode(ws) ? 15 : 7)) << 11;
    unsigned y = (line + io[0x11 + screen * 2]) & 255;
    unsigned first = 0, end = WS_WIDTH;
    bool window = screen && (io[0] & 0x20);
    bool outside = io[0] & 0x10;
    bool within_y = line >= io[9] && line <= io[0xb];

    if (window && !outside) {
        if (!within_y)
            return;
        first = io[8];
        if (end > (unsigned)io[0xa] + 1)
            end = (unsigned)io[0xa] + 1;
    }

    for (unsigned x = first; x < end; ) {
        unsigned sx = (x + io[0x10 + screen * 2]) & 255;
        unsigned entry = map + ((y / 8) * 32 + sx / 8) * 2;
        unsigned attr = ws->ram[entry] | (ws->ram[entry + 1] << 8);
        unsigned palette = (attr >> 9) & 15;
        for (unsigned col = sx & 7; col < 8 && x < end; ++col, ++x) {
            if (window && outside && within_y && x >= io[8] && x <= io[0xa])
                continue;
            unsigned color = tile_pixel(ws, attr, y & 7, col, false);
            if (!color && ((mode(ws) & 2) || (palette & 4)))
                continue;
            pixels[x] = palette_pixel(ws, palette, color) |
                        (screen ? SCREEN2_PIXEL : 0);
        }
    }
}

static void draw_sprites(const Ws *ws, unsigned line, Pixel pixels[WS_WIDTH])
{
    const uint8_t *io = ws->display;
    const uint8_t *table = ws->sprites;
    unsigned index = io[5] & 127;
    unsigned count = io[6];
    unsigned visible = 0;
    bool within_y = line >= io[0x0d] && line <= io[0x0f];

    for (; count && index < 128 && visible < 32; --count, ++index) {
        const uint8_t *entry = table + index * 4;
        unsigned row = (line - entry[2]) & 255;
        if (row >= 8)
            continue;
        ++visible;

        unsigned attr = entry[0] | (entry[1] << 8);
        unsigned palette = 8 + ((attr >> 9) & 7);
        for (unsigned col = 0; col < 8; ++col) {
            unsigned x = (entry[3] + col) & 255;
            if (x >= WS_WIDTH || (pixels[x] & SPRITE_PIXEL))
                continue;
            if (io[0] & 8) {
                bool inside = within_y && x >= io[0x0c] && x <= io[0x0e];
                if (inside == ((attr & 0x1000) != 0))
                    continue;
            }

            unsigned color = tile_pixel(ws, attr, row, col, true);
            if (!color && ((mode(ws) & 2) || (palette & 4)))
                continue;
            // The first opaque sprite wins even when Screen 2 hides it.
            if ((attr & 0x2000) || !(pixels[x] & SCREEN2_PIXEL))
                pixels[x] = palette_pixel(ws, palette, color);
            pixels[x] |= SPRITE_PIXEL;
        }
    }
}

static void render(const Ws *ws, unsigned line, Pixel pixels[WS_WIDTH])
{
#if WS_ENABLE_COLOR
    Pixel background = mode(ws) ? palette_pixel(ws, ws->display[1] >> 4, ws->display[1] & 15)
                                : darkness(ws, ws->display[1] & 7);
    for (unsigned x = 0; x < WS_WIDTH; ++x)
        pixels[x] = background;
#else
    memset(pixels, darkness(ws, ws->display[1] & 7), WS_WIDTH);
#endif
    if (line < WS_HEIGHT && !(ws->display[0x1a] & 1)) {
        for (unsigned screen = 0; screen < 2; ++screen)
            if (ws->display[0] & (1u << screen))
                draw_screen(ws, screen, line, pixels);
        if (ws->display[0] & 4)
            draw_sprites(ws, line, pixels);
    }
}

#if WS_ENABLE_COLOR
void ws_render_line_rgb(const Ws *ws, unsigned line, uint16_t pixels[WS_WIDTH])
{
    render(ws, line, pixels);
    for (unsigned x = 0; x < WS_WIDTH; ++x)
        pixels[x] = mode(ws) ? pixels[x] & 0xfff : (15 - (pixels[x] & 15)) * 0x111;
}
#endif

void ws_render_line(const Ws *ws, unsigned line, uint8_t pixels[WS_WIDTH])
{
#if WS_ENABLE_COLOR
    uint16_t rgb[WS_WIDTH];
    ws_render_line_rgb(ws, line, rgb);
    for (unsigned x = 0; x < WS_WIDTH; ++x) {
        unsigned red = rgb[x] >> 8, green = (rgb[x] >> 4) & 15, blue = rgb[x] & 15;
        pixels[x] = (red * 77 + green * 150 + blue * 29) * 17 / 256;
    }
#else
    render(ws, line, pixels);
    for (unsigned x = 0; x < WS_WIDTH; ++x)
        pixels[x] = 255 - 17 * (pixels[x] & 15);
#endif
}


static void tick_timer(Ws *ws, unsigned timer, unsigned irq)
{
    unsigned control = ws->timer_control >> (timer * 2);
    // BizHawk's core only counts enabled timers and raises the interrupt when the
    // count reaches zero, reloading before the handler can observe the count.
    if ((control & 1) && ws->timer_count[timer]) {
        if (--ws->timer_count[timer] == 0) {
            ws->irq_pending |= ws->irq_enable & irq;
            if (control & 2)
                ws->timer_count[timer] = ws->timer_reload[timer];
        }
    }
}

static void copy_sprites(Ws *ws, unsigned clock, unsigned clocks)
{
    unsigned base = (ws->display[4] & (mode(ws) ? 63 : 31)) << 9;
    memcpy(ws->sprites + 2 * clock, ws->ram + base + 2 * clock, 2 * clocks);
}

// The default sink sees each completed visible line. BizHawk samples at
// line entry and counts a completed frame after executing line 144.
// Hardware transfers two OAM bytes per clock on line 144.
void ws_video_clock(Ws *ws, unsigned clocks, WsLineSink sink, void *context)
{
#if WS_ENABLE_BIZHAWK_TIMING
    bool bizhawk_timing = ws->bizhawk_timing;
#else
    const bool bizhawk_timing = false;
#endif
    unsigned until_line = 256 - ws->line_clock;
    while (clocks >= until_line) {
        if (ws->uart_tx_clocks)
            ws_uart_clock(ws, until_line);
        if (ws->rtc.present || ws->rtc.delay)
            ws_rtc_clock(ws, until_line);
        ws_audio_clock(ws, until_line);
        if (!bizhawk_timing && ws->line == WS_HEIGHT)
            copy_sprites(ws, ws->line_clock, until_line);
        clocks -= until_line;
        ws->line_clock = 0;
        if (bizhawk_timing) {
            if (ws->line == WS_HEIGHT)
                ++ws->frames;
        } else if (sink && ws->line < WS_HEIGHT) {
            sink(context, ws, ws->line);
        }
        if (ws->line_override) {
            ws->line = ws->display[0x18];
            ws->line_override = false;
        } else if (ws->line == ws->display[0x16]) {
            ws->line = 0;
        } else {
            ++ws->line;
        }
        tick_timer(ws, 0, 0x80);
        if (ws->line == ws->display[3])
            ws->irq_pending |= ws->irq_enable & 0x10;
        if (ws->line == WS_HEIGHT) {
            tick_timer(ws, 1, 0x20);
            if (!bizhawk_timing)
                ++ws->frames;
            ws->irq_pending |= ws->irq_enable & 0x40;
        }
        if (bizhawk_timing && ws->line == WS_HEIGHT - 2) {
            copy_sprites(ws, 0, 256);
        }
        if (bizhawk_timing && sink && ws->line < WS_HEIGHT)
            sink(context, ws, ws->line);
        until_line = 256;
    }
    if (ws->uart_tx_clocks)
        ws_uart_clock(ws, clocks);
    if (ws->rtc.present || ws->rtc.delay)
        ws_rtc_clock(ws, clocks);
    ws_audio_clock(ws, clocks);
    if (!bizhawk_timing && ws->line == WS_HEIGHT && clocks)
        copy_sprites(ws, ws->line_clock, clocks);
    ws->line_clock += clocks;
}
