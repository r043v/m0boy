#include "ws.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

typedef struct {
    uint32_t size;
    uint8_t mapper;
} Cartridge;

static uint8_t read_rom(void *context, uint32_t offset)
{
    const Cartridge *cart = context;
    CHECK(offset < cart->size);
    return offset == cart->size - 3 ? cart->mapper : offset >> 16;
}

static uint8_t read_flash(void *context, uint32_t offset)
{
    return ((uint8_t *)context)[offset];
}

static void store_flash(void *context, uint32_t offset, uint32_t count, uint8_t value)
{
    CHECK(offset < WS_FLASH_SIZE && count <= WS_FLASH_SIZE - offset);
    memset((uint8_t *)context + offset, value, count);
}

static void flash_command(Ws *ws, uint16_t address, uint8_t value)
{
    ws_write(ws, 0x10aaa, 0xaa);
    ws_write(ws, 0x10555, 0x55);
    ws_write(ws, 0x10000 + address, value);
}

int main(void)
{
    Ws ws;
    Cartridge cart = {3 * 65536, 0};
    CHECK(ws_boot(&ws, (WsRom){read_rom, &cart, cart.size}));
    CHECK(ws_rom_offset(&ws, 0xffff0) == cart.size - 16);
    ws_out(&ws, 0xc2, 0);
    CHECK(ws_rom_offset(&ws, 0x20000) == UINT32_MAX);
    CHECK(ws_read(&ws, 0x20000) == 0xff);
    ws_out(&ws, 0xc2, 1);
    CHECK(ws_rom_offset(&ws, 0x20012) == 0x12);
    ws_out(&ws, 0xd2, 2);
    CHECK(ws_in(&ws, 0xc2) == 1 && ws_in(&ws, 0xd2) == 0xff);

    cart = (Cartridge){WS_MAX_ROM_SIZE, 1};
    CHECK(ws_boot(&ws, (WsRom){read_rom, &cart, cart.size}));
    CHECK(ws_rom_offset(&ws, 0xffff0) == cart.size - 16);
    ws_out(&ws, 0xd3, 2);
    ws_out(&ws, 0xc2, 5);
    CHECK(ws_in(&ws, 0xd2) == 5 && ws_in(&ws, 0xd3) == 2);
    CHECK(ws_rom_offset(&ws, 0x20012) == 0x2050012);
    ws_out(&ws, 0xcf, 33);
    CHECK(ws_in(&ws, 0xc0) == 33);
    CHECK(ws_rom_offset(&ws, 0x40012) == 0x2140012);

    uint8_t sram[8192] = {0};
    CHECK(ws_attach_sram(&ws, sram, sizeof(sram)));
    ws_write(&ws, 0x10000, 0xa5);
    ws_out(&ws, 0xd0, 0x1e);
    ws_out(&ws, 0xce, 1);
    CHECK(ws_read(&ws, 0x10000) == 0x1e);
    ws_write(&ws, 0x10000, 0);
    ws_out(&ws, 0xce, 0);
    CHECK(ws_read(&ws, 0x10000) == 0xa5);

    ws_out(&ws, 0xcc, 0xf5);
    ws_out(&ws, 0xcd, 0xfe);
    CHECK(ws_in(&ws, 0xcc) == 5 && ws_in(&ws, 0xcd) == 4);

    static uint8_t nor[WS_FLASH_SIZE];
    memset(nor, 0xff, sizeof(nor));
    nor[sizeof(nor) - 3] = 1;
    CHECK(ws_boot(&ws, (WsRom){read_flash, nor, sizeof(nor)}));
    CHECK(ws_attach_flash(&ws, store_flash, nor));
    ws_out(&ws, 0xce, 1);
    flash_command(&ws, 0xaaa, 0x90);
    CHECK(ws_read(&ws, 0x10000) == 4 && ws_read(&ws, 0x10002) == 0x0c);
    CHECK(ws_read(&ws, 0x10003) == 0 && ws_read(&ws, 0x20003) == 0x22);
    ws_write(&ws, 0x10000, 0xf0);
    ws_out(&ws, 0xc1, 7);
    flash_command(&ws, 0xaaa, 0xa0);
    ws_write(&ws, 0x12123, 0xa5);
    flash_command(&ws, 0xaaa, 0xa0);
    ws_write(&ws, 0x12123, 0x1d);
    CHECK(ws_read(&ws, 0x12123) == 5 && ws_read(&ws, 0x22123) == 5);
    nor[0x70000] = 0;
    nor[0x74000] = 0;
    flash_command(&ws, 0xaaa, 0x80);
    flash_command(&ws, 0x2123, 0x30);
    CHECK(ws_read(&ws, 0x12123) == 0xff && nor[0x70000] == 0 && nor[0x74000] == 0);
    flash_command(&ws, 0xaaa, 0x80);
    flash_command(&ws, 0xaaa, 0x10);
    CHECK(nor[0x70000] == 0xff && nor[0x74000] == 0xff);
}
