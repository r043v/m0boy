#include "ws.h"
#include <string.h>

enum { ROM_BANK_SIZE = 65536 };

static uint32_t bank_offset(const Ws *ws, unsigned bank)
{
    unsigned banks = ws->rom.size >> 16;
    return ((bank & ws->rom_bank_mask) + banks - (ws->rom_bank_mask + 1u)) << 16;
}

static void map_linear(Ws *ws, uint8_t value)
{
    ws->rom_linear = value & (ws->mapper_2003 ? 63 : 15);
    for (unsigned window = 4; window < 16; ++window)
        ws->rom_window[window] = bank_offset(ws, ws->rom_linear * 16 + window);
}

static void map_bank(Ws *ws, unsigned window, unsigned bank)
{
    if (window == 1)
        ws->sram_bank = bank;
    else
        ws->rom_bank[window - 2] = bank;
    ws->rom_window[window] = bank_offset(ws, bank);
}

bool ws_boot_model(Ws *ws, WsRom rom, bool color, const uint8_t *bios)
{
    if (!rom.read || rom.size < ROM_BANK_SIZE ||
        rom.size > WS_MAX_ROM_SIZE || rom.size % ROM_BANK_SIZE)
        return false;
    if (color && !WS_ENABLE_COLOR)
        return false;

    memset(ws, 0, sizeof(*ws));
    ws->rom = rom;
    ws->boot_rom = bios;
    // Cartridge authentication is supplied by the frontend.
    ws->system_control = (bios ? 0x84 : 0x85) | (color ? 2 : 0);
    unsigned mapper = rom.read(rom.context, rom.size - 3);
    ws->rtc.present = mapper == 1;
    ws->mapper_2003 = mapper != 0 || rom.size > 16u * 1024 * 1024;
    while (ws->rom_bank_mask + 1u < rom.size / ROM_BANK_SIZE)
        ws->rom_bank_mask = ws->rom_bank_mask * 2 + 1;
    ws->rtc.date[1] = ws->rtc.date[2] = 1;
    ws->rtc.status = 0xc0;
    ws->rtc.data = 0xff;
    ws->audio.io[0x1e] = 3;
    ws->internal_eeprom = (WsEeprom){
        .data = ws->internal_nv,
        .size = color ? WS_COLOR_EEPROM_SIZE : WS_MONO_EEPROM_SIZE,
        .write_protected = !bios
    };
    ws->cart_eeprom.data = ws->cart_nv;
    memset(ws->cart_nv, 0xff, sizeof(ws->cart_nv));
    // The save-device code is the fifth byte from the end of the ROM.
    switch (rom.read(rom.context, rom.size - 5)) {
    case 0x01: ws->cart_sram_size = 8 * 1024; break;
    case 0x02: ws->cart_sram_size = 32 * 1024; break;
    case 0x03: ws->cart_sram_size = 128 * 1024; break;
    case 0x04: ws->cart_sram_size = 256 * 1024; break;
    case 0x05: ws->cart_sram_size = 512 * 1024; break;
    case 0x10: ws->cart_eeprom.size = 128; break;
    case 0x20: ws->cart_eeprom.size = 2048; break;
    case 0x50: ws->cart_eeprom.size = 1024; break;
    }
    // Both the boot ROM and the mapped cartridge provide a reset entry here.
    ws->cs = 0xffff;
    ws->flags = 2;
    if (!bios)
        ws->reg[4] = 0x2000;
    ws->display[0x16] = 158;
    unsigned last_bank = ws->mapper_2003 ? 1023 : 255;
    map_linear(ws, last_bank >> 4);
    map_bank(ws, 1, 0);
    map_bank(ws, 2, last_bank);
    map_bank(ws, 3, last_bank);
    return true;
}

bool ws_boot(Ws *ws, WsRom rom)
{
    return ws_boot_model(ws, rom, false, NULL);
}

bool ws_boot_with_bios(Ws *ws, WsRom rom, const uint8_t bios[WS_MONO_BIOS_SIZE])
{
    return ws_boot_model(ws, rom, false, bios);
}

static unsigned ram_size(const Ws *ws)
{
#if WS_ENABLE_COLOR
    if (ws->video_mode & 0x80)
        return WS_RAM_SIZE;
#else
    (void)ws;
#endif
    return WS_MONO_RAM_SIZE;
}

bool ws_attach_sram(Ws *ws, uint8_t *data, uint32_t size)
{
    if (size && (!data || size > 512 * 1024 || (size & (size - 1))))
        return false;
    ws->sram = data;
    ws->sram_read = NULL;
    ws->sram_write = NULL;
    ws->sram_size = size;
    return true;
}

bool ws_attach_sram_bus(Ws *ws, uint32_t size, void *context,
                        uint8_t (*read)(void *, uint32_t),
                        void (*write)(void *, uint32_t, uint8_t))
{
    if (!read || !write || !size || size > 512 * 1024 || (size & (size - 1)))
        return false;
    ws->sram = NULL;
    ws->sram_size = size;
    ws->sram_context = context;
    ws->sram_read = read;
    ws->sram_write = write;
    return true;
}

static uint32_t sram_offset(const Ws *ws, uint32_t address)
{
    return (((uint32_t)ws->sram_bank << 16) | (address & 0xffff)) &
           (ws->sram_size - 1);
}

uint32_t ws_address(uint16_t segment, uint16_t offset)
{
    return (((uint32_t)segment << 4) + offset) & 0xfffff;
}

static unsigned bios_size(const Ws *ws)
{
#if WS_ENABLE_COLOR
    if (ws->system_control & 2)
        return WS_COLOR_BIOS_SIZE;
#else
    (void)ws;
#endif
    return WS_MONO_BIOS_SIZE;
}

uint32_t ws_rom_offset(const Ws *ws, uint32_t address)
{
    address &= 0xfffff;
    if (address < 0x10000 || (address < 0x20000 && !ws->rom_in_sram_window))
        return UINT32_MAX;
    if (ws->boot_rom && !(ws->system_control & 1) && address >= 0x100000 - bios_size(ws))
        return UINT32_MAX;
    uint32_t base = ws->rom_window[address >> 16];
    return base < ws->rom.size ? base + (address & 0xffff) : UINT32_MAX;
}

uint8_t ws_read(const Ws *ws, uint32_t address)
{
    address &= 0xfffff;
    if (ws->boot_rom && !(ws->system_control & 1) && address >= 0x100000 - bios_size(ws))
        return ws->boot_rom[address & (bios_size(ws) - 1)];
    if (address < ram_size(ws))
        return ws->ram[address];
    if (address < 0x20000 && !ws->rom_in_sram_window) {
        if (address < 0x10000 || !ws->sram_size)
            return 0xff;
        uint32_t offset = sram_offset(ws, address);
        return ws->sram_read ? ws->sram_read(ws->sram_context, offset) : ws->sram[offset];
    }

    uint32_t offset = ws_rom_offset(ws, address);
    if (offset == UINT32_MAX)
        return 0xff;
    if (ws->flash.identify || ws->flash.fast)
        return ws_flash_read(ws, offset, address >= 0x20000 && (ws->system_control & 4));
    return ws->rom.read(ws->rom.context, offset);
}

void ws_write(Ws *ws, uint32_t address, uint8_t value)
{
    address &= 0xfffff;
    if (address < ram_size(ws))
        ws->ram[address] = value;
    else if (address >= 0x10000 && address < 0x20000 && ws->rom_in_sram_window) {
        if (ws->flash.store)
            ws_flash_write(ws, ws_rom_offset(ws, address), value);
    } else if (address >= 0x10000 && address < 0x20000 && ws->sram_size) {
        uint32_t offset = sram_offset(ws, address);
        if (ws->sram_write)
            ws->sram_write(ws->sram_context, offset, value);
        else
            ws->sram[offset] = value;
    }
}

void ws_set_low_battery(Ws *ws, bool low)
{
    if (low && !ws->low_battery && ws->nmi_enabled)
        ws->nmi_pending = true;
    ws->low_battery = low;
}

void ws_set_keys(Ws *ws, uint16_t keys)
{
    keys &= 0x0ffe;
    if (keys & ~ws->keys)
        ws->irq_pending |= ws->irq_enable & 2;
    ws->keys = keys;
}

static unsigned mapper_port(const Ws *ws, unsigned port)
{
    if (ws->mapper_2003 && port >= 0xcf && port <= 0xd4) {
        if (port == 0xcf)
            return 0xc0;
        if (!(port & 1))
            return 0xc1 + ((port - 0xd0) >> 1);
    }
    return port;
}

#if WS_ENABLE_COLOR
static unsigned dma_read(const Ws *ws, unsigned port)
{
    if (!(ws->video_mode & 0x80))
        return 0xff;
    if (port < 0x43)
        return ws->dma_source >> (8 * (port - 0x40));
    if (port == 0x43)
        return 0xff;
    if (port < 0x46)
        return ws->dma_target >> (8 * (port - 0x44));
    if (port < 0x48)
        return ws->dma_length >> (8 * (port - 0x46));
    return ws->dma_control;
}

static void dma_write(Ws *ws, unsigned port, unsigned value)
{
    if (!(ws->video_mode & 0x80))
        return;
    if (!(port & 1) && port != 0x42)
        value &= 0xfe;
    if (port < 0x43) {
        unsigned shift = 8 * (port - 0x40);
        ws->dma_source = ((ws->dma_source & ~(255u << shift)) | (value << shift)) & 0xfffff;
    } else if (port == 0x44 || port == 0x45) {
        unsigned shift = 8 * (port - 0x44);
        ws->dma_target = (ws->dma_target & ~(255u << shift)) | (value << shift);
    } else if (port == 0x46 || port == 0x47) {
        unsigned shift = 8 * (port - 0x46);
        ws->dma_length = (ws->dma_length & ~(255u << shift)) | (value << shift);
    } else if (port == 0x48) {
        ws->dma_control = value & 0xc0;
    }
}

static bool dma_source_valid(const Ws *ws)
{
    return ws->dma_source < 0x10000 ||
           (ws->dma_source >= 0x20000 && (ws->system_control & 0x0c) == 4);
}

void ws_dma_run(Ws *ws, WsLineSink sink, void *context)
{
    if (!(ws->dma_control & 0x80))
        return;
    if (ws->dma_length && dma_source_valid(ws)) {
        int delta = ws->dma_control & 0x40 ? -2 : 2;
        ws_video_clock(ws, 5, sink, context);
        do {
            uint8_t low = ws_read(ws, ws->dma_source);
            uint8_t high = ws_read(ws, ws->dma_source + 1);
            ws->ram[ws->dma_target] = low;
            ws->ram[ws->dma_target + 1] = high;
            ws->dma_source = (ws->dma_source + delta) & 0xfffff;
            ws->dma_target += delta;
            ws->dma_length -= 2;
            ws_video_clock(ws, 2, sink, context);
        } while (ws->dma_length && dma_source_valid(ws));
    }
    ws->dma_control &= 0x40;
}
#endif

uint8_t ws_in(Ws *ws, uint16_t port)
{
#if WS_ENABLE_COLOR
    if (port >= 0x40 && port <= 0x48)
        return dma_read(ws, port);
    if ((port >= 0x4a && port <= 0x52) || (port >= 0x64 && port <= 0x6b))
        return ws_color_audio_read(ws, port);
#endif
    if (port >= 0x80 && port <= 0x9e)
        return ws_audio_read(ws, port - 0x80);
    if (port >= 0xba && port <= 0xbe)
        return ws_eeprom_read(&ws->internal_eeprom, port - 0xba, true);
    if (port >= 0xc4 && port <= 0xc8)
        return ws_eeprom_read(&ws->cart_eeprom, port - 0xc4, false);
    if (port == 2)
        return ws->line;
    if (port == 0x18)
        return 0xff;
    if (port < sizeof(ws->display))
        return ws->display[port];
    if (port >= 0xa4 && port <= 0xab) {
        const uint16_t *values = port < 0xa8 ? ws->timer_reload : ws->timer_count;
        return values[(port >> 1) & 1] >> ((port & 1) * 8);
    }
    switch (mapper_port(ws, port)) {
#if WS_ENABLE_COLOR
    case 0x60: return (ws->system_control & 2) ? ws->video_mode : 0xff;
#endif
    case 0xa0: return ws->system_control;
    case 0xa2: return ws->timer_control;
    case 0xa3: return 0;
    case 0xb0: {
        unsigned source = 7;
        while (source && !(ws->irq_pending & (1u << source)))
            --source;
        return ws->irq_base | source;
    }
    case 0xb1: return ws_uart_read(ws, false);
    case 0xb3: return ws_uart_read(ws, true);
    case 0xb2: return ws->irq_enable;
    case 0xb4: return ws->irq_pending;
    case 0xb5: {
        unsigned value = ws->key_select;
        for (unsigned row = 0; row < 3; ++row)
            if (value & (0x10u << row))
                value |= (ws->keys >> (8 - row * 4)) & 15;
        return value;
    }
    case 0xb7: return ws->nmi_enabled ? 0x10 : 0;
    case 0xc0: return ws->mapper_2003 ? ws->rom_linear : 0xf0 | ws->rom_linear;
    case 0xc1: return ws->sram_bank;
    case 0xc2: return ws->rom_bank[0];
    case 0xc3: return ws->rom_bank[1];
    case 0xca: return ws_rtc_read(ws, true);
    case 0xcb: return ws_rtc_read(ws, false);
    case 0xcc: return ws->mapper_2003 ? ws->gpo_direction : 0xff;
    case 0xcd: return ws->mapper_2003 ? ws->gpo_data & ws->gpo_direction : 0xff;
    case 0xce: return ws->mapper_2003 ? ws->rom_in_sram_window : 0xff;
    case 0xd1: return ws->mapper_2003 ? ws->sram_bank >> 8 : 0xff;
    case 0xd3: return ws->mapper_2003 ? ws->rom_bank[0] >> 8 : 0xff;
    case 0xd5: return ws->mapper_2003 ? ws->rom_bank[1] >> 8 : 0xff;
    default: return 0xff;
    }
}

void ws_out(Ws *ws, uint16_t port, uint8_t value)
{
#if WS_ENABLE_COLOR
    if (port >= 0x40 && port <= 0x48) {
        dma_write(ws, port, value);
        return;
    }
    if ((port >= 0x4a && port <= 0x52) || (port >= 0x64 && port <= 0x6b)) {
        ws_color_audio_write(ws, port, value);
        return;
    }
#endif
    if (port >= 0x80 && port <= 0x9e) {
        ws_audio_write(ws, port - 0x80, value);
        return;
    }
    if (port >= 0xba && port <= 0xbe) {
        ws_eeprom_write(&ws->internal_eeprom, port - 0xba, value, true);
        return;
    }
    if (port >= 0xc4 && port <= 0xc8) {
        ws_eeprom_write(&ws->cart_eeprom, port - 0xc4, value, false);
        return;
    }
    if (port < sizeof(ws->display)) {
        if (port == 0x18)
            ws->line_override = true;
        if (port != 2)
            ws->display[port] = value;
        return;
    }
    if (port >= 0xa4 && port <= 0xa7) {
        unsigned timer = (port >> 1) & 1;
        unsigned shift = (port & 1) * 8;
        ws->timer_reload[timer] = (ws->timer_reload[timer] & ~(255u << shift)) |
                                  ((unsigned)value << shift);
        ws->timer_count[timer] = ws->timer_reload[timer];
        return;
    }
    switch (mapper_port(ws, port)) {
#if WS_ENABLE_COLOR
    case 0x60:
        if (ws->system_control & 2)
            ws->video_mode = value;
        break;
#endif
    case 0xa0:
        ws->system_control = (ws->system_control & 0x83) | (value & 0x0d);
        break;
    case 0xa2: ws->timer_control = value & 15; break;
    case 0xb0: ws->irq_base = value & 0xf8; break;
    case 0xb1: ws_uart_write(ws, false, value); break;
    case 0xb2:
        ws->irq_enable = value;
        ws_uart_refresh_irq(ws);
        ws_rtc_refresh_irq(ws);
        break;
    case 0xb3: ws_uart_write(ws, true, value); break;
    case 0xb6:
        ws->irq_pending &= ~value;
        ws_uart_refresh_irq(ws);
        ws_rtc_refresh_irq(ws);
        break;
    case 0xb5: ws->key_select = value & 0x70; break;
    case 0xb7: ws->nmi_enabled = (value & 0x10) != 0; break;
    case 0xc0: map_linear(ws, value); break;
    case 0xc1: map_bank(ws, 1, (ws->sram_bank & 0x300) | value); break;
    case 0xc2: map_bank(ws, 2, (ws->rom_bank[0] & 0x300) | value); break;
    case 0xc3: map_bank(ws, 3, (ws->rom_bank[1] & 0x300) | value); break;
    case 0xca: ws_rtc_write(ws, true, value); break;
    case 0xcb: ws_rtc_write(ws, false, value); break;
    case 0xcc: if (ws->mapper_2003) ws->gpo_direction = value & 15; break;
    case 0xcd: if (ws->mapper_2003) ws->gpo_data = value & 15; break;
    case 0xce: if (ws->mapper_2003) ws->rom_in_sram_window = value & 1; break;
    case 0xd1:
        if (ws->mapper_2003) map_bank(ws, 1, ((value & 3) << 8) | (ws->sram_bank & 255));
        break;
    case 0xd3:
        if (ws->mapper_2003) map_bank(ws, 2, ((value & 3) << 8) | (ws->rom_bank[0] & 255));
        break;
    case 0xd5:
        if (ws->mapper_2003) map_bank(ws, 3, ((value & 3) << 8) | (ws->rom_bank[1] & 255));
        break;
    }
}
