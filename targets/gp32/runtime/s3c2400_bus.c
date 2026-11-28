#include "s3c2400_bus.h"

#ifdef M0_GP32_RAM_PROFILE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t gp32_ram_uses[GP32_RAM_PAGES];

uint8_t gp32_ram_shown[GP32_RAM_PAGES];

/* "page uses shown", one line per page reached, when the program ends, into the file
   which the environment variable GP32_RAM_PROFILE names. */
static __attribute__((destructor)) void write_profile(void)
{
    const char *path = getenv("GP32_RAM_PROFILE");
    FILE *file = path ? fopen(path, "w") : NULL;
    if (!file)
        return;
    for (unsigned page = 0; page < GP32_RAM_PAGES; ++page)
        if (gp32_ram_uses[page])
            fprintf(file, "%u %" PRIu64 " %u\n", page, gp32_ram_uses[page], gp32_ram_shown[page]);
    fclose(file);
}
#endif

#ifdef M0_GP32_PAGED_RAM
uintptr_t gp32_ram_page[GP32_RAM_PAGES];
#endif

static uint32_t width_mask(uint8_t width)
{
    switch (width) {
    case 1: return UINT32_C(0xff);
    case 2: return UINT32_C(0xffff);
    case 4: return UINT32_MAX;
    default: return 0;
    }
}

static bool contains(uint32_t offset, uint8_t width, size_t size)
{
    return offset < size && width <= size - offset;
}

static S3c2400Access decode(const S3c2400Bus *bus, S3c2400AccessKind kind,
                            uint32_t address, uint8_t width)
{
    S3c2400Access access = { .address = address, .kind = kind, .width = width };
    if (!width_mask(width))
        return access;

    if (bus->boot_rom && contains(address, width, S3C2400_BANK_SIZE) &&
        contains(address, width, bus->boot_rom_size)) {
        access.region = S3C2400_BOOT_ROM;
        access.offset = address;
    } else if (bus->ram && address >= S3C2400_SDRAM_BASE &&
               contains(address - S3C2400_SDRAM_BASE, width, S3C2400_BANK_SIZE) &&
               contains(address - S3C2400_SDRAM_BASE, width, bus->ram_size)) {
        access.region = S3C2400_SDRAM;
        access.offset = address - S3C2400_SDRAM_BASE;
    } else if (bus->absent_sdram_open_bus && address >= S3C2400_SDRAM_BASE &&
               contains(address - S3C2400_SDRAM_BASE, width, 2 * S3C2400_BANK_SIZE)) {
        access.region = S3C2400_OPEN_BUS;
        access.offset = address;
    } else if (address >= UINT32_C(0x14000000) &&
               address <= UINT32_C(0x1fffffff) - (width - 1u)) {
        access.region = S3C2400_MMIO;
        access.offset = address;
    }
    return access;
}

static GP32_HOT S3c2400Access read_bus(const S3c2400Bus *bus, S3c2400AccessKind kind,
                              uint32_t address, uint8_t width, bool inspect)
{
    S3c2400Access access = decode(bus, kind, address, width);
    const uint8_t *memory = NULL;
    switch (access.region) {
    case S3C2400_BOOT_ROM:
        memory = bus->boot_rom;
        break;
    case S3C2400_SDRAM:
        memory = bus->ram;
        break;
    case S3C2400_MMIO:
        if (bus->mmio.read)
            access.accepted = bus->mmio.read(bus->mmio.context, address, width,
                                             !inspect, &access.value);
        break;
    case S3C2400_OPEN_BUS:
        access.value = UINT32_MAX;
        access.accepted = true;
        break;
    case S3C2400_UNMAPPED:
        if (bus->absent_boot_rom_id && !bus->boot_rom && kind == S3C2400_LOAD &&
            address >= S3C2400_FIRMWARE_ID && width_mask(width) &&
            contains(address - S3C2400_FIRMWARE_ID, width, 8)) {
            uint32_t offset = address - S3C2400_FIRMWARE_ID;
            access.value = bus->absent_boot_rom_id[offset / 4] >> (8u * (offset % 4));
            access.accepted = true;
        } else if (bus->absent_boot_rom_stores && !bus->boot_rom && kind == S3C2400_LOAD &&
                   width_mask(width) && contains(address, width, S3C2400_BANK_SIZE)) {
            access.accepted = true;     /* reads as zero */
        }
        break;
    }

    if (memory) {
        const uint8_t *at = access.region == S3C2400_SDRAM
            ? GP32_RAM(memory, access.offset) : memory + access.offset;
        if (access.region == S3C2400_SDRAM && !(access.offset & (width - 1u)))
            access.value = gp32_load(at, width);
        else if (width == 4)
            access.value = (uint32_t)at[0] | (uint32_t)at[1] << 8 |
                           (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        else if (width == 2)
            access.value = (uint32_t)at[0] | (uint32_t)at[1] << 8;
        else
            access.value = at[0];
        access.accepted = true;
    }
    access.value = access.accepted ? access.value & width_mask(width) : 0;
    if (!inspect) {
        if (access.accepted && access.region == S3C2400_MMIO &&
            kind == S3C2400_LOAD && M0_OBSERVER(bus->hooks.probe))
            access.value = bus->hooks.probe(bus->hooks.context, &access) & width_mask(width);
        if (M0_OBSERVER(bus->hooks.access))
            bus->hooks.access(bus->hooks.context, &access);
    }
    return access;
}

S3c2400Access s3c2400_bus_read(const S3c2400Bus *bus, S3c2400AccessKind kind,
                               uint32_t address, uint8_t width)
{
    return read_bus(bus, kind, address, width, false);
}

S3c2400Access s3c2400_bus_peek(const S3c2400Bus *bus, uint32_t address, uint8_t width)
{
    return read_bus(bus, S3C2400_LOAD, address, width, true);
}

GP32_HOT S3c2400Access s3c2400_bus_write(const S3c2400Bus *bus, S3c2400AccessKind kind,
                                uint32_t address, uint8_t width, uint32_t value)
{
    S3c2400Access access = decode(bus, kind, address, width);
    access.value = value & width_mask(width);
    if (access.region == S3C2400_SDRAM) {
        if (width && !(access.offset & (width - 1u)))
            gp32_store(GP32_RAM(bus->ram, access.offset), access.value, width);
        else
            for (unsigned i = 0; i < width; ++i)
                GP32_RAM(bus->ram, access.offset)[i] = (uint8_t)(access.value >> (8u * i));
        access.accepted = true;
    } else if (access.region == S3C2400_OPEN_BUS) {
        access.accepted = true;
    } else if (access.region == S3C2400_MMIO && bus->mmio.write) {
        access.accepted = bus->mmio.write(bus->mmio.context, address, width, access.value);
    } else if (bus->absent_boot_rom_stores && !bus->boot_rom && width_mask(width) &&
               contains(address, width, S3C2400_BANK_SIZE)) {
        access.accepted = true;
    }
    if (M0_OBSERVER(bus->hooks.access))
        bus->hooks.access(bus->hooks.context, &access);
    return access;
}

bool s3c2400_bus_dma_unit(const S3c2400Bus *bus, uint32_t source,
                          uint32_t destination, uint8_t width)
{
    /* The audio port's path, from SDRAM to a device with no observer: the same
       two accesses without their descriptions. */
    uint32_t offset = source - S3C2400_SDRAM_BASE;
    if (bus->ram && bus->mmio.write && !M0_OBSERVER(bus->hooks.access) && !M0_OBSERVER(bus->hooks.copy) &&
        (width == 2 || width == 4) &&
        contains(offset, width, S3C2400_BANK_SIZE) &&
        contains(offset, width, bus->ram_size) &&
        destination >= UINT32_C(0x14000000) &&
        destination <= UINT32_C(0x1fffffff) - (width - 1u)) {
        const uint8_t *at = GP32_RAM(bus->ram, offset);
        uint32_t value;
        if (!(offset & (width - 1u))) {
            value = gp32_load(at, width);
        } else {
            value = (uint32_t)at[0] | (uint32_t)at[1] << 8;
            if (width == 4)
                value |= (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        }
        return bus->mmio.write(bus->mmio.context, destination, width, value);
    }

    S3c2400Access read = s3c2400_bus_read(bus, S3C2400_DMA_LOAD, source, width);
    if (!read.accepted)
        return false;

    S3c2400Access write = s3c2400_bus_write(bus, S3C2400_DMA_STORE, destination,
                                           width, read.value);
    if (write.accepted && M0_OBSERVER(bus->hooks.copy))
        bus->hooks.copy(bus->hooks.context, &read, &write);
    return write.accepted;
}
