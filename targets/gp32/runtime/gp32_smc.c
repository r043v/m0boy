#include "gp32_smc.h"

#include <string.h>

enum { IDLE, READ, PROGRAM, ERASE, STATUS, ID, UID_SETUP };

/* A byte of the card, valid to the end of its 512-byte sector and until the
   next call.  A paged card lives outside memory: `image` only tells that it is
   there. */
static uint8_t *card_bytes(const Gp32Smc *card, size_t offset, bool write)
{
#ifdef M0_GP32_PAGED_CARD
    (void)card;
    return gp32_card_bytes(offset, write);
#else
    (void)write;
    return card->image + offset;
#endif
}

bool gp32_smc_attach(Gp32Smc *card, uint8_t *image, size_t size, bool writable)
{
    static const struct {
        unsigned pages, data_size, block_pages;
        uint8_t id;
    } geometry[] = {
        {2048, 256, 8, 0xa4}, {4096, 256, 8, 0x6e}, {8192, 256, 16, 0xea},
        {8192, 512, 16, 0xe3}, {16384, 512, 16, 0xe6}, {32768, 512, 32, 0x73},
        {65536, 512, 32, 0x75}, {131072, 512, 32, 0x76}, {262144, 512, 32, 0x79},
    };
    if (!image) {
        *card = (Gp32Smc){0};
        return size == 0;
    }
    for (unsigned i = 0; i < sizeof(geometry) / sizeof(*geometry); ++i) {
        unsigned data = geometry[i].data_size;
        unsigned stride = data + data / 32;
        size_t payload = (size_t)geometry[i].pages * stride;
        uint8_t first[3] = {0};
        if (size == payload + 1024) {
            const Gp32Smc raw = {.image = image};
            memcpy(first, card_bytes(&raw, 0, false), sizeof(first));
        }
        bool header = size == payload + 1024 && (first[0] == 0xec || first[0] == 0x98) &&
                      first[1] == geometry[i].id;
        if (size != payload && !header)
            continue;
        *card = (Gp32Smc){
            .image = image, .offset = header ? 1024 : 0,
            .pages = geometry[i].pages, .data_size = data, .page_size = stride,
            .block_pages = geometry[i].block_pages, .row_cycles = geometry[i].pages > 65536 ? 3 : 2,
            .id = {header ? first[0] : 0xec, geometry[i].id, header ? first[2] : 0},
            .writable = writable
        };
        return true;
    }
    return false;
}

bool gp32_smc_set_uid(Gp32Smc *card, const uint8_t uid[16])
{
    if (!card->image || card->data_size != 512)
        return false;
    memcpy(card->uid, uid, sizeof(card->uid));
    card->uid_override = true;
    card->id[2] = 0xa5;
    return true;
}

const uint8_t *gp32_smc_uid(const Gp32Smc *card)
{
    if (card->uid_override)
        return card->uid;
    return card->offset && card->id[2] == 0xa5 ? card_bytes(card, 256, false) : NULL;
}

void gp32_smc_command(Gp32Smc *c, uint8_t command, bool write_enable)
{
    switch (command) {
    case 0xff:
        c->mode = IDLE;
        c->failed = c->uid_mode = false;
        c->area = 0;
        break;
    case 0x00: case 0x01: case 0x50:
        c->area = command == 0x50 ? c->data_size : command * 256u;
        c->mode = READ;
        c->row = c->column = c->address_bytes = 0;
        break;
    case 0x80:
        c->mode = PROGRAM;
        c->row = c->column = c->address_bytes = 0;
        memset(c->program, 0xff, c->page_size);
        break;
    case 0x10:
        c->failed = c->mode != PROGRAM || c->address_bytes != 1 + c->row_cycles ||
                    c->row >= c->pages || !write_enable;
        if (!c->failed) {
            size_t page = c->offset + (size_t)c->row * c->page_size;
            for (unsigned i = 0; i < c->page_size; ++i)
                *card_bytes(c, page + i, true) &= c->program[i];
            c->dirty = true;
#ifdef M0_GP32_PAGED_CARD
            gp32_card_flush();
#endif
        }
        c->mode = IDLE;
        break;
    case 0x60:
        c->mode = ERASE;
        c->row = c->address_bytes = 0;
        break;
    case 0xd0:
        c->failed = c->mode != ERASE || c->address_bytes != c->row_cycles ||
                    c->row >= c->pages || !write_enable;
        if (!c->failed) {
            unsigned first = c->row & ~(c->block_pages - 1);    /* a power of two */
            size_t at = c->offset + (size_t)first * c->page_size;
            size_t left = (size_t)c->block_pages * c->page_size;
            while (left) {
                size_t run = 512u - (at & 511u);
                if (run > left)
                    run = left;
                memset(card_bytes(c, at, true), 0xff, run);
                at += run;
                left -= run;
            }
            c->dirty = true;
#ifdef M0_GP32_PAGED_CARD
            gp32_card_flush();
#endif
        }
        c->mode = IDLE;
        break;
    case 0x70: c->mode = STATUS; break;
    case 0x90:
        c->mode = ID;
        c->column = c->address_bytes = 0;
        break;
    case 0x30: case 0x5a:
        c->mode = command == (c->id[0] == 0x98 ? 0x5a : 0x30) ? UID_SETUP : IDLE;
        break;
    case 0x65: case 0xb5:
        c->uid_mode = c->mode == UID_SETUP && command == (c->id[0] == 0x98 ? 0xb5 : 0x65);
        break;
    default: c->mode = IDLE; break;
    }
}

void gp32_smc_address(Gp32Smc *c, uint8_t value)
{
    unsigned n = c->address_bytes;
    if (c->mode == ID) {
        if (!n) {
            c->column = value;
            ++c->address_bytes;
        }
        return;
    }
    if (c->mode == READ || c->mode == PROGRAM) {
        if (!n) {
            unsigned mask = c->area == c->data_size ? c->data_size / 32 - 1 : 255;
            c->column = (c->uid_mode && c->area == c->data_size ? 256 : c->area) +
                        (value & mask);
            ++c->address_bytes;
            return;
        }
        --n;
    } else if (c->mode != ERASE) {
        return;
    }
    if (n < c->row_cycles) {
        c->row |= (unsigned)value << (8 * n);
        ++c->address_bytes;
    }
}

void gp32_smc_write(Gp32Smc *c, uint8_t value)
{
    if (c->mode == PROGRAM && c->address_bytes == 1 + c->row_cycles &&
        c->column < c->page_size)
        c->program[c->column++] = value;
}

uint8_t gp32_smc_read(Gp32Smc *c, bool write_enable)
{
    if (c->mode == STATUS)
        return 0x40 | (write_enable ? 0x80 : 0) | c->failed;
    if (c->mode == ID) {
        if (c->column < sizeof(c->id))
            return c->id[c->column++];
        return 0;
    }
    if (c->mode != READ || c->address_bytes != 1 + c->row_cycles ||
        c->row >= c->pages || c->column >= c->page_size)
        return 0xff;
    size_t offset = (size_t)c->row * c->page_size + c->column++;
    if (c->column == c->page_size) {
        c->row = (c->row + 1) & (c->pages - 1);   /* a power of two */
        c->column = c->area == c->data_size ? c->data_size : 0;
    }
    if (!c->uid_mode)
        return *card_bytes(c, c->offset + offset, false);
    const uint8_t *uid = gp32_smc_uid(c);
    if (!uid || offset >= 272)
        return 0xff;
    if (offset >= 256)
        return c->uid_override ? (offset < 264 ? (offset & 1 ? 0xaa : 0x55) : 0xff)
                               : *card_bytes(c, 272 + offset - 256, false);
    return uid[offset % 16] ^ (offset & 16 ? 0xff : 0);
}
