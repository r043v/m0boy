#include "ws.h"

static uint16_t set_byte(uint16_t word, unsigned high, uint8_t value)
{
    unsigned shift = high * 8;
    return (word & ~(255u << shift)) | ((unsigned)value << shift);
}

uint8_t ws_eeprom_read(const WsEeprom *chip, unsigned reg, bool internal)
{
    if (!chip->size)
        return 0xff;
    if (reg < 2)
        return chip->read_data >> (reg * 8);
    if (reg < 4)
        return chip->command >> ((reg - 2) * 8);
    return 2 | chip->read_done | (internal && chip->write_protected ? 0x80 : 0);
}

void ws_eeprom_write(WsEeprom *chip, unsigned reg, uint8_t value, bool internal)
{
    if (!chip->size)
        return;
    if (reg < 2) {
        chip->write_data = set_byte(chip->write_data, reg, value);
        return;
    }
    if (reg < 4) {
        chip->command = set_byte(chip->command, reg - 2, value);
        return;
    }

    unsigned action = value >> 4;
    if (action == 8) {
        if (internal)
            chip->write_protected = true;
        return;
    }

    unsigned bits = chip->size == 128 ? 6 : 10;
    unsigned prefix = chip->command >> bits;
    if (!(prefix & 4))
        return;
    unsigned opcode = prefix & 3;
    unsigned mode = (chip->command >> (bits - 2)) & 3;
    unsigned offset = (chip->command * 2u) & (chip->size - 1);
    if (action == 1) {
        chip->read_data = opcode == 2
            ? chip->data[offset] | ((unsigned)chip->data[offset + 1] << 8)
            : 0xffff;
        chip->read_done = true;
        return;
    }
    if (action == 4 && opcode == 0 && (mode == 0 || mode == 3)) {
        chip->write_enabled = mode == 3;
        return;
    }
    if (!chip->write_enabled)
        return;

    bool write = action == 2 && (opcode == 1 || (!opcode && mode == 1));
    bool erase = action == 4 && (opcode == 3 || (!opcode && mode == 2));
    if (!write && !erase)
        return;

    unsigned limit = internal && chip->write_protected ? 96 : chip->size;
    unsigned start = opcode ? offset : 0;
    unsigned end = opcode && offset + 2 < limit ? offset + 2 : limit;
    uint16_t word = write ? chip->write_data : 0xffff;
    for (unsigned i = start; i < end; i += 2) {
        if (chip->data[i] != (uint8_t)word || chip->data[i + 1] != (word >> 8)) {
            chip->data[i] = word;
            chip->data[i + 1] = word >> 8;
            chip->dirty = true;
        }
    }
}
