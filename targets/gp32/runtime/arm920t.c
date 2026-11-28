#include "arm920t.h"

#include <string.h>

#define N UINT32_C(0x80000000)
#define Z UINT32_C(0x40000000)
#define C UINT32_C(0x20000000)
#define V UINT32_C(0x10000000)
enum { T = 32, F = 64, I = 128, USER = 16, FIQ = 17, IRQ = 18,
       SVC = 19, ABT = 23, UND = 27, SYS = 31 };
enum { BUFFERABLE = 4, CACHEABLE = 8, PAGE_MAPPING = 16 };

static unsigned bank_index(uint32_t mode)
{
    switch (mode & 31u) {
    case FIQ: return 1;
    case IRQ: return 2;
    case SVC: return 3;
    case ABT: return 4;
    case UND: return 5;
    default: return 0;
    }
}

void arm920t_set_status(Arm920t *cpu, uint32_t psr)
{
    unsigned old = bank_index(cpu->cpsr), next = bank_index(psr);
    if (old != next) {
        ++cpu->effects;
        memcpy(cpu->bank[old], cpu->r + 13, sizeof cpu->bank[old]);
        memcpy(cpu->r + 13, cpu->bank[next], sizeof cpu->bank[next]);
        if (old == 1 || next == 1) {
            memcpy(old == 1 ? cpu->fiq : cpu->shared, cpu->r + 8, sizeof cpu->fiq);
            memcpy(cpu->r + 8, next == 1 ? cpu->fiq : cpu->shared, sizeof cpu->fiq);
        }
    }
    cpu->cpsr = psr;
}

static void branch(Arm920t *cpu, uint32_t target)
{
    cpu->r[15] = target & ((cpu->cpsr & T) ? ~1u : ~3u);
}

static void exchange(Arm920t *cpu, uint32_t target)
{
    cpu->cpsr = (cpu->cpsr & ~T) | ((target & 1) ? T : 0);
    branch(cpu, target);
}

static Arm920tStep exception(Arm920t *cpu, unsigned mode, unsigned vector, uint32_t lr)
{
    uint32_t old = cpu->cpsr;
    ++cpu->effects;
    arm920t_set_status(cpu, (old & ~(31u | T)) | mode | I | (mode == FIQ ? F : 0));
    cpu->spsr[bank_index(mode)] = old;
    cpu->r[14] = lr;
    cpu->r[15] = ((cpu->control & (1u << 13)) ? 0xffff0000u : 0) + vector;
    return ARM920T_EXCEPTION;
}

void arm920t_reset(Arm920t *cpu, Arm920tConfig config, bool high_vectors)
{
    memset(cpu, 0, sizeof *cpu);
    cpu->config = config;
    cpu->replacement_random = 1;
    cpu->tlb_generation = 1;
    cpu->cpsr = SVC | I | F;
    cpu->control = 0x78 | (high_vectors ? 1u << 13 : 0);
    cpu->r[15] = high_vectors ? 0xffff0000u : 0;
}

static uint32_t ror(uint32_t x, unsigned n)
{
    n &= 31;
    return n ? (x >> n) | (x << (32 - n)) : x;
}

static uint32_t extend(uint32_t x, unsigned bits)
{
    uint32_t sign = 1u << (bits - 1);
    return ((x & (sign * 2 - 1)) ^ sign) - sign;
}

static uint32_t endian(const Arm920t *cpu, uint32_t x, unsigned width)
{
    if (!(cpu->control & 128) || width == 1) return x;
    if (width == 2) return ((x & 255) << 8) | ((x >> 8) & 255);
    return (x << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) | (x >> 24);
}

static uint32_t modified_address(const Arm920t *cpu, uint32_t va)
{
    return va < 0x02000000 ? va | cpu->pid : va;
}

static void drain(Arm920t *cpu);

static unsigned walk(Arm920t *cpu, uint32_t mva, Arm920tTlbEntry *mapping,
                     unsigned *domain)
{
    ++cpu->effects;
    drain(cpu);
    S3c2400Access entry = s3c2400_bus_read(cpu->config.bus, S3C2400_PAGE_TABLE_LOAD,
        cpu->ttbr | ((mva >> 18) & 0x3ffc), 4);
    if (!entry.accepted) return 12;
    uint32_t desc = endian(cpu, entry.value, 4);
    unsigned type = desc & 3, ap;
    uint32_t offset_mask, base;
    if (!type) return 5;
    *domain = (desc >> 5) & 15;
    if (type == 2) {
        offset_mask = 0xfffff;
        base = desc & ~offset_mask;
        ap = (desc >> 10) & 3;
    } else {
        uint32_t pte = type == 1
            ? (desc & 0xfffffc00) | ((mva >> 10) & 0x3fc)
            : (desc & 0xfffff000) | ((mva >> 8) & 0xffc);
        entry = s3c2400_bus_read(cpu->config.bus, S3C2400_PAGE_TABLE_LOAD, pte, 4);
        if (!entry.accepted) return 14;
        desc = endian(cpu, entry.value, 4);
        unsigned page = desc & 3;
        if (!page || (page == 3 && type != 3)) return 7;
        offset_mask = page == 1 ? 0xffff : (page == 2 ? 0xfff : 0x3ff);
        base = desc & ~offset_mask;
        ap = (desc >> 4) & (page == 3 ? 3 : 255);
    }
    *mapping = (Arm920tTlbEntry){
        .virtual_base = mva & ~offset_mask, .physical_base = base,
        .offset_mask = offset_mask, .domain = *domain, .permissions = ap,
        .attributes = (desc & (BUFFERABLE | CACHEABLE)) | (type == 2 ? 0 : PAGE_MAPPING)
    };
    return 0;
}

static bool tlb_match(const Arm920tTlbEntry *entry, uint32_t mva)
{
    return entry->offset_mask && (mva & ~entry->offset_mask) == entry->virtual_base;
}

static GP32_HOT unsigned translate(Arm920t *cpu, uint32_t va, bool write, bool user,
                          bool fetch, uint32_t *pa, unsigned *domain, unsigned *attributes)
{
    uint32_t mva = modified_address(cpu, va);
    *domain = 0;
    *pa = mva;
    *attributes = fetch ? CACHEABLE : 0;
    if (!(cpu->control & 1)) return 0;
    unsigned port = fetch ? 0 : 1;
    Arm920tTlbEntry *entries = cpu->tlb[port];
    uint8_t *hint = &cpu->tlb_hint[port][(mva >> 12) & 255];
    unsigned slot = *hint;
    if (!tlb_match(&entries[slot], mva))
        for (slot = 0; slot < 64 && !tlb_match(&entries[slot], mva); ++slot) {}
    if (slot == 64) {
        slot = cpu->victim[port];
        ++cpu->tlb_generation;
        unsigned fault = walk(cpu, mva, &entries[slot], domain);
        if (fault) return fault;
        entries[slot].preserved = cpu->tlb_preserve[port];
        cpu->victim[port] = slot == 63 ? cpu->tlb_base[port] : slot + 1;
    }
    *hint = (uint8_t)slot;
    const Arm920tTlbEntry *mapping = &entries[slot];
    *domain = mapping->domain;
    *attributes = mapping->attributes;
    *pa = mapping->physical_base | (mva & mapping->offset_mask);
    bool section = mapping->offset_mask == 0xfffff;
    unsigned ap = mapping->permissions;
    if (!section && mapping->offset_mask != 0x3ff) {
        /* The two highest offset bits select the subpage permissions. */
        uint32_t half = (mapping->offset_mask >> 1) + 1;
        unsigned quarter = (mva & half ? 2u : 0u) | (mva & (half >> 1) ? 1u : 0u);
        ap = (ap >> (2 * quarter)) & 3;
    }
    unsigned access = (cpu->domains >> (*domain * 2)) & 3;
    if (access == 3) return 0;
    if (access != 1) return section ? 9 : 11;
    bool allowed;
    switch (ap) {
    case 0: allowed = !write && ((cpu->control & 0x300) == 0x200 ||
                                 ((cpu->control & 0x300) == 0x100 && !user)); break;
    case 1: allowed = !user; break;
    case 2: allowed = !user || !write; break;
    default: allowed = true; break;
    }
    return allowed ? 0 : (section ? 13 : 15);
}

static void drain_word(Arm920t *cpu)
{
    if (!cpu->write_count) return;
    ++cpu->effects;
    Arm920tBufferedWrite *entry = &cpu->writes[cpu->write_head];
    s3c2400_bus_write(cpu->config.bus, entry->kind, entry->physical,
                     entry->width, entry->value);
    cpu->write_head = (cpu->write_head + 1) % 16;
    --cpu->write_count;
}

static void drain(Arm920t *cpu)
{
    while (cpu->write_count) drain_word(cpu);
}

static void enqueue(Arm920t *cpu, S3c2400AccessKind kind, uint32_t physical,
                    unsigned width, uint32_t value)
{
    if (cpu->write_count == 16) drain_word(cpu);
    unsigned tail = (cpu->write_head + cpu->write_count) % 16;
    cpu->writes[tail] = (Arm920tBufferedWrite){physical, value, width, kind};
    ++cpu->write_count;
}

static Arm920tCacheLine *cache_lookup(Arm920t *cpu, unsigned port, uint32_t mva)
{
    unsigned segment = (mva >> 5) & 7;
    Arm920tCacheLine *set = cpu->cache[port].lines[segment];
    uint8_t *hint = &cpu->way_hint[port][(mva >> 5) & 1023];
    Arm920tCacheLine *last = &set[*hint];
    /* A line is filled only on a miss, so a tag has at most one way. */
    if (last->valid && last->tag == (mva & ~31u)) return last;
    for (unsigned way = 0; way < 64; ++way)
        if (set[way].valid && set[way].tag == (mva & ~31u)) {
            *hint = (uint8_t)way;
            return &set[way];
        }
    return NULL;
}

static void cache_clean(Arm920t *cpu, Arm920tCacheLine *line)
{
    for (unsigned i = 0; i < 8; ++i)
        if (line->dirty & (1u << (i / 4)))
            enqueue(cpu, S3C2400_CACHE_WRITEBACK, line->physical + 4 * i, 4, line->words[i]);
    line->dirty = 0;
}

static Arm920tCacheLine *cache_fill(Arm920t *cpu, unsigned port, uint32_t mva,
                                   uint32_t physical)
{
    Arm920tCache *cache = &cpu->cache[port];
    unsigned segment = (mva >> 5) & 7, way = cache->victim[segment];
    Arm920tCacheLine *line = &cache->lines[segment][way];
    Arm920tCacheLine incoming = {.tag = mva & ~31u, .physical = physical & ~31u, .valid = true};
    S3c2400AccessKind kind = port ? S3C2400_DCACHE_FILL : S3C2400_ICACHE_FILL;
    ++cpu->effects;
    /* Write back the victim before issuing the external line read. */
    if (line->valid) cache_clean(cpu, line);
    drain(cpu);
    if (!s3c2400_bus_line(cpu->config.bus, incoming.physical, incoming.words))
        for (unsigned i = 0; i < 8; ++i) {
            S3c2400Access access = s3c2400_bus_read(cpu->config.bus, kind,
                                                    incoming.physical + 4 * i, 4);
            if (!access.accepted) return NULL;
            incoming.words[i] = access.value;
        }
    *line = incoming;
    if (cpu->control & (1u << 14)) {
        cache->victim[segment] = way == 63 ? cache->base : way + 1;
    } else {
        uint32_t random = cpu->replacement_random;
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        cpu->replacement_random = random;
        /* All 64 ways, or a power of two of them, need no division. */
        uint32_t ways = 64 - cache->base;
        cache->victim[segment] = cache->base +
            (ways & (ways - 1) ? random % ways : random & (ways - 1));
    }
    return line;
}

static bool memory_full(Arm920t *cpu, S3c2400AccessKind kind, uint32_t va,
                   unsigned width, bool user, bool swap, uint32_t *data, uint32_t *physical)
{
    bool write = kind == S3C2400_STORE, fetch = kind == S3C2400_FETCH;
    unsigned fault, domain = 0, attributes;
    if (write) ++cpu->effects;
    uint32_t aligned = va & ~(width - 1), mva = modified_address(cpu, aligned);
    if (!fetch && (cpu->control & 2) && va != aligned) fault = 1;
    else fault = translate(cpu, aligned, write, user || (cpu->cpsr & 31) == USER,
                           fetch, physical, &domain, &attributes);
    if (fault) goto abort;
    unsigned port = fetch ? 0 : 1;
    bool enabled = fetch ? (cpu->control & 0x1000) != 0 : (cpu->control & 5) == 5;
    bool cacheable = enabled && (attributes & CACHEABLE);
    Arm920tCacheLine *line = (cacheable || (enabled && write)) ? cache_lookup(cpu, port, mva) : NULL;
    bool hit = line != NULL;
    if (!line && cacheable && !write) {
        line = cache_fill(cpu, port, mva, *physical);
        if (!line) goto external_abort;
    }
    bool buffered = false;
    uint32_t raw = write ? endian(cpu, *data, width) : 0;
    uint32_t mask = UINT32_MAX >> (32 - 8 * width);
    if (line) {
        unsigned index = (mva >> 2) & 7, shift = (mva & 3) * 8;
        if (write) {
            line->words[index] = (line->words[index] & ~(mask << shift)) | ((raw & mask) << shift);
            if (cacheable && (attributes & BUFFERABLE)) line->dirty |= 1u << (index / 4);
        } else raw = (line->words[index] >> shift) & mask;
    }
    if (write)
        buffered = (cacheable || (!swap && (attributes & BUFFERABLE))) &&
                   !(line && cacheable && (attributes & BUFFERABLE));
    if (buffered) {
        enqueue(cpu, kind, *physical, width, raw & mask);
    } else if (!line || (write && !cacheable)) {
        drain(cpu);
        S3c2400Access access = write
            ? s3c2400_bus_write(cpu->config.bus, kind, *physical, width, raw)
            : s3c2400_bus_read(cpu->config.bus, kind, *physical, width);
        if (!access.accepted) goto external_abort;
        if (!write && *physical - S3C2400_SDRAM_BASE >= S3C2400_BANK_SIZE) ++cpu->device_reads;
        if (!write) raw = access.value;
    }
    if (!write && cacheable) {
        uint32_t page = mva & ~0x3ffu;
        cpu->read_pages[port][(mva >> 10) & 63] = (Arm920tReadPage){
            page, *physical & ~0x3ffu, cpu->control, cpu->domains, cpu->tlb_generation,
            user || (cpu->cpsr & 31) == USER};
    }
    if (!write) *data = endian(cpu, raw, width);
    if (M0_OBSERVER(cpu->config.access)) {
        Arm920tAccess event = {kind, va, *physical, *data & mask, width, hit, buffered};
        cpu->config.access(cpu->config.context, &event);
    }
    return true;

external_abort:
    fault = (attributes & PAGE_MAPPING) ? 10 : 8;
abort:
    if (fetch) cpu->ifsr = fault | (domain << 4);
    else {
        cpu->dfsr = fault | (domain << 4);
        cpu->far = modified_address(cpu, va);
    }
    return false;
}

/* Unobserved, untranslated and uncached SDRAM accesses go straight to the
   host buffer. Every other case takes the complete path above. */
static inline bool memory(Arm920t *cpu, S3c2400AccessKind kind, uint32_t va,
                          unsigned width, bool user, bool swap, uint32_t *data,
                          uint32_t *physical)
{
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const S3c2400Bus *bus = cpu->config.bus;
    uint32_t aligned = va & ~(uint32_t)(width - 1);
    uint32_t offset = aligned - S3C2400_SDRAM_BASE;
    bool fetch = kind == S3C2400_FETCH;
    uint32_t cache_bits = fetch ? 0x1001u : 0x0001u;
    if (!(cpu->control & cache_bits) && offset < S3C2400_BANK_SIZE &&
        (size_t)offset + width <= bus->ram_size && !cpu->write_count &&
        !M0_OBSERVER(cpu->config.access) && !M0_OBSERVER(bus->hooks.access) && bus->ram &&
        (fetch || va == aligned || !(cpu->control & 2))) {
        uint8_t *memory = GP32_RAM(bus->ram, offset);
        *physical = aligned;
        if (kind == S3C2400_STORE) {
            /* A store of what memory already holds has no effect: a loop
               making only those repeats identically. */
            uint32_t raw = endian(cpu, *data, width) & (UINT32_MAX >> (32 - 8 * width));
            if (gp32_load(memory, width) != raw) {
                ++cpu->effects;
                gp32_store(memory, raw, width);
            }
        } else {
            *data = endian(cpu, gp32_load(memory, width), width);
        }
        return true;
    }
#endif
    /* A cache hit on a page already read under the same permissions has no
       side effect to reproduce. */
    if (kind != S3C2400_STORE && !user && !swap && !M0_OBSERVER(cpu->config.access) &&
        (fetch || va == (va & ~(uint32_t)(width - 1)) || !(cpu->control & 2))) {
        uint32_t mva = modified_address(cpu, va & ~(uint32_t)(width - 1));
        unsigned port = fetch ? 0 : 1;
        const Arm920tReadPage *page = &cpu->read_pages[port][(mva >> 10) & 63];
        if (page->page == (mva & ~0x3ffu) && page->generation == cpu->tlb_generation &&
            page->control == cpu->control && page->domains == cpu->domains &&
            page->user == ((cpu->cpsr & 31) == USER)) {
            const Arm920tCacheLine *line = &cpu->cache[port]
                .lines[(mva >> 5) & 7][cpu->way_hint[port][(mva >> 5) & 1023]];
            if (line->valid && line->tag == (mva & ~31u)) {
                uint32_t raw = line->words[(mva >> 2) & 7] >> ((mva & 3) * 8);
                *physical = page->physical | (mva & 0x3ffu);
                *data = endian(cpu, raw & (UINT32_MAX >> (32 - 8 * width)), width);
                return true;
            }
        }
    }
    return memory_full(cpu, kind, va, width, user, swap, data, physical);
}

/* The instruction fetch of recompiled code, which compares every opcode: the
   hit on a cached page already read, then the general path. */
static GP32_HOT __attribute__((noinline, unused)) bool fetch_opcode(Arm920t *cpu, uint32_t at,
    unsigned width, uint32_t *opcode, uint32_t *physical)
{
    uint32_t mva = modified_address(cpu, at);
    const Arm920tReadPage *page = &cpu->read_pages[0][(mva >> 10) & 63];
    if (page->page == (mva & ~0x3ffu) && page->generation == cpu->tlb_generation &&
        page->control == cpu->control && page->domains == cpu->domains &&
        page->user == ((cpu->cpsr & 31) == USER) && !M0_OBSERVER(cpu->config.access)) {
        const Arm920tCacheLine *line = &cpu->cache[0]
            .lines[(mva >> 5) & 7][cpu->way_hint[0][(mva >> 5) & 1023]];
        if (line->valid && line->tag == (mva & ~31u)) {
            uint32_t raw = line->words[(mva >> 2) & 7] >> ((mva & 3) * 8);
            *physical = page->physical | (mva & 0x3ffu);
            *opcode = endian(cpu, raw & (UINT32_MAX >> (32 - 8 * width)), width);
            return true;
        }
    }
    return memory(cpu, S3C2400_FETCH, at, width, false, false, opcode, physical);
}

static bool condition(uint32_t psr, unsigned cond)
{
    bool n = (psr & N) != 0, z = (psr & Z) != 0;
    bool c = (psr & C) != 0, v = (psr & V) != 0;
    bool test;
    switch (cond >> 1) {
    case 0: test = z; break;
    case 1: test = c; break;
    case 2: test = n; break;
    case 3: test = v; break;
    case 4: test = c && !z; break;
    case 5: test = n == v; break;
    case 6: test = !z && n == v; break;
    default: return cond == 14;
    }
    return test != ((cond & 1) != 0);
}

static uint32_t shift(uint32_t x, unsigned type, unsigned n, bool reg, bool *carry)
{
    if (!n && (reg || type == 0)) return x;
    switch (type) {
    case 0:
        *carry = n <= 32 && ((x >> (32 - n)) & 1);
        return n < 32 ? x << n : 0;
    case 1:
        if (!n) n = 32;
        *carry = n <= 32 && ((x >> (n - 1)) & 1);
        return n < 32 ? x >> n : 0;
    case 2:
        if (!n || n > 32) n = 32;
        *carry = ((x >> (n - 1)) & 1) != 0;
        return n < 32 ? (x >> n) | ((x & N) ? (~0u << (32 - n)) : 0) : (x & N ? ~0u : 0);
    default:
        if (!n) {
            uint32_t result = (x >> 1) | (*carry ? N : 0);
            *carry = (x & 1) != 0;
            return result;
        }
        x = ror(x, n);
        *carry = (x & N) != 0;
        return x;
    }
}

static void nz(Arm920t *cpu, uint32_t result)
{
    cpu->cpsr = (cpu->cpsr & ~(N | Z)) | (result & N) | (!result ? Z : 0);
}

static uint32_t alu(Arm920t *cpu, unsigned op, uint32_t a, uint32_t b, bool s, bool carry)
{
    bool input_c = (cpu->cpsr & C) != 0, overflow = (cpu->cpsr & V) != 0;
    uint32_t result;
    switch (op) {
    case 0: case 8: result = a & b; break;
    case 1: case 9: result = a ^ b; break;
    case 12: result = a | b; break;
    case 13: result = b; break;
    case 14: result = a & ~b; break;
    case 15: result = ~b; break;
    default: {
        unsigned cin = 0;
        if (op == 3 || op == 7) { uint32_t swap = a; a = b; b = swap; }
        if (op == 2 || op == 3 || op == 6 || op == 7 || op == 10) {
            b = ~b;
            cin = (op == 6 || op == 7) ? input_c : 1;
        } else if (op == 5) cin = input_c;
        uint64_t wide = (uint64_t)a + b + cin;
        result = (uint32_t)wide;
        carry = (wide >> 32) != 0;
        overflow = ((a ^ result) & (b ^ result) & N) != 0;
        break;
    }
    }
    if (s) {
        nz(cpu, result);
        cpu->cpsr = (cpu->cpsr & ~(C | V)) | (carry ? C : 0) | (overflow ? V : 0);
    }
    return result;
}

static bool transfer(Arm920t *cpu, uint32_t addr, unsigned width, bool load,
                     bool sign, bool user, uint32_t *value)
{
    uint32_t physical;
    bool odd_signed_half = load && sign && width == 2 && (addr & 1) && !(cpu->control & 2);
    if (odd_signed_half) width = 1;
    if (!memory(cpu, load ? S3C2400_LOAD : S3C2400_STORE,
                addr, width, user, false, value, &physical)) return false;
    if (load) {
        if (width == 4) *value = ror(*value, (addr & 3) * 8);
        if (width == 2 && (addr & 1)) *value = ror(*value, 8);
        if (sign) *value = extend(*value, width * 8);
    }
    return true;
}

bool arm920t_data_byte(Arm920t *cpu, uint32_t address, bool load, uint8_t *byte)
{
    uint32_t value = load ? 0 : *byte;
    if (!transfer(cpu, address, 1, load, false, false, &value))
        return false;
    if (load)
        *byte = value;
    return true;
}

static Arm920tStep software_interrupt(Arm920t *cpu, uint32_t number, uint32_t pc, unsigned width)
{
    ++cpu->effects;
    if (cpu->config.software_interrupt &&
        cpu->config.software_interrupt(cpu->config.software_interrupt_context, number, pc))
        return ARM920T_INSTRUCTION;
    return exception(cpu, SVC, 8, pc + width);
}

static uint32_t *user_register(Arm920t *cpu, unsigned reg)
{
    unsigned bank = bank_index(cpu->cpsr);
    if (reg >= 13 && reg <= 14 && bank) return &cpu->bank[0][reg - 13];
    if (reg >= 8 && reg <= 12 && bank == 1) return &cpu->shared[reg - 8];
    return &cpu->r[reg];
}

static Arm920tStep block(Arm920t *cpu, uint32_t ins, uint32_t pc)
{
    uint32_t mask = ins & 0xffff;
    unsigned rn = (ins >> 16) & 15, count = 0;
    bool load = (ins & (1u << 20)) != 0, up = (ins & (1u << 23)) != 0;
    bool pre = (ins & (1u << 24)) != 0, special = (ins & (1u << 22)) != 0;
    bool restore = load && special && (mask & 0x8000);
    bool writeback = (ins & (1u << 21)) != 0;
    for (unsigned r = 0; r < 16; ++r) count += (mask >> r) & 1;
    if (!count) { mask = 0x8000; count = 16; }
    uint32_t base = rn == 15 ? pc + 8 : cpu->r[rn];
    uint32_t last = up ? base + 4 * count : base - 4 * count;
    uint32_t address = (up ? base : last) + ((up == pre) ? 4 : 0);
    uint32_t target = 0;
    for (unsigned r = 0; r < 16; ++r) {
        if (!(mask & (1u << r))) continue;
        uint32_t *reg = special && !restore ? user_register(cpu, r) : &cpu->r[r];
        uint32_t value = r == 15 ? pc + 12 : *reg;
        if ((cpu->control & 2) && (address & 3)) {
            cpu->dfsr = 1;
            cpu->far = modified_address(cpu, address);
            goto abort;
        }
        if (!transfer(cpu, address & ~3u, 4, load, false, false, &value)) goto abort;
        if (load) {
            if (r == 15) target = value;
            else *reg = value;
        }
        address += 4;
    }
    if (writeback && !(load && (mask & (1u << rn))) && rn != 15) cpu->r[rn] = last;
    if (load && (mask & 0x8000)) {
        unsigned bank = bank_index(cpu->cpsr);
        if (restore && bank) arm920t_set_status(cpu, cpu->spsr[bank]);
        branch(cpu, target);
    }
    return ARM920T_INSTRUCTION;

abort:
    if (rn != 15) cpu->r[rn] = base;
    return exception(cpu, ABT, 16, pc + 8);
}

static bool cache_operation(Arm920t *cpu, unsigned crm, unsigned op, uint32_t value)
{
    if (op == 4 && (crm == 0 || crm == 10)) {
        drain(cpu);
        if (crm == 0) cpu->waiting = true;
        return true;
    }
    if (crm == 13 && op == 1) {
        uint32_t physical;
        unsigned domain, attributes;
        if (!translate(cpu, value, false, false, true, &physical, &domain, &attributes) &&
            (attributes & CACHEABLE) && !cache_lookup(cpu, 0, value))
            cache_fill(cpu, 0, value, physical);
        return true;
    }
    if (op == 0 && crm >= 5 && crm <= 7) {
        for (unsigned port = 0; port < 2; ++port) {
            if (crm != 7 && crm != 5 + port) continue;
            for (unsigned segment = 0; segment < 8; ++segment)
                for (unsigned way = 0; way < 64; ++way)
                    cpu->cache[port].lines[segment][way].valid = false;
        }
        return true;
    }
    bool clean = crm == 10 || crm == 14;
    if (!(clean || crm == 5 || crm == 6) || (op != 1 && !(clean && op == 2)))
        return false;
    Arm920tCache *cache = &cpu->cache[crm == 5 ? 0 : 1];
    Arm920tCacheLine *line = op == 1 ? cache_lookup(cpu, crm == 5 ? 0 : 1, value)
                                  : &cache->lines[(value >> 5) & 7][value >> 26];
    if (line && line->valid) {
        if (clean) cache_clean(cpu, line);
        if (crm != 10) line->valid = false;
    }
    return true;
}

static bool cp15(Arm920t *cpu, uint32_t ins)
{
    if ((cpu->cpsr & 31) == USER || ((ins >> 8) & 15) != 15 ||
        ((ins >> 21) & 7)) return false;
    unsigned crn = (ins >> 16) & 15, crm = ins & 15, op = (ins >> 5) & 7;
    unsigned rd = (ins >> 12) & 15;
    bool read = (ins & (1u << 20)) != 0;
    ++cpu->effects;
    uint32_t value = 0, mask = ~0u, *reg = NULL;
    switch (crn) {
    case 0:
        if (!read || crm || op > 1) return false;
        value = op ? 0x0d172172 : 0x41129200;
        break;
    case 1: reg = &cpu->control; mask = 0xc0007387; break;
    case 2: reg = &cpu->ttbr; mask = 0xffffc000; break;
    case 3: reg = &cpu->domains; break;
    case 5:
        if (op > 1) return false;
        reg = op ? &cpu->ifsr : &cpu->dfsr;
        mask = 255;
        break;
    case 6: reg = &cpu->far; break;
    case 7:
        return !read && rd != 15 && cache_operation(cpu, crm, op, cpu->r[rd]);
    case 8:
        if (read || rd == 15 || crm < 5 || crm > 7 || op > 1 || (crm == 7 && op))
            return false;
        ++cpu->tlb_generation;
        for (unsigned port = 0; port < 2; ++port) {
            if (crm != 7 && crm != 5 + port) continue;
            for (unsigned slot = 0; slot < 64; ++slot) {
                Arm920tTlbEntry *entry = &cpu->tlb[port][slot];
                if (op ? tlb_match(entry, cpu->r[rd]) : !entry->preserved)
                    entry->offset_mask = 0;
            }
        }
        return true;
    case 9: case 10: {
        if (crm || op > 1 || (!read && rd == 15)) return false;
        unsigned port = op ? 0 : 1;
        if (crn == 9) {
            Arm920tCache *cache = &cpu->cache[port];
            if (read) value = (uint32_t)cache->base << 26;
            else {
                cache->base = cpu->r[rd] >> 26;
                memset(cache->victim, cache->base, sizeof cache->victim);
            }
        } else if (read) {
            value = (cpu->tlb_base[port] << 26) | (cpu->victim[port] << 20) |
                    cpu->tlb_preserve[port];
        } else {
            cpu->tlb_base[port] = cpu->r[rd] >> 26;
            cpu->victim[port] = (cpu->r[rd] >> 20) & 63;
            cpu->tlb_preserve[port] = (cpu->r[rd] & 1) != 0;
        }
        if (!read) return true;
        break;
    }
    case 13: reg = &cpu->pid; mask = 0xfe000000; break;
    default: return false;
    }
    if (crm || (op && crn != 0 && crn != 5 && crn != 9 && crn != 10)) return false;
    if (read) {
        if (reg) value = *reg;
        if (rd == 15) cpu->cpsr = (cpu->cpsr & 0x0fffffff) | (value & 0xf0000000);
        else cpu->r[rd] = value;
    } else {
        if (rd == 15 || !reg) return false;
        *reg = (cpu->r[rd] & mask) | (crn == 1 ? 0x78 : 0);
    }
    return true;
}

static uint32_t operand(const Arm920t *cpu, unsigned reg, uint32_t pc)
{
    return reg == 15 ? pc + 8 : cpu->r[reg];
}

static Arm920tStep arm(Arm920t *cpu, uint32_t ins, uint32_t pc)
{
    unsigned rd = (ins >> 12) & 15, rn = (ins >> 16) & 15, rm = ins & 15;
    unsigned rs = (ins >> 8) & 15, group = (ins >> 25) & 7;
    unsigned bank = bank_index(cpu->cpsr);
    bool imm = (ins & (1u << 25)) != 0, load = (ins & (1u << 20)) != 0;
    if ((ins & 0x0ffffff0) == 0x012fff10) {
        exchange(cpu, operand(cpu, rm, pc));
    } else if ((ins & 0x0fbf0fff) == 0x010f0000) {
        if (rd == 15 || ((ins & (1u << 22)) && !bank)) goto undefined;
        cpu->r[rd] = (ins & (1u << 22)) ? cpu->spsr[bank] : cpu->cpsr;
    } else if ((ins & 0x0db0f000) == 0x0120f000 && (imm || !(ins & 0xff0))) {
        uint32_t value = imm ? ror(ins & 255, rs * 2) : operand(cpu, rm, pc);
        uint32_t mask = 0;
        for (unsigned i = 0; i < 4; ++i)
            if (rn & (1u << i)) mask |= 255u << (8 * i);
        if (ins & (1u << 22)) {
            if (!bank) goto undefined;
            cpu->spsr[bank] = (cpu->spsr[bank] & ~mask) | (value & mask);
        } else {
            mask &= (cpu->cpsr & 31) == USER ? 0xf0000000 : 0xf00000df;
            uint32_t psr = (cpu->cpsr & ~mask) | (value & mask);
            unsigned mode = psr & 31;
            if (!bank_index(mode) && mode != USER && mode != SYS) goto undefined;
            arm920t_set_status(cpu, psr);
        }
    } else if ((ins & 0x0fb00ff0) == 0x01000090) {
        if (rd == 15 || rn == 15 || rm == 15) goto undefined;
        unsigned width = (ins & (1u << 22)) ? 1 : 4;
        uint32_t address = cpu->r[rn], src = cpu->r[rm], dst, physical;
        bool read_ok = memory(cpu, S3C2400_LOAD, address, width, false, true, &dst, &physical);
        uint32_t read_status = cpu->dfsr, read_address = cpu->far;
        if (!read_ok && (read_status & 15) != 8 && (read_status & 15) != 10) goto abort;
        bool write_ok = memory(cpu, S3C2400_STORE, address, width, false, true, &src, &physical);
        if (!read_ok) {
            cpu->dfsr = read_status;
            cpu->far = read_address;
            goto abort;
        }
        if (!write_ok) goto abort;
        cpu->r[rd] = width == 4 ? ror(dst, (address & 3) * 8) : dst;
    } else if ((ins & 0x0f8000f0) == 0x00800090) {
        if (rn == 15 || rd == 15 || rs == 15 || rm == 15 || rn == rd) goto undefined;
        uint64_t a = cpu->r[rm], b = cpu->r[rs];
        if (ins & (1u << 22)) {
            if (a & N) a |= UINT64_C(0xffffffff00000000);
            if (b & N) b |= UINT64_C(0xffffffff00000000);
        }
        uint64_t result = a * b;
        if (ins & (1u << 21)) result += ((uint64_t)cpu->r[rn] << 32) | cpu->r[rd];
        cpu->r[rn] = (uint32_t)(result >> 32);
        cpu->r[rd] = (uint32_t)result;
        if (load) cpu->cpsr = (cpu->cpsr & ~(N | Z)) | (cpu->r[rn] & N) | (!result ? Z : 0);
    } else if ((ins & 0x0fc000f0) == 0x00000090) {
        if (rn == 15 || rs == 15 || rm == 15 ||
            ((ins & (1u << 21)) && rd == 15)) goto undefined;
        uint32_t value = cpu->r[rm] * cpu->r[rs];
        if (ins & (1u << 21)) value += cpu->r[rd];
        cpu->r[rn] = value;
        if (load) nz(cpu, value);
    } else if ((group == 0 && (ins & 0x90) == 0x90) || group == 2 || group == 3) {
        bool half = group == 0, sign = half && (ins & 0x40);
        bool pre = (ins & (1u << 24)) != 0, wb = (ins & (1u << 21)) != 0;
        unsigned width = 4;
        uint32_t offset;
        if (half) {
            if (!(ins & 0x60) || (sign && !load)) goto undefined;
            width = (ins & 0x20) ? 2 : 1;
            offset = (ins & (1u << 22)) ? ((ins >> 4) & 0xf0) | rm
                                       : operand(cpu, rm, pc);
        } else {
            width = (ins & (1u << 22)) ? 1 : 4;
            if (imm) {
                if (ins & 16) goto undefined;
                bool carry = (cpu->cpsr & C) != 0;
                offset = shift(operand(cpu, rm, pc), (ins >> 5) & 3, (ins >> 7) & 31,
                               false, &carry);
            } else offset = ins & 0xfff;
        }
        uint32_t base = operand(cpu, rn, pc);
        uint32_t adjusted = (ins & (1u << 23)) ? base + offset : base - offset;
        uint32_t value = rd == 15 ? pc + 12 : cpu->r[rd];
        if (!transfer(cpu, pre ? adjusted : base, width, load, sign, !pre && wb, &value))
            goto abort;
        if ((!pre || wb) && rn != 15) cpu->r[rn] = adjusted;
        if (load) {
            if (rd == 15) branch(cpu, value);
            else cpu->r[rd] = value;
        }
    } else if (group < 2) {
        unsigned op = (ins >> 21) & 15;
        bool test = op >= 8 && op <= 11;
        if (test && !load) goto undefined;
        bool carry = (cpu->cpsr & C) != 0;
        uint32_t a = operand(cpu, rn, pc), b;
        if (imm) {
            b = ror(ins & 255, rs * 2);
            if (rs) carry = (b & N) != 0;
        } else {
            bool by_reg = (ins & 16) != 0;
            b = operand(cpu, rm, pc);
            if (by_reg && rm == 15) b += 4;
            if (by_reg && rn == 15) a += 4;
            b = shift(b, (ins >> 5) & 3,
                      by_reg ? operand(cpu, rs, pc) & 255 : (ins >> 7) & 31,
                      by_reg, &carry);
        }
        uint32_t value = alu(cpu, op, a, b, load && (test || rd != 15), carry);
        if (!test) {
            if (rd == 15) {
                if (load && bank) arm920t_set_status(cpu, cpu->spsr[bank]);
                branch(cpu, value);
            } else cpu->r[rd] = value;
        }
    } else if (group == 4) {
        return block(cpu, ins, pc);
    } else if (group == 5) {
        if (ins & (1u << 24)) cpu->r[14] = pc + 4;
        branch(cpu, pc + 8 + (extend(ins, 24) << 2));
    } else if ((ins & 0x0f000000) == 0x0f000000) {
        return software_interrupt(cpu, ins & 0xffffffu, pc, 4);
    } else if ((ins & 0x0f000010) != 0x0e000010 || !cp15(cpu, ins)) {
        goto undefined;
    }
    return ARM920T_INSTRUCTION;

abort:
    return exception(cpu, ABT, 16, pc + 8);
undefined:
    return exception(cpu, UND, 4, pc + 4);
}

static Arm920tStep thumb(Arm920t *cpu, uint32_t ins, uint32_t pc)
{
    unsigned rd = ins & 7, rs = (ins >> 3) & 7, rn = (ins >> 6) & 7;
    unsigned rs4 = (ins >> 3) & 15;
    bool carry = (cpu->cpsr & C) != 0;
    if (ins < 0x1800) {
        cpu->r[rd] = shift(cpu->r[rs], ins >> 11, (ins >> 6) & 31, false, &carry);
        nz(cpu, cpu->r[rd]);
        cpu->cpsr = (cpu->cpsr & ~C) | (carry ? C : 0);
    } else if (ins < 0x2000) {
        cpu->r[rd] = alu(cpu, (ins & 0x200) ? 2 : 4, cpu->r[rs],
                         (ins & 0x400) ? rn : cpu->r[rn], true, carry);
    } else if (ins < 0x4000) {
        static const unsigned ops[] = {13, 10, 4, 2};
        rd = (ins >> 8) & 7;
        unsigned op = ops[(ins >> 11) & 3];
        uint32_t result = alu(cpu, op, cpu->r[rd], ins & 255, true, carry);
        if (op != 10) cpu->r[rd] = result;
    } else if (ins < 0x4400) {
        static const unsigned ops[] = {0, 1, 0, 0, 0, 5, 6, 0, 8, 3, 10, 11, 12, 0, 14, 15};
        unsigned op = (ins >> 6) & 15;
        uint32_t a = cpu->r[rd], b = cpu->r[rs];
        if ((op >= 2 && op <= 4) || op == 7) {
            cpu->r[rd] = shift(a, op == 7 ? 3 : op - 2, b & 255, true, &carry);
            nz(cpu, cpu->r[rd]);
            cpu->cpsr = (cpu->cpsr & ~C) | (carry ? C : 0);
        } else if (op == 13) {
            cpu->r[rd] = a * b;
            nz(cpu, cpu->r[rd]);
        } else {
            uint32_t value = alu(cpu, ops[op], op == 9 ? 0 : a, b, true, carry);
            if (op != 8 && op != 10 && op != 11) cpu->r[rd] = value;
        }
    } else if (ins < 0x4800) {
        rd |= (ins >> 4) & 8;
        unsigned op = (ins >> 8) & 3;
        uint32_t a = rd == 15 ? pc + 4 : cpu->r[rd];
        uint32_t b = rs4 == 15 ? pc + 4 : cpu->r[rs4];
        if (op == 3) {
            if (ins & 0x80) goto undefined;
            exchange(cpu, b);
        } else if (op == 1) {
            alu(cpu, 10, a, b, true, carry);
        } else {
            uint32_t value = op == 0 ? a + b : b;
            if (rd == 15) branch(cpu, value);
            else cpu->r[rd] = value;
        }
    } else if (ins < 0xa000) {
        uint32_t address;
        unsigned width = 4;
        bool load = true, sign = false;
        switch (ins >> 12) {
        case 4:
            rd = (ins >> 8) & 7;
            address = ((pc + 4) & ~3u) + (ins & 255) * 4;
            break;
        case 5: {
            static const unsigned widths[] = {4, 2, 1, 1, 4, 2, 1, 2};
            unsigned op = (ins >> 9) & 7;
            width = widths[op];
            load = op >= 3;
            sign = op == 3 || op == 7;
            address = cpu->r[rs] + cpu->r[rn];
            break;
        }
        case 9:
            rd = (ins >> 8) & 7;
            load = (ins & 0x800) != 0;
            address = cpu->r[13] + (ins & 255) * 4;
            break;
        default:
            width = ins >= 0x8000 ? 2 : (ins >= 0x7000 ? 1 : 4);
            load = (ins & 0x800) != 0;
            address = cpu->r[rs] + ((ins >> 6) & 31) * width;
            break;
        }
        if (!transfer(cpu, address, width, load, sign, false, &cpu->r[rd]))
            return exception(cpu, ABT, 16, pc + 8);
    } else if (ins < 0xb000) {
        uint32_t base = (ins & 0x800) ? cpu->r[13] : ((pc + 4) & ~3u);
        cpu->r[(ins >> 8) & 7] = base + (ins & 255) * 4;
    } else if ((ins & 0xff00) == 0xb000) {
        uint32_t delta = (ins & 127) * 4;
        cpu->r[13] += (ins & 128) ? 0u - delta : delta;
    } else if ((ins & 0xf600) == 0xb400) {
        bool pop = (ins & 0x800) != 0;
        uint32_t list = (ins & 255) | ((ins & 256) ? (pop ? 0x8000 : 0x4000) : 0);
        return block(cpu, (pop ? 0x08bd0000 : 0x092d0000) | list, pc);
    } else if (ins >= 0xc000 && ins < 0xd000) {
        return block(cpu, 0x08a00000 | ((ins & 0x800) << 9) |
                          ((ins & 0x700) << 8) | (ins & 255), pc);
    } else if (ins >= 0xd000 && ins < 0xe000) {
        unsigned cond = (ins >> 8) & 15;
        if (cond == 15) return software_interrupt(cpu, ins & 255u, pc, 2);
        if (cond == 14) goto undefined;
        branch(cpu, pc + 4 + (extend(ins, 8) << 1));
    } else if ((ins & 0xf800) == 0xe000) {
        branch(cpu, pc + 4 + (extend(ins, 11) << 1));
    } else if ((ins & 0xf800) == 0xf000) {
        cpu->r[14] = pc + 4 + (extend(ins, 11) << 12);
    } else if ((ins & 0xf800) == 0xf800) {
        branch(cpu, cpu->r[14] + ((ins & 0x7ff) << 1));
        cpu->r[14] = (pc + 2) | 1;
    } else goto undefined;
    return ARM920T_INSTRUCTION;

undefined:
    return exception(cpu, UND, 4, pc + 2);
}

/* Condition, observers and effects of an already fetched instruction. */
static Arm920tStep execute(Arm920t *cpu, uint32_t pc, uint32_t physical,
                           uint32_t opcode, bool is_thumb)
{
    unsigned cond = is_thumb ? 14 : opcode >> 28;
    if (is_thumb && opcode >= 0xd000 && opcode < 0xde00)
        cond = (opcode >> 8) & 15;
    Arm920tTrace trace = {pc, physical, opcode, is_thumb, condition(cpu->cpsr, cond)};
    if (cond < 14 && M0_OBSERVER(cpu->config.predicate))
        trace.executed = cpu->config.predicate(cpu->config.context, &trace);
    if (M0_OBSERVER(cpu->config.trace)) cpu->config.trace(cpu->config.context, &trace);
    cpu->r[15] = pc + (is_thumb ? 2 : 4);
    if (!trace.executed) return ARM920T_INSTRUCTION;
    return is_thumb ? thumb(cpu, opcode, pc) : arm(cpu, opcode, pc);
}

/* An instruction step whose buffered write has already drained. */
static Arm920tStep step_drained(Arm920t *cpu)
{
    uint32_t pc = cpu->r[15], physical, opcode;
    if (cpu->fiq_line || cpu->irq) cpu->waiting = false;
    if (cpu->fiq_line && !(cpu->cpsr & F)) return exception(cpu, FIQ, 28, pc + 4);
    if (cpu->irq && !(cpu->cpsr & I)) return exception(cpu, IRQ, 24, pc + 4);
    if (cpu->waiting) return ARM920T_WAITING;
    bool is_thumb = (cpu->cpsr & T) != 0;
    unsigned width = is_thumb ? 2 : 4;
    if (!memory(cpu, S3C2400_FETCH, pc, width, false, false, &opcode, &physical))
        return exception(cpu, ABT, 12, pc + 4);
    return execute(cpu, pc, physical, opcode, is_thumb);
}

Arm920tStep arm920t_step(Arm920t *cpu)
{
    drain_word(cpu);
    return step_drained(cpu);
}
