#include "arm920t.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); \
} } while (0)

typedef struct {
    unsigned events, predicates;
    Arm920tTrace last;
    Arm920tAccess last_access;
    bool force;
} Events;

static void trace(void *context, const Arm920tTrace *event)
{
    Events *events = context;
    ++events->events;
    events->last = *event;
}

static void access(void *context, const Arm920tAccess *event)
{
    Events *events = context;
    events->last_access = *event;
}

static bool predicate(void *context, const Arm920tTrace *event)
{
    Events *events = context;
    ++events->predicates;
    return events->force || event->executed;
}

static void put(uint8_t *data, unsigned offset, unsigned width, uint32_t value)
{
    for (unsigned i = 0; i < width; ++i)
        data[offset + i] = (uint8_t)(value >> (8 * i));
}

static void instruction(Arm920t *cpu)
{
    CHECK(arm920t_step(cpu) == ARM920T_INSTRUCTION);
}

static uint32_t peek(const S3c2400Bus *bus, uint32_t address)
{
    S3c2400Access entry = s3c2400_bus_peek(bus, address, 4);
    CHECK(entry.accepted);
    return entry.value;
}

static void cache_test(void)
{
    const uint32_t ram_base = S3C2400_SDRAM_BASE;
    uint8_t rom[128] = {0}, ram[0x8000] = {0};
    S3c2400Bus bus = {.boot_rom = rom, .boot_rom_size = sizeof rom,
                     .ram = ram, .ram_size = sizeof ram};
    Events events = {0};
    Arm920t cpu, snapshot;
    arm920t_reset(&cpu, (Arm920tConfig){.bus = &bus, .context = &events,
                                      .access = access}, false);
    cpu.ttbr = ram_base + 0x4000;
    cpu.domains = 1;
    cpu.control |= 0x5005;
    cpu.r[2] = 0x00100000;
    cpu.r[3] = 0x00100020;
    put(ram, 0x4000, 4, 0x00000c02); /* Uncached ROM. */
    put(ram, 0x4004, 4, ram_base | 0xc0e); /* Cached write-back RAM. */
    put(ram, 0, 4, 0x12345678);
    put(ram, 0x20, 4, 0xe3a06001); /* MOV r6, #1 */
    put(rom, 0, 4, 0xe5924000); /* LDR r4, [r2] */
    put(rom, 4, 4, 0xe5825000); /* STR r5, [r2] */
    put(rom, 8, 4, 0xee072f3a); /* Clean D line by MVA. */
    put(rom, 12, 4, 0xee070f9a); /* Drain write buffer. */
    put(rom, 16, 4, 0xe5835000); /* STR r5, [r3] */
    put(rom, 20, 4, 0xee073f35); /* Invalidate I line by MVA. */
    put(rom, 24, 4, 0xee070f90); /* Wait for interrupt. */
    put(rom, 28, 4, 0xee093f30); /* Set I cache lockdown. */
    put(rom, 32, 4, 0xee194f30); /* Read I cache lockdown. */
    instruction(&cpu);
    CHECK(cpu.r[4] == 0x12345678 && !events.last_access.cache_hit);
    CHECK(events.last_access.virtual_address == cpu.r[2] &&
          events.last_access.physical == ram_base);
    cpu.r[5] = 0xdeadbeef;
    instruction(&cpu);
    CHECK(events.last_access.cache_hit && !events.last_access.buffered);
    CHECK(peek(&bus, ram_base) == 0x12345678);
    snapshot = cpu;
    cpu.r[15] = 0;
    instruction(&cpu);
    CHECK(cpu.r[4] == 0xdeadbeef && events.last_access.cache_hit);
    cpu = snapshot;
    instruction(&cpu);
    CHECK(cpu.write_count == 4 && peek(&bus, ram_base) == 0x12345678);
    instruction(&cpu);
    CHECK(!cpu.write_count && peek(&bus, ram_base) == 0xdeadbeef);

    cpu.r[15] = cpu.r[3];
    instruction(&cpu);
    CHECK(cpu.r[6] == 1 && !events.last_access.cache_hit);
    cpu.r[15] = 16;
    cpu.r[5] = 0xe3a06002;
    instruction(&cpu);
    CHECK(!events.last_access.cache_hit && events.last_access.buffered);
    CHECK(peek(&bus, ram_base + 0x20) == 0xe3a06001);
    cpu.r[15] = cpu.r[3];
    instruction(&cpu);
    CHECK(cpu.r[6] == 1 && events.last_access.cache_hit);
    CHECK(peek(&bus, ram_base + 0x20) == 0xe3a06002);
    cpu.r[15] = 20;
    instruction(&cpu);
    cpu.r[15] = cpu.r[3];
    instruction(&cpu);
    CHECK(cpu.r[6] == 2 && !events.last_access.cache_hit);
    cpu.r[15] = 24;
    instruction(&cpu);
    CHECK(arm920t_step(&cpu) == ARM920T_WAITING);
    cpu.irq = true; /* A masked IRQ wakes WFI without taking the exception. */
    cpu.r[3] = 1u << 26;
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[4] == (1u << 26) && cpu.cache[0].victim[1] == 1);
}

static void tlb_lockdown_test(void)
{
    const uint32_t ram_base = S3C2400_SDRAM_BASE;
    uint8_t rom[32] = {0}, ram[0x8000] = {0};
    S3c2400Bus bus = {.boot_rom = rom, .boot_rom_size = sizeof rom,
                     .ram = ram, .ram_size = sizeof ram};
    Arm920t cpu;
    arm920t_reset(&cpu, (Arm920tConfig){.bus = &bus}, false);
    cpu.ttbr = ram_base + 0x4000;
    cpu.control |= 1;
    cpu.domains = 1;
    cpu.r[0] = 1; /* Preserve the next DTLB fill. */
    cpu.r[2] = 0x00100000;
    cpu.r[3] = 0x04100000; /* Base 1, victim 1, no preserve. */
    put(rom, 0, 4, 0xee0a0f10);
    put(rom, 4, 4, 0xe5924000);
    put(rom, 8, 4, 0xee0a3f10);
    put(rom, 12, 4, 0xee080f16); /* Invalidate all unpreserved DTLB entries. */
    put(rom, 16, 4, 0xe5924000);
    put(rom, 20, 4, 0xee082f36); /* Invalidate the preserved entry by MVA. */
    put(rom, 24, 4, 0xe5924000);
    put(rom, 28, 4, 0xee1a5f10);
    put(ram, 0x4000, 4, 0xc02);
    put(ram, 0x4004, 4, ram_base | 0x401);
    put(ram, 0x400, 4, (ram_base + 0x1000) | 0xff2);
    put(ram, 0x1000, 4, 1);
    put(ram, 0x2000, 4, 2);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[4] == 1);
    instruction(&cpu);
    put(ram, 0x400, 4, (ram_base + 0x2000) | 0xff2);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[4] == 1);
    instruction(&cpu);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[4] == 2 && cpu.r[5] == 0x04200000);
}

static bool rejected_mmio_read(void *context, uint32_t address, uint8_t width,
                               bool side_effects, uint32_t *value)
{
    (void)context;
    (void)address;
    (void)width;
    (void)side_effects;
    (void)value;
    return false;
}

static bool record_mmio_write(void *context, uint32_t address, uint8_t width, uint32_t value)
{
    CHECK(address == 0x14000000 && width == 4);
    *(uint32_t *)context = value;
    return true;
}

static void data_abort_test(void)
{
    uint8_t rom[8] = {0}, ram[4] = {0};
    uint32_t written = 0;
    S3c2400Bus bus = {.boot_rom = rom, .boot_rom_size = sizeof rom,
                     .ram = ram, .ram_size = sizeof ram,
                     .mmio = {rejected_mmio_read, record_mmio_write, &written}};
    Arm920t cpu;
    arm920t_reset(&cpu, (Arm920tConfig){.bus = &bus}, false);
    put(rom, 0, 4, 0xe1020091); /* SWP r0, r1, [r2] */
    put(rom, 4, 4, 0xe8b00003); /* LDMIA r0!, {r0, r1} */
    put(ram, 0, 4, 0xdeadbeef);
    cpu.r[0] = 1;
    cpu.r[1] = 0x12345678;
    cpu.r[2] = 0x14000000;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.r[0] == 1 && cpu.far == cpu.r[2] && cpu.dfsr == 8);
    CHECK(written == 0x12345678);
    cpu.r[15] = 4;
    cpu.r[0] = S3C2400_SDRAM_BASE;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.r[0] == S3C2400_SDRAM_BASE && cpu.far == S3C2400_SDRAM_BASE + 4);
}

int main(void)
{
    uint8_t rom[128] = {0}, ram[0x8000] = {0};
    S3c2400Bus bus = {.boot_rom = rom, .boot_rom_size = sizeof rom,
                     .ram = ram, .ram_size = sizeof ram};
    Events events = {0};
    Arm920tConfig config = {.bus = &bus, .trace = trace, .context = &events,
                            .predicate = predicate};
    Arm920t cpu;

    put(rom, 0, 4, 0xe3a00001);  /* MOV r0, #1 */
    put(rom, 4, 4, 0xe3500000);  /* CMP r0, #0 */
    put(rom, 8, 4, 0x03a010ff);  /* MOVEQ r1, #255 */
    put(rom, 12, 4, 0xe12fff12); /* BX r2 */
    put(rom, 24, 4, 0xe25ef004); /* SUBS pc, lr, #4 */
    put(rom, 32, 2, 0x2105);     /* MOVS r1, #5 */
    put(rom, 34, 2, 0xd000);     /* BEQ +0 */
    put(rom, 36, 2, 0x3103);     /* ADDS r1, #3 */
    put(rom, 38, 2, 0xb402);     /* PUSH {r1} */
    put(rom, 40, 2, 0xbc08);     /* POP {r3} */
    arm920t_reset(&cpu, config, false);
    cpu.r[2] = 33;
    cpu.r[13] = S3C2400_SDRAM_BASE + 0x100;
    instruction(&cpu);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[1] == 0 && !events.last.executed && events.last.pc == 8);
    events.force = true;
    cpu.r[15] = 8;
    instruction(&cpu);
    CHECK(cpu.r[1] == 255 && events.last.executed);
    events.force = false;
    instruction(&cpu);
    CHECK(cpu.r[15] == 32 && (cpu.cpsr & 32));
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[15] == 36 && !events.last.executed && events.last.thumb);
    instruction(&cpu);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[3] == 8 && cpu.r[13] == S3C2400_SDRAM_BASE + 0x100);
    CHECK(events.predicates == 3);

    cpu.cpsr &= ~128u;
    cpu.irq = true;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.r[15] == 24 && cpu.r[14] == 46 && (cpu.cpsr & 63) == 18);
    cpu.irq = false;
    instruction(&cpu);
    CHECK(cpu.r[15] == 42 && (cpu.cpsr & 63) == 51);
    CHECK(cpu.r[13] == S3C2400_SDRAM_BASE + 0x100);

    put(rom, 0, 4, 0xee110f10); /* MRC p15, 0, r0, c1, c0, 0 */
    put(rom, 4, 4, 0xee020f10); /* MCR p15, 0, r0, c2, c0, 0 */
    put(rom, 8, 4, 0xee033f10); /* MCR p15, 0, r3, c3, c0, 0 */
    put(rom, 12, 4, 0xee011f10); /* MCR p15, 0, r1, c1, c0, 0 */
    put(rom, 16, 4, 0xe5924000); /* LDR r4, [r2] */
    put(rom, 20, 4, 0xe5824000); /* STR r4, [r2] */
    put(ram, 0x4000, 4, 0x00000c02); /* Identity-mapped ROM section */
    put(ram, 0x4004, 4, S3C2400_SDRAM_BASE | 0x421); /* Coarse table, domain 1 */
    put(ram, 0x400, 4, (S3C2400_SDRAM_BASE + 0x1000) | 0xff2);
    put(ram, 0x1000, 4, 0x12345678);
    arm920t_reset(&cpu, config, false);
    instruction(&cpu);
    CHECK(cpu.r[0] == 0x78);
    cpu.r[0] = S3C2400_SDRAM_BASE + 0x4000;
    cpu.r[1] = 0x7b;
    cpu.r[2] = 0x00100000;
    cpu.r[3] = 5;
    instruction(&cpu);
    instruction(&cpu);
    instruction(&cpu);
    instruction(&cpu);
    CHECK(cpu.r[4] == 0x12345678 && events.last.physical == 16);
    cpu.r[2]++;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.dfsr == 1 && cpu.far == 0x00100001 && cpu.r[14] == 28);

    cpu.r[15] = 16;
    cpu.r[2] = 0x00100000;
    cpu.domains = 1;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.dfsr == 0x1b && cpu.far == 0x00100000);
    cpu.r[15] = 0x00200000;
    CHECK(arm920t_step(&cpu) == ARM920T_EXCEPTION);
    CHECK(cpu.ifsr == 5 && cpu.r[15] == 12);

    /* Cache a data translation, change the descriptor, then invalidate it. */
    put(rom, 32, 4, 0xee082f36); /* MCR p15, 0, r2, c8, c6, 1 */
    put(ram, 0x400, 4, (S3C2400_SDRAM_BASE + 0x2000) | 0xff2);
    put(ram, 0x2000, 4, 0xaabbccdd);
    cpu.domains = 5;
    cpu.r[15] = 16;
    instruction(&cpu);
    CHECK(cpu.r[4] == 0x12345678);
    cpu.r[15] = 32;
    instruction(&cpu);
    cpu.r[15] = 16;
    instruction(&cpu);
    CHECK(cpu.r[4] == 0xaabbccdd);

    cache_test();
    tlb_lockdown_test();
    data_abort_test();
    return 0;
}
