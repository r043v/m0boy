#include "s3c2400_bus.h"

#include <stdio.h>
#include <stdlib.h>

static void check(bool condition, unsigned line)
{
    if (!condition) {
        fprintf(stderr, "Bus test failed at line %u\n", line);
        exit(EXIT_FAILURE);
    }
}
#define CHECK(condition) check((condition), __LINE__)

#define SRCPND UINT32_C(0x14400000)

typedef struct {
    unsigned access_count, probe_count, copy_count, device_reads;
    S3c2400Access last;
} TestState;

static bool mmio_read(void *context, uint32_t address, uint8_t width,
                      bool side_effects, uint32_t *value)
{
    TestState *state = context;
    if (address != SRCPND || width != 4)
        return false;
    state->device_reads += side_effects;
    *value = 1;
    return true;
}

static uint32_t probe(void *context, const S3c2400Access *access)
{
    TestState *state = context;
    CHECK(access->address == SRCPND && access->value == 1);
    CHECK(state->device_reads == 1);
    ++state->probe_count;
    return 2;
}

static void observe(void *context, const S3c2400Access *access)
{
    TestState *state = context;
    ++state->access_count;
    state->last = *access;
}

static void copy(void *context, const S3c2400Access *source, const S3c2400Access *target)
{
    TestState *state = context;
    CHECK(source->kind == S3C2400_DMA_LOAD && target->kind == S3C2400_DMA_STORE);
    CHECK(state->last.kind == S3C2400_DMA_STORE);
    CHECK(source->region == S3C2400_BOOT_ROM && source->offset == 0);
    CHECK(target->region == S3C2400_SDRAM && target->offset == 2);
    CHECK(source->value == target->value && target->width == 2);
    ++state->copy_count;
}

int main(void)
{
    const uint8_t rom[] = { 0x78, 0x56, 0x34, 0x12 };
    uint8_t ram[4] = { 0 };
    TestState state = { 0 };
    const S3c2400Bus bus = {
        .boot_rom = rom, .boot_rom_size = sizeof(rom),
        .ram = ram, .ram_size = sizeof(ram),
        .mmio = { .read = mmio_read, .context = &state },
        .hooks = { .probe = probe, .access = observe, .copy = copy, .context = &state }
    };

    CHECK(s3c2400_bus_peek(&bus, SRCPND, 4).value == 1);
    CHECK(state.device_reads == 0 && state.probe_count == 0 && state.access_count == 0);
    CHECK(s3c2400_bus_read(&bus, S3C2400_FETCH, 0, 4).value == UINT32_C(0x12345678));
    CHECK(state.last.kind == S3C2400_FETCH && state.last.region == S3C2400_BOOT_ROM);
    CHECK(s3c2400_bus_read(&bus, S3C2400_LOAD, SRCPND, 4).value == 2);
    CHECK(state.last.value == 2 && state.probe_count == 1 && state.device_reads == 1);
    CHECK(!s3c2400_bus_read(&bus, S3C2400_LOAD, SRCPND, 1).accepted);
    CHECK(!s3c2400_bus_read(&bus, S3C2400_LOAD, UINT32_MAX, 4).accepted);
    CHECK(!s3c2400_bus_read(&bus, S3C2400_LOAD, 0, 3).accepted);
    CHECK(!s3c2400_bus_write(&bus, S3C2400_STORE, 0, 1, 0).accepted);

    CHECK(s3c2400_bus_dma_unit(&bus, 0, S3C2400_SDRAM_BASE + 2, 2));
    CHECK(state.copy_count == 1 && ram[2] == 0x78 && ram[3] == 0x56);
    CHECK(!s3c2400_bus_dma_unit(&bus, 0, S3C2400_SDRAM_BASE + 3, 2));
    CHECK(!s3c2400_bus_dma_unit(&bus, 3, S3C2400_SDRAM_BASE, 2));
    CHECK(state.copy_count == 1 && ram[3] == 0x56);

    const S3c2400Bus no_rom = { .ram = ram, .ram_size = sizeof(ram),
                                .absent_boot_rom_stores = true };
    CHECK(s3c2400_bus_write(&no_rom, S3C2400_STORE, 0, 4, 1).accepted);
    CHECK(s3c2400_bus_read(&no_rom, S3C2400_LOAD, 0x11, 1).accepted);
    CHECK(s3c2400_bus_read(&no_rom, S3C2400_LOAD, 0x11, 1).value == 0);
    CHECK(!s3c2400_bus_read(&no_rom, S3C2400_LOAD, S3C2400_BANK_SIZE - 2, 4).accepted);
    CHECK(!s3c2400_bus_read(&no_rom, S3C2400_FETCH, 0x10, 4).accepted);
    CHECK(!s3c2400_bus_write(&no_rom, S3C2400_STORE, S3C2400_BANK_SIZE - 2, 4, 1).accepted);
    CHECK(ram[3] == 0x56);

    static const uint32_t id[2] = { UINT32_C(0x47503332), UINT32_C(0x01030507) };
    const S3c2400Bus identified = { .ram = ram, .ram_size = sizeof(ram),
                                    .absent_boot_rom_id = id };
    CHECK(s3c2400_bus_read(&identified, S3C2400_LOAD, 0x1014, 4).value == id[1]);
    CHECK(s3c2400_bus_read(&identified, S3C2400_LOAD, 0x1011, 1).value == 0x33);
    CHECK(!s3c2400_bus_read(&identified, S3C2400_FETCH, 0x1010, 4).accepted);
    CHECK(!s3c2400_bus_read(&identified, S3C2400_LOAD, 0x1016, 4).accepted);
    CHECK(!s3c2400_bus_read(&identified, S3C2400_LOAD, 0x100c, 4).accepted);
    return EXIT_SUCCESS;
}
