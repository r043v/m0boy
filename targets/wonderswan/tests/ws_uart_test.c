#include "ws.h"
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

static uint8_t rom(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    return 0;
}

static void send(void *peer, uint8_t value)
{
    ws_uart_receive(peer, value);
}

int main(void)
{
    Ws tx, rx;
    CHECK(ws_boot(&tx, (WsRom){rom, 0, 65536}));
    CHECK(ws_boot(&rx, (WsRom){rom, 0, 65536}));
    ws_uart_connect(&tx, send, &rx);
    ws_out(&tx, 0xb2, 1);
    ws_out(&rx, 0xb2, 8);
    ws_out(&rx, 0xb3, 0x80);
    ws_out(&tx, 0xb3, 0x80);
    CHECK(ws_in(&tx, 0xb4) == 1);
    ws_out(&tx, 0xb1, 0xa5);
    ws_out(&tx, 0xb1, 0x11);
    ws_out(&tx, 0xb6, 1);
    ws_video_clock(&tx, 3199, 0, 0);
    CHECK(ws_in(&tx, 0xb4) == 0 && !(ws_in(&rx, 0xb3) & 1));
    ws_video_clock(&tx, 1, 0, 0);
    CHECK(ws_in(&tx, 0xb4) == 1 && ws_in(&rx, 0xb4) == 8);
    ws_out(&rx, 0xb6, 8);
    CHECK(ws_in(&rx, 0xb4) == 8);

    ws_out(&tx, 0xb3, 0xc0);
    ws_out(&tx, 0xb1, 0x5a);
    ws_video_clock(&tx, 799, 0, 0);
    CHECK(!(ws_in(&rx, 0xb3) & 2));
    ws_video_clock(&tx, 1, 0, 0);
    CHECK(ws_in(&rx, 0xb3) & 2);
    CHECK(ws_in(&rx, 0xb1) == 0xa5);
    ws_out(&rx, 0xb6, 8);
    CHECK(ws_in(&rx, 0xb4) == 0);
    ws_out(&rx, 0xb3, 0xa0);
    CHECK(ws_in(&rx, 0xb3) == 0x84);
}
