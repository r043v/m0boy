#include "ws.h"

enum { IDLE, UNLOCK_2, COMMAND, PROGRAM, ERASE_UNLOCK_1, ERASE_UNLOCK_2, ERASE_COMMAND };

bool ws_attach_flash(Ws *ws, WsFlashStore store, void *context)
{
    if (!store || !ws->mapper_2003 || ws->rom.size != WS_FLASH_SIZE)
        return false;
    ws->flash = (WsFlash){.store = store, .context = context};
    return true;
}

uint8_t ws_flash_read(const Ws *ws, uint32_t offset, bool word_bus)
{
    if (ws->flash.identify) {
        switch (offset & 0xfff) {
        case 0: return 0x04;
        case 2: return 0x0c;
        case 3: return word_bus ? 0x22 : 0;
        default: return 0;
        }
    }
    // Embedded operations complete synchronously in this model.
    return 4 | (ws->rom.read(ws->rom.context, offset) & 0x80);
}

static void erase_sector(WsFlash *flash, uint32_t offset)
{
    static const uint32_t boundaries[] = {
        0x00000, 0x10000, 0x20000, 0x30000, 0x40000, 0x50000,
        0x60000, 0x64000, 0x6c000, 0x6e000, 0x70000, 0x72000,
        0x74000, 0x7c000, WS_FLASH_SIZE
    };
    unsigned sector = 0;
    while (offset >= boundaries[sector + 1])
        ++sector;
    flash->store(flash->context, boundaries[sector],
                 boundaries[sector + 1] - boundaries[sector], 0xff);
}

void ws_flash_write(Ws *ws, uint32_t offset, uint8_t value)
{
    WsFlash *flash = &ws->flash;
    if (offset >= WS_FLASH_SIZE)
        return;
    unsigned phase = flash->phase;
    unsigned command_address = offset & 0xfff;
    flash->phase = IDLE;
    if (phase == PROGRAM) {
        uint8_t previous = ws->rom.read(ws->rom.context, offset);
        flash->store(flash->context, offset, 1, previous & value);
        return;
    }
    if (flash->fast) {
        if (flash->exit_fast && (value == 0xf0 || value == 0))
            flash->fast = false;
        if (value == 0xa0)
            flash->phase = PROGRAM;
        flash->exit_fast = value == 0x90;
        return;
    }
    if (value == 0xf0) {
        flash->identify = false;
        return;
    }

    switch (phase) {
    case IDLE:
        if (command_address == 0xaaa && value == 0xaa)
            flash->phase = UNLOCK_2;
        break;
    case UNLOCK_2:
    case ERASE_UNLOCK_2:
        if (command_address == 0x555 && value == 0x55)
            flash->phase = phase == UNLOCK_2 ? COMMAND : ERASE_COMMAND;
        break;
    case COMMAND:
        if (command_address != 0xaaa)
            break;
        if (value == 0x90)
            flash->identify = true;
        else if (value == 0xa0)
            flash->phase = PROGRAM;
        else if (value == 0x80)
            flash->phase = ERASE_UNLOCK_1;
        else if (value == 0x20)
            flash->fast = true;
        break;
    case ERASE_UNLOCK_1:
        if (command_address == 0xaaa && value == 0xaa)
            flash->phase = ERASE_UNLOCK_2;
        break;
    case ERASE_COMMAND:
        if (value == 0x30)
            erase_sector(flash, offset);
        else if (command_address == 0xaaa && value == 0x10)
            flash->store(flash->context, 0, WS_FLASH_SIZE, 0xff);
        break;
    }
}
