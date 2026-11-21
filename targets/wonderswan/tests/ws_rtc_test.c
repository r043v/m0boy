#include "ws.h"
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

static uint8_t rom(void *context, uint32_t offset)
{
    return context && offset == 65536 - 3;
}

static void wait_byte(Ws *ws)
{
    ws_video_clock(ws, 64, NULL, NULL);
}

static void write_command(Ws *ws, uint8_t command, const uint8_t *bytes, unsigned count)
{
    ws_out(ws, 0xcb, bytes[0]);
    ws_out(ws, 0xca, command);
    for (unsigned i = 1; i < count; ++i) {
        wait_byte(ws);
        CHECK(ws_in(ws, 0xca) == (command | 0x80));
        ws_out(ws, 0xcb, bytes[i]);
    }
    wait_byte(ws);
    CHECK(ws_in(ws, 0xca) == ((command & 15) | 0x80));
}

static void read_command(Ws *ws, uint8_t command, const uint8_t *bytes, unsigned count)
{
    ws_out(ws, 0xca, command);
    CHECK(ws_in(ws, 0xca) == command);
    for (unsigned i = 0; i < count; ++i) {
        wait_byte(ws);
        CHECK(ws_in(ws, 0xca) == (command | 0x80));
        CHECK(ws_in(ws, 0xcb) == bytes[i]);
    }
    wait_byte(ws);
    CHECK(ws_in(ws, 0xca) == ((command & 15) | 0x80));
    CHECK(ws_in(ws, 0xcb) == bytes[count - 1]);
    CHECK(ws_in(ws, 0xca) == (command & 15));
}

int main(void)
{
    Ws ws;
    CHECK(ws_boot(&ws, (WsRom){rom, NULL, 65536}));
    read_command(&ws, 0x17, (uint8_t[]){0xff, 0xff, 0xff}, 3);
    ws_out(&ws, 0xca, 0x1f);
    wait_byte(&ws);
    CHECK(ws_in(&ws, 0xca) == 0x1f);
    CHECK(ws_in(&ws, 0xcb) == 0xff);

    CHECK(ws_boot(&ws, (WsRom){rom, &ws, 65536}));
    write_command(&ws, 0x12, (uint8_t[]){0x48}, 1);
    write_command(&ws, 0x14, (uint8_t[]){0x24, 0x02, 0x28, 3, 0x23, 0x59, 0x59}, 7);
    read_command(&ws, 0x15, (uint8_t[]){0x24, 0x02, 0x28, 3, 0x23, 0x59, 0x59}, 7);
    ws_out(&ws, 0xb2, 4);
    CHECK(!(ws_in(&ws, 0xb4) & 4));
    ws_video_clock(&ws, 3072000, NULL, NULL);
    read_command(&ws, 0x15, (uint8_t[]){0x24, 0x02, 0x29, 4, 0x00, 0x00, 0x00}, 7);
    CHECK(ws_in(&ws, 0xb4) & 4);
    ws_out(&ws, 0xb6, 4);
    CHECK(ws_in(&ws, 0xb4) & 4);
    ws_video_clock(&ws, 3072000, NULL, NULL);
    ws_out(&ws, 0xb6, 4);
    CHECK(!(ws_in(&ws, 0xb4) & 4));

    write_command(&ws, 0x18, (uint8_t[]){0x12, 0x34}, 2);
    uint8_t save[WS_RTC_SAVE_SIZE];
    ws_rtc_export(&ws.rtc, save);
    CHECK(ws_boot(&ws, (WsRom){rom, &ws, 65536}));
    CHECK(ws_rtc_import(&ws.rtc, save));
    read_command(&ws, 0x15, (uint8_t[]){0x24, 0x02, 0x29, 4, 0x00, 0x00, 0x01}, 7);
    read_command(&ws, 0x19, (uint8_t[]){0x12, 0x34}, 2);
    save[3] = 13;
    CHECK(!ws_rtc_import(&ws.rtc, save));
    read_command(&ws, 0x15, (uint8_t[]){0x24, 0x02, 0x29, 4, 0x00, 0x00, 0x01}, 7);
}
