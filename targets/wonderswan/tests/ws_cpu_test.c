#include "ws.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

static uint8_t rom(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    return 0xff;
}

static void load(Ws *ws, const uint8_t *code, unsigned size)
{
    CHECK(ws_boot(ws, (WsRom){rom, 0, 65536}));
    ws->cs = 0;
    ws->ip = 0x400;
    memcpy(ws->ram + ws->ip, code, size);
}

int main(void)
{
    Ws ws;
    uint8_t bios[4096] = {[4096 - 16] = 0x90};
    CHECK(ws_boot_with_bios(&ws, (WsRom){rom, NULL, 65536}, bios));
    CHECK(ws_in(&ws, 0xa0) == 0x84);
    CHECK(ws_read(&ws, ws_address(ws.cs, ws.ip)) == 0x90);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 1);
    ws_out(&ws, 0xa0, 0x0f);
    CHECK(ws_in(&ws, 0xa0) == 0x8d && ws_read(&ws, 0xffff0) == 0xff);
    ws_out(&ws, 0xa0, 0);
    CHECK(ws_in(&ws, 0xa0) == 0x81 && ws_read(&ws, 0xffff0) == 0xff);

    const uint8_t arithmetic[] = {
        0xb0, 0x7f, 0xf9, 0x14, 0, // ADC AL,0 with carry
        0xb0, 0, 0xf9, 0x1c, 0,   // SBB AL,0 with borrow
        0xb8, 0xf9, 0xff, 0xb3, 3, 0xf6, 0xfb, // IDIV BL: -7 / 3
        0x6b, 0xdb, 0xfe,          // IMUL BX,BX,-2
    };
    load(&ws, arithmetic, sizeof(arithmetic));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK((ws.reg[0] & 255) == 0x80 && (ws.flags & 0x891) == 0x890);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK((ws.reg[0] & 255) == 0xff && (ws.flags & 0x891) == 0x091);
    for (unsigned i = 0; i < 3; ++i)
        CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK(ws.reg[0] == 0xfffe);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.reg[3] == 0xfffa && !(ws.flags & 0x801));

    const uint8_t software_interrupt[] = {0xcd, 0x21, 0xf4};
    load(&ws, software_interrupt, sizeof(software_interrupt));
    ws.ram[0x21 * 4 + 1] = 5;
    ws.ram[0x500] = 0xcf;
    ws.flags |= 0x200;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x500 && ws.reg[4] == 0x1ffa);
    CHECK(ws.ram[0x1ffa] == 2 && ws.ram[0x1ffb] == 4);
    CHECK(!(ws.flags & 0x200));
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x402 && ws.reg[4] == 0x2000);
    CHECK(ws.flags & 0x200);
    CHECK(ws_step(&ws) == WS_STEP_HALTED);

    const uint8_t standby[] = {0xf4, 0xb0, 0x56};
    load(&ws, standby, sizeof(standby));
    ws_out(&ws, 0xb2, 0x40);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.halted && ws.ip == 0x401);
    ws_video_clock(&ws, 144 * 256 - ws.line_clock, NULL, NULL);
    CHECK(ws_in(&ws, 0xb4) == 0x40);
    CHECK(ws_step(&ws) == WS_STEP_OK && !ws.halted && ws.ip == 0x403);
    CHECK(ws.reg[0] == 0x56 && ws.reg[4] == 0x2000);

    const uint8_t scan[] = {0xf2, 0xae, 0xf3, 0xa6, 0xf3, 0x6f, 0xf3, 0x6d};
    load(&ws, scan, sizeof(scan));
    ws.reg[0] = 0x55;
    ws.reg[1] = 3;
    ws.reg[7] = 0x600;
    ws.ram[0x601] = 0x55;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x400 && ws.reg[1] == 2);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x402 && ws.reg[1] == 1);
    CHECK(ws.reg[7] == 0x602 && (ws.flags & 0x40));

    ws.flags |= 0x400;
    ws.reg[1] = 3;
    ws.reg[6] = 0x703;
    ws.reg[7] = 0x603;
    ws.ram[0x702] = 1;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x402 && ws.reg[1] == 2);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x404 && ws.reg[1] == 1);
    CHECK(ws.reg[6] == 0x701 && ws.reg[7] == 0x601 && !(ws.flags & 0x40));

    ws.flags &= ~0x400u;
    ws.reg[2] = 0xa4;
    ws.reg[6] = 0x700;
    ws.ram[0x700] = 0x34;
    ws.ram[0x701] = 0x12;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x406 && ws.reg[1] == 0);
    CHECK(ws.timer_reload[0] == 0x1234 && ws.reg[6] == 0x702);
    ws.reg[1] = 1;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x408 && ws.reg[7] == 0x603);
    CHECK(ws.ram[0x601] == 0x34 && ws.ram[0x602] == 0x12);

    const uint8_t loop[] = {0xe2, 0xfe, 0xe3, 0x00};
    load(&ws, loop, sizeof(loop));
    ws.reg[1] = 2;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x400 && ws.line_clock == 5);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x402 && ws.line_clock == 7);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x404 && ws.line_clock == 11);

    const uint8_t decimal[] = {
        0xb0, 0x99, 0x04, 1, 0x27, // DAA: 99 + 1 = 00 with carry
        0xb0, 42, 0xd4, 10, 0xd5, 10,
    };
    load(&ws, decimal, sizeof(decimal));
    for (unsigned i = 0; i < 3; ++i)
        CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK((ws.reg[0] & 255) == 0 && (ws.flags & 0x51) == 0x51);
    CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.reg[0] == 0x0402);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.reg[0] == 42);

    const uint8_t stack[] = {0xc8, 8, 0, 2, 0xc9, 0xff, 0x1e, 0x20, 0x06};
    load(&ws, stack, sizeof(stack));
    ws.reg[5] = 0x1f00;
    ws.ram[0x1efe] = 0xa5;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.reg[5] == 0x1ffe && ws.reg[4] == 0x1ff2);
    CHECK(ws.ram[0x1ffc] == 0xa5);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.reg[5] == 0x1f00 && ws.reg[4] == 0x2000);
    ws.ram[0x620] = 0x20;
    ws.ram[0x622] = 0x50;
    ws.ram[0x520] = 0xcb;
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.cs == 0x50 && ws.ip == 0x20);
    CHECK(ws.reg[4] == 0x1ffc);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.cs == 0 && ws.ip == 0x409);
    CHECK(ws.reg[4] == 0x2000);

    const uint8_t trap[] = {0x68, 0x02, 0x01, 0x9d, 0xf3, 0xaa, 0xf4};
    load(&ws, trap, sizeof(trap));
    ws.ram[5] = 5;
    ws.ram[0x500] = 0xcf;
    ws.reg[1] = 2;
    ws.reg[7] = 0x600;
    CHECK(ws_step(&ws) == WS_STEP_OK);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x404);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x500 && ws.reg[1] == 1);
    CHECK(ws.ram[0x1ffa] == 4 && ws.ram[0x1ffb] == 4 && !(ws.flags & 0x100));
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x404 && (ws.flags & 0x100));
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x500 && ws.reg[1] == 0);
    CHECK(ws.ram[0x1ffa] == 6 && ws.ram[0x1ffb] == 4);

    const uint8_t stack_shadow[] = {0x8e, 0xd0, 0xbc, 0x00, 0x18, 0xf4};
    load(&ws, stack_shadow, sizeof(stack_shadow));
    ws.ram[9] = 5;
    ws.ram[0x500] = 0xcf;
    ws_set_low_battery(&ws, true);
    CHECK(!ws.nmi_pending);
    ws_set_low_battery(&ws, false);
    ws_out(&ws, 0xb7, 0x10);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x402);
    ws_set_low_battery(&ws, true);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x405 && ws.reg[4] == 0x1800);
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x500 && ws.reg[4] == 0x17fa);
    CHECK(!ws.nmi_pending && !(ws.flags & 0x200));
    CHECK(ws_step(&ws) == WS_STEP_OK && ws.ip == 0x405);
    CHECK(ws_step(&ws) == WS_STEP_HALTED);
    ws_set_low_battery(&ws, true);
    CHECK(ws_step(&ws) == WS_STEP_HALTED);
    ws_set_low_battery(&ws, false);
    ws_set_low_battery(&ws, true);
    CHECK(ws_step(&ws) == WS_STEP_OK && !ws.halted && ws.ip == 0x500);
}
