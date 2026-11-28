#ifndef M0BOY_S3C2400_BUS_H
#define M0BOY_S3C2400_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The few functions a host with a small instruction cache keeps in its internal
   memory: with M0_GP32_IRAM, on an ESP32. */
#if defined(M0_GP32_IRAM) && defined(ESP_PLATFORM)
#include <esp_attr.h>
#define GP32_HOT IRAM_ATTR
#else
#define GP32_HOT
#endif

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define GP32_LITTLE_HOST 1
#else
#define GP32_LITTLE_HOST 0
#endif

/* Every host pointer to guest memory goes through GP32_RAM.  With
   M0_GP32_PAGED_RAM the guest SDRAM is scattered over the host's memories in
   16 KiB pages, so that a small host can keep some of them in its faster
   memory.  The host fills gp32_ram_page before boot. */
#define GP32_RAM_PAGE_BITS 14
#define GP32_RAM_PAGE_SIZE (1u << GP32_RAM_PAGE_BITS)
#define GP32_RAM_PAGES (0x800000u >> GP32_RAM_PAGE_BITS)
/* With M0_GP32_RAM_PROFILE (a PC build), how many times each page was reached
   through GP32_RAM: what a small host chooses its fast pages from.  Cached
   accesses of the guest do not reach it, a line or a block counts once.  What
   the LCD fetches (GP32_RAM_SHOWN) only marks the page: a frontend places the
   pages on the screen by itself. */
#ifdef M0_GP32_RAM_PROFILE
extern uint64_t gp32_ram_uses[GP32_RAM_PAGES];
extern uint8_t gp32_ram_shown[GP32_RAM_PAGES];
static inline uint32_t gp32_ram_show(uint32_t offset)
{
    gp32_ram_shown[offset >> GP32_RAM_PAGE_BITS & (GP32_RAM_PAGES - 1)] = 1;
    return offset;
}
static inline uint32_t gp32_ram_used(uint32_t offset)
{
    ++gp32_ram_uses[offset >> GP32_RAM_PAGE_BITS & (GP32_RAM_PAGES - 1)];
    return offset;
}
#else
#define gp32_ram_used(offset) (offset)
#define gp32_ram_show(offset) (offset)
#endif
#ifdef M0_GP32_PAGED_RAM
/* The host address of each page minus the page's guest offset: pages that
   follow each other in host memory hold the same value. */
extern uintptr_t gp32_ram_page[GP32_RAM_PAGES];
static inline uint8_t *gp32_ram_at(uint32_t offset)
{
    return (uint8_t *)(gp32_ram_page[offset >> GP32_RAM_PAGE_BITS] + offset);
}
#define GP32_RAM(ram, offset) ((void)(ram), gp32_ram_at(gp32_ram_used((uint32_t)(offset))))
#define GP32_RAM_SHOWN(ram, offset) ((void)(ram), gp32_ram_at(gp32_ram_show((uint32_t)(offset))))
/* Whether at most a page of bytes at `offset` is one run of host memory. */
static inline bool gp32_ram_whole(uint32_t offset, uint32_t length)
{
    return gp32_ram_page[offset >> GP32_RAM_PAGE_BITS] ==
           gp32_ram_page[(offset + length - 1) >> GP32_RAM_PAGE_BITS];
}
static inline size_t gp32_ram_run(uint32_t offset, size_t length)
{
    size_t left = GP32_RAM_PAGE_SIZE - (offset & (GP32_RAM_PAGE_SIZE - 1));
    return length < left ? length : left;
}
#else
#define GP32_RAM(ram, offset) ((ram) + gp32_ram_used(offset))
#define GP32_RAM_SHOWN(ram, offset) ((ram) + gp32_ram_show(offset))
static inline bool gp32_ram_whole(uint32_t offset, uint32_t length)
{
    (void)offset; (void)length;
    return true;
}
static inline size_t gp32_ram_run(uint32_t offset, size_t length)
{
    (void)offset;
    return length;
}
#endif

/* Bulk accesses of any length.  A copy goes forward: overlapping ranges
   need the destination below the source. */
static inline void gp32_ram_copy_in(uint8_t *ram, uint32_t to, const uint8_t *from, size_t length)
{
    while (length) {
        size_t n = gp32_ram_run(to, length);
        memmove(GP32_RAM(ram, to), from, n);
        to += (uint32_t)n; from += n; length -= n;
    }
}

static inline void gp32_ram_fill(uint8_t *ram, uint32_t to, int value, size_t length)
{
    while (length) {
        size_t n = gp32_ram_run(to, length);
        memset(GP32_RAM(ram, to), value, n);
        to += (uint32_t)n; length -= n;
    }
}

static inline void gp32_ram_copy(uint8_t *ram, uint32_t to, uint32_t from, size_t length)
{
    while (length) {
        size_t n = gp32_ram_run(from, gp32_ram_run(to, length));
        memmove(GP32_RAM(ram, to), GP32_RAM(ram, from), n);
        to += (uint32_t)n; from += (uint32_t)n; length -= n;
    }
}

/* A guest access of 1, 2 or 4 bytes at a host address aligned to its width.
   The guest is little endian and the host's copy of its memory word aligned
   (every page of it with M0_GP32_PAGED_RAM): a little endian host moves an
   aligned access in one piece.  The alignment is stated for compilers which
   would otherwise move it byte by byte, as they do for Xtensa. */
static inline uint32_t gp32_load(const uint8_t *at, unsigned width)
{
#if GP32_LITTLE_HOST && defined(__GNUC__)
    if (width == 4) {
        uint32_t word;
        memcpy(&word, __builtin_assume_aligned(at, 4), 4);
        return word;
    }
    if (width == 2) {
        uint16_t half;
        memcpy(&half, __builtin_assume_aligned(at, 2), 2);
        return half;
    }
    return at[0];
#else
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= (uint32_t)at[i] << (8u * i);
    return value;
#endif
}

static inline void gp32_store(uint8_t *at, uint32_t value, unsigned width)
{
#if GP32_LITTLE_HOST && defined(__GNUC__)
    if (width == 4) {
        memcpy(__builtin_assume_aligned(at, 4), &value, 4);
    } else if (width == 2) {
        uint16_t half = (uint16_t)value;
        memcpy(__builtin_assume_aligned(at, 2), &half, 2);
    } else {
        at[0] = (uint8_t)value;
    }
#else
    for (unsigned i = 0; i < width; ++i)
        at[i] = (uint8_t)(value >> (8u * i));
#endif
}

typedef enum {
    S3C2400_FETCH, S3C2400_LOAD, S3C2400_STORE,
    S3C2400_DMA_LOAD, S3C2400_DMA_STORE, S3C2400_LCD_LOAD, S3C2400_PAGE_TABLE_LOAD,
    S3C2400_ICACHE_FILL, S3C2400_DCACHE_FILL, S3C2400_CACHE_WRITEBACK
} S3c2400AccessKind;

typedef enum {
    S3C2400_UNMAPPED, S3C2400_BOOT_ROM, S3C2400_SDRAM, S3C2400_MMIO, S3C2400_OPEN_BUS
} S3c2400Region;

typedef struct {
    uint32_t address, value;
    uint32_t offset;
    uint8_t width;
    S3c2400AccessKind kind;
    S3c2400Region region;
    bool accepted;
} S3c2400Access;

typedef struct {
    /* A probe can replace a successful CPU MMIO read after device side
       effects. The observer receives the value actually delivered. */
    uint32_t (*probe)(void *context, const S3c2400Access *access);
    void (*access)(void *context, const S3c2400Access *access);
    /* A copy event follows the corresponding successful DMA store. */
    void (*copy)(void *context, const S3c2400Access *source,
                 const S3c2400Access *destination);
    void *context;
} S3c2400Hooks;

typedef struct {
    /* Inspection calls read with side_effects=false. A rejected access
       must not modify the device. Bus addresses are physical. */
    bool (*read)(void *context, uint32_t address, uint8_t width,
                 bool side_effects, uint32_t *value);
    bool (*write)(void *context, uint32_t address, uint8_t width, uint32_t value);
    void *context;
} S3c2400Mmio;

typedef struct {
    /* The caller owns the image, RAM, callback contexts and machine state. */
    const uint8_t *boot_rom;
    size_t boot_rom_size;
    uint8_t *ram;
    size_t ram_size;
    S3c2400Mmio mmio;
    S3c2400Hooks hooks;
    bool absent_sdram_open_bus;
    /* Without a boot ROM image, stores to bank 0 are accepted and dropped, as
       the real ROM drops them, and data loads read zero where it would return
       its own bytes: games go through null pointers both ways. Fetches fault. */
    bool absent_boot_rom_stores;
    /* Without a boot ROM image, the two firmware identification words that
       applications load from S3C2400_FIRMWARE_ID, or NULL. */
    const uint32_t *absent_boot_rom_id;
} S3c2400Bus;

#define S3C2400_SDRAM_BASE UINT32_C(0x0c000000)
#define S3C2400_BANK_SIZE UINT32_C(0x02000000)
#define S3C2400_FIRMWARE_ID UINT32_C(0x00001010)

/* The eight words of a cache line, at once, when they are all in the SDRAM and
   nobody observes the accesses.  False: read them one by one. */
static inline bool s3c2400_bus_line(const S3c2400Bus *bus, uint32_t address,
                                    uint32_t words[8])
{
    uint32_t offset = address - S3C2400_SDRAM_BASE;
    if (bus->hooks.access || !bus->ram || (address & 31u) ||
        offset >= S3C2400_BANK_SIZE || bus->ram_size < 32 || offset > bus->ram_size - 32)
        return false;
    const uint8_t *at = GP32_RAM(bus->ram, offset);
    for (unsigned i = 0; i < 8; ++i)
        words[i] = gp32_load(at + 4 * i, 4);
    return true;
}

/* Little-endian physical bus. CPU alignment and translation belong to the
   ARM920T. A failed read returns zero and reports accepted=false. */
S3c2400Access s3c2400_bus_read(const S3c2400Bus *bus, S3c2400AccessKind kind,
                               uint32_t address, uint8_t width);
S3c2400Access s3c2400_bus_write(const S3c2400Bus *bus, S3c2400AccessKind kind,
                                uint32_t address, uint8_t width, uint32_t value);
S3c2400Access s3c2400_bus_peek(const S3c2400Bus *bus, uint32_t address, uint8_t width);
bool s3c2400_bus_dma_unit(const S3c2400Bus *bus, uint32_t source,
                          uint32_t destination, uint8_t width);

#endif
