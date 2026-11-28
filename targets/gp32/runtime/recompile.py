#!/usr/bin/env python3
"""Static recompilation of a GP32 application from a deepeel coverage map.

usage: recompile.py MAP.code IMAGE.bin OUTPUT.c [--fast | --hybrid] [--count | --hot COUNTS... [--share PERCENT]]
       recompile.py --compact OUTPUT.c [MAP.code IMAGE.bin PAGES.txt COUNT]
       recompile.py --page-profile OUTPUT.c

Every covered instruction becomes C code specialized for its opcode: the
generated files include the interpreter, and the compiler folds the decoding
away.  The pages are spread over OUTPUT.partNNN.c files, which keeps the
memory needed by one compilation bounded.  Each instruction still fetches its opcode and compares it with the
recompiled one, so code that was patched, unpacked elsewhere or never covered
runs in the interpreter with the same behaviour.

Only the instructions a program spends its time in deserve inline code, which
takes about 500 bytes each.  --count makes a build which counts executions into
the file named by M0_GP32_COUNTS.  --hot reads such files and inlines the most
executed instructions, up to PERCENT of all executions (default 99.5); every
other one stays recompiled but calls one shared step with its own function,
three times smaller.  Without these options all instructions are inlined.

--fast trades exactness for speed: the inlined ARM code is compared with memory
once per straight block, not through the instruction cache, and the clocks of a
block are counted at once, so a peripheral event may wait up to its end.

--hybrid translates the inlined ARM code into native C (see native.py): guest
registers are local variables and SDRAM is accessed in place.  Devices, system
instructions, Thumb code and everything near a peripheral event stay with the
precise steps, so events are on time.  Not usable with --count.  With --hot,
only the hottest instructions are recompiled at all: the other covered ones are
run by the interpreter, from a loop of their own (one bit per address).

--compact needs no map: it generates one function per instruction shape (the
opcode bits that select an operation), dispatched from one loop.  It covers
every instruction of any application in a few hundred kilobytes.  With a map,
the COUNT first pages of PAGES.txt (one hexadecimal page address per line,
hottest first) are also recompiled in full.

--page-profile is the compact form instrumented to write such a list, to the
file named by M0_GP32_PAGES when the program exits.
"""
import glob
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import native  # noqa: E402: found beside this script, even under python -P

PAGE = 1024
# Instructions of a fast block at most: events wait for its end.
BLOCK = 24
# Instructions per generated part: about the largest unit that compiles in a few
# hundred megabytes.
PART = 2500
# Public symbols of arm920t.c, renamed in every part but the main file.
PUBLIC = ("arm920t_set_status", "arm920t_reset", "arm920t_data_byte", "arm920t_step")
# "inferred" is mostly padding: the interpreter covers it if it ever runs.
CODE = {"natural", "forced", "static"}
WIDTH = {"arm": 4, "thumb": 2}


described = 0x0C000000, 0, ""  # base, size and SHA-1 of the mapped image


def map_macros():
    base, size, digest = described
    header = f"base {base:08x}\\nimage {size} {digest}\\n" if size else ""
    return (f"\n#define MAP_BASE 0x{base:08x}u\n#define MAP_BYTES {size}u\n"
            f"#define MAP_HEADER \"{header}\"\n")


def hle_stubs():
    """The routines gp32_hle.c writes into RAM, outside any application image:
    {address: opcode}.  Recompiled code checks each opcode before running it, so
    a program without them, or with other code there, is interpreted as usual."""
    routines = {
        0x0C7B1400: [0xEF7F0000, 0xE12FFF1E],  # display flip
        0x0C7B0400: [0xE92D503F, 0xE59F0020, 0xE5901000, 0xE2811001, 0xE5801000,
                     0xE5902004, 0xE3520000, 0x11A0E00F, 0x112FFF12, 0xE8BD503F,
                     0xE25EF004],  # timer tick interrupt
        0x0C7B0430: [0xE25EF004],  # empty interrupt handler
    }
    for entry in range(13):  # card pin routines
        routines[0x0C7AB800 + 8 * entry] = [0xEF7F0010 + entry, 0xE12FFF1E]
    return {start + 4 * i: word for start, words in routines.items()
            for i, word in enumerate(words)}


def load(map_path, image_path):
    global described
    image = open(image_path, "rb").read()
    base, bios, sections = None, None, []
    for line in open(map_path, encoding="ascii"):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "base":
            base = int(fields[1], 16)
        elif fields[0] == "image":
            if int(fields[1]) != len(image) or hashlib.sha1(image).hexdigest() != fields[2]:
                sys.exit(f"{image_path} is not the image described by {map_path}")
        elif fields[0] == "bios":
            bios = (int(fields[1], 16), int(fields[2], 16))
        else:
            sections.append((int(fields[0], 16), int(fields[1], 16), fields[2], fields[3]))
    if base is None:
        sys.exit(f"{map_path}: no base line")
    described = base, len(image), hashlib.sha1(image).hexdigest()
    instructions = {"arm": {}, "thumb": {}}
    for offset, length, kind, mode in sections:
        if kind not in CODE or mode not in WIDTH:
            continue
        width = WIDTH[mode]
        for at in range((offset + width - 1) & -width, offset + length - width + 1, width):
            address = bios[1] + at - bios[0] if bios and at >= bios[0] else base + at
            instructions[mode][address] = int.from_bytes(image[at:at + width], "little")
    for address, word in hle_stubs().items():
        instructions["arm"].setdefault(address, word)
    return instructions


def leaves(mode, opcode):
    """Instructions after which the machine must look at the CPU again."""
    if mode == "arm":
        return (opcode & 0x0F000000) == 0x0F000000
    return (opcode & 0xFF00) == 0xDF00


def pure(opcode):
    """True for an ARM data processing or multiply instruction that cannot
    reach memory, the PC, the mode or the interrupt masks: nothing but the next
    instruction can follow it."""
    if opcode >> 28 == 15 or opcode & 0x0C000000:
        return False
    high, low = opcode >> 16 & 15, opcode >> 12 & 15
    if not opcode & 0x02000000 and opcode & 0x90 == 0x90:
        if opcode & 0x60 or opcode & 0x01000000:
            return False  # halfword and signed transfers, swaps
        if opcode & 0x00800000:
            return high != 15 and low != 15
        return not opcode & 0x00400000 and high != 15
    operation = opcode >> 21 & 15
    if 8 <= operation <= 11 and not opcode & 0x00100000:
        return False  # status register moves, branches through registers
    return low != 15


def targets(instructions):
    """Addresses entered by a branch: a run of instructions starts there."""
    found = set()
    for address, opcode in instructions["arm"].items():
        if opcode >> 28 != 15 and opcode & 0x0E000000 == 0x0A000000:
            offset = ((opcode & 0xFFFFFF) ^ 0x800000) - 0x800000
            found.add((address + 8 + 4 * offset) & 0xFFFFFFFF)
    return found


def plain(opcode):
    """True for an ARM load or store which the next instruction follows, unless
    it faults: it writes neither the PC nor the status registers."""
    if opcode >> 28 == 15:
        return False
    load, low = opcode & 0x00100000, opcode >> 12 & 15
    if opcode & 0x0C000000 == 0x04000000:
        return not (opcode & 0x02000010 == 0x02000010 or load and low == 15)
    if opcode & 0x0E000090 == 0x00000090 and opcode & 0x60:
        return not (load and low == 15)
    if opcode & 0x0E000000 == 0x08000000:
        return not (opcode & 0x00400000 or load and opcode & 0x8000)
    return False


def blocks(instructions, addresses, hot, entries):
    """{first address: length} of the inlined straight code a fast block covers:
    consecutive, entered at its start, ending with the first instruction which
    may lead elsewhere than to the next one."""
    found, current = {}, []

    def close():
        if len(current) > 1:
            found[current[0]] = len(current)
        current.clear()

    for address in addresses:
        if hot is not None and ("arm", address) not in hot:
            close()
            continue
        if current and (address != current[-1] + 4 or address in entries or len(current) == BLOCK):
            close()
        current.append(address)
        opcode = instructions["arm"][address]
        if not pure(opcode) and not plain(opcode):
            close()
    close()
    return found


def block_body(words, first):
    """The C code of a checked fast block: only its last instruction may leave."""
    body = []
    for i, word in enumerate(words):
        at = f"0x{first + 4 * i:08x}u"
        if i == len(words) - 1:
            halt = "true" if leaves("arm", word) else "false"
            body.append(f"            account(run, {i});\n"
                        f"            LAST({at}, arm_{word:x}(cpu, {at}), {halt});\n")
        elif pure(word):
            body.append(f"            cpu->r[15] = {at} + 4;\n"
                        f"            arm_{word:x}(cpu, {at});\n")
        else:
            body.append(f"            MIDDLE({at}, arm_{word:x}(cpu, {at}), {i});\n")
    return "".join(body)


def runs(instructions, addresses, hot, entries):
    """{first address: length} of the inlined pure instructions which one check
    covers: consecutive, in one cache line, entered at the first only."""
    found, current = {}, []
    for address in addresses + [None]:
        fits = (address is not None and pure(instructions["arm"][address])
                and (hot is None or ("arm", address) in hot))
        if current and (not fits or address != current[-1] + 4 or not address % 32
                        or address in entries):
            if len(current) > 1:
                found[current[0]] = len(current)
            current = []
        if fits:
            current.append(address)
    return found


HEADER = """/* Generated by recompile.py: do not edit. */
#include "arm920t.h"

#define FOLD static inline __attribute__((always_inline))
FOLD Arm920tStep arm(Arm920t *cpu, uint32_t ins, uint32_t pc);
FOLD Arm920tStep thumb(Arm920t *cpu, uint32_t ins, uint32_t pc);
FOLD Arm920tStep block(Arm920t *cpu, uint32_t ins, uint32_t pc);
FOLD bool condition(uint32_t psr, unsigned cond);
FOLD uint32_t shift(uint32_t x, unsigned type, unsigned n, bool reg, bool *carry);
FOLD uint32_t alu(Arm920t *cpu, unsigned op, uint32_t a, uint32_t b, bool s, bool carry);
FOLD bool transfer(Arm920t *cpu, uint32_t addr, unsigned width, bool load,
                   bool sign, bool user, uint32_t *value);

#include "arm920t.c"

#define AGAIN ((Arm920tStep)-1)
#define MISS ((Arm920tStep)-2)
#define PAGE_BYTES 1024u

typedef Arm920tStep (*Page)(Arm920t *, Arm920tAot *);

/* Why an instruction went to the interpreter. */
enum { MISS_UNCOVERED, MISS_CHANGED, MISS_WAKE };
#if M0_GP32_PC_TOOLS
void m0_gp32_missed(uint32_t pc, bool is_thumb, unsigned reason);
#else
#define m0_gp32_missed(pc, is_thumb, reason) ((void)(pc), (void)(is_thumb), (void)(reason))
#endif

/* Called before step_drained(): taking an interrupt or sleeping is not an
   instruction; only a wake up with a masked interrupt interprets one. */
static inline void count_drained(const Arm920t *cpu, Arm920tAot *run)
{
    bool line = cpu->fiq_line || cpu->irq;
    if ((cpu->fiq_line && !(cpu->cpsr & F)) || (cpu->irq && !(cpu->cpsr & I)) ||
        (cpu->waiting && !line))
        return;
    ++*run->interpreted;
    m0_gp32_missed(cpu->r[15], (cpu->cpsr & T) != 0, MISS_WAKE);
}
"""

# Written once, in the main file: the distinct interpreted instructions are the
# coverage the recompiled code lacks.  They go to the map named by
# arm920t_aot_found(), and to the file named by M0_GP32_MISSES.  MAP_BASE,
# MAP_BYTES and MAP_HEADER describe the image the recompiled map was made from.
MISSES = """
#if M0_GP32_PC_TOOLS
#include <stdio.h>
#include <stdlib.h>
#define MISS_SLOTS (1u << 18)
static uint64_t miss_keys[MISS_SLOTS];
static char found_path[1100];
static bool found_new;

static void write_misses(void)
{
    FILE *file = fopen(getenv("M0_GP32_MISSES"), "a");
    if (!file) return;
    for (size_t i = 0; i < MISS_SLOTS; ++i)
        if (miss_keys[i])
            fprintf(file, "%08x %s %s\\n", (unsigned)miss_keys[i],
                    miss_keys[i] >> 32 & 4 ? "thumb" : "arm",
                    (const char *[]){"uncovered", "changed", "wake"}[miss_keys[i] >> 32 & 3]);
    fclose(file);
}

static void remember(uint32_t pc, bool is_thumb, unsigned reason)
{
    uint64_t key = (uint64_t)(8u | (is_thumb ? 4u : 0u) | reason) << 32 | pc;
    size_t at = (pc >> 1) & (MISS_SLOTS - 1);
    for (size_t tries = 0; tries < MISS_SLOTS; ++tries, at = (at + 1) & (MISS_SLOTS - 1)) {
        if (miss_keys[at] == key) return;
        if (!miss_keys[at]) {
            miss_keys[at] = key;
            found_new = true;
            return;
        }
    }
}

void m0_gp32_missed(uint32_t pc, bool is_thumb, unsigned reason)
{
    static int wanted = -1;
    if (wanted < 0) {
        wanted = getenv("M0_GP32_MISSES") != 0;
        if (wanted) atexit(write_misses);
    }
    if (wanted || found_path[0])
        remember(pc, is_thumb, reason);
}

/* Adds the instructions of an earlier map to the ones of this run. */
static void read_found(void)
{
    FILE *file = fopen(found_path, "r");
    char line[128], mode[8];
    unsigned at, length;
    while (file && fgets(line, sizeof line, file)) {
        if (sscanf(line, "%x %x natural %7s", &at, &length, mode) == 3)
            remember(MAP_BASE + at, mode[0] == 't', MISS_UNCOVERED);
        else if (sscanf(line, "# changed %x %7s", &at, mode) == 2)
            remember(at, mode[0] == 't', MISS_CHANGED);
        else if (sscanf(line, "# outside %x %7s", &at, mode) == 2)
            remember(at, mode[0] == 't', MISS_UNCOVERED);
    }
    if (file) fclose(file);
}

void arm920t_aot_flush(void)
{
    char next[1120];
    if (!found_path[0] || !found_new) return;
    /* Only code the image holds and the map lacks is worth a file: every session
       meets changed and outside instructions, which no map can take. */
    bool lacking = false;
    for (size_t i = 0; i < MISS_SLOTS && !lacking; ++i)
        lacking = miss_keys[i] && (miss_keys[i] >> 32 & 3) != MISS_CHANGED &&
                  (uint32_t)miss_keys[i] - MAP_BASE < MAP_BYTES;
    if (!lacking) return;
    snprintf(next, sizeof next, "%s.new", found_path);
    FILE *file = fopen(next, "w");
    if (!file) return;
    fputs("# instructions the recompiled application did not hold\\n" MAP_HEADER, file);
    for (size_t i = 0; i < MISS_SLOTS; ++i) {
        if (!miss_keys[i]) continue;
        uint32_t pc = (uint32_t)miss_keys[i];
        bool is_thumb = (miss_keys[i] >> 32 & 4) != 0;
        unsigned reason = miss_keys[i] >> 32 & 3;
        const char *mode = is_thumb ? "thumb" : "arm";
        if (reason == MISS_CHANGED)
            fprintf(file, "# changed %08x %s\\n", (unsigned)pc, mode);
        else if (pc - MAP_BASE < MAP_BYTES)
            fprintf(file, "%x %x natural %s\\n", (unsigned)(pc - MAP_BASE), is_thumb ? 2 : 4, mode);
        else
            fprintf(file, "# outside %08x %s\\n", (unsigned)pc, mode);
    }
    bool written = !ferror(file);
    if (fclose(file) || !written) return;
    remove(found_path);
    if (!rename(next, found_path)) found_new = false;
}

void arm920t_aot_found(const char *path)
{
    static bool registered;
    if (!registered) registered = !atexit(arm920t_aot_flush);
    int length = snprintf(found_path, sizeof found_path, "%s", path);
    if (length < 0 || (size_t)length >= sizeof found_path) {
        found_path[0] = 0;
        return;
    }
    read_found();
    found_new = false;
}
#endif
"""

PART_HEADER = """
static inline bool interrupted(const Arm920t *cpu)
{
    return cpu->waiting || (cpu->irq && !(cpu->cpsr & I)) ||
           (cpu->fiq_line && !(cpu->cpsr & F));
}

static Arm920tStep interpret(Arm920t *cpu, Arm920tAot *run)
{
    run->pc = cpu->r[15];
    ++*run->interpreted;
    m0_gp32_missed(run->pc, (cpu->cpsr & T) != 0, MISS_UNCOVERED);
    return arm920t_step(cpu);
}

/* A short backward branch goes back to the machine, which skips a loop that
   repeats without effect; with effects since the last such branch it cannot. */
static inline bool repeats(const Arm920t *cpu, Arm920tAot *run, uint32_t at)
{
    if (at - cpu->r[15] >= 128u) return false;
    if (cpu->effects == run->effects) return true;
    run->effects = cpu->effects;
    return false;
}

/* The instruction at `at`: fetch, check, execute, then either account it and
   go on, or hand it back to the machine. */
#define STEP(at, expected, width, is_thumb, handler, stop) do { \\
    uint32_t opcode_, physical_; \\
    drain_word(cpu); \\
    if (__builtin_expect(interrupted(cpu), 0)) goto drained; \\
    if (__builtin_expect(!fetch_opcode(cpu, (at), width, &opcode_, &physical_), 0)) { \\
        run->pc = (at); \\
        return exception(cpu, ABT, 12, (at) + 4); \\
    } \\
    if (__builtin_expect(opcode_ != (expected), 0)) { \\
        run->pc = (at); \\
        ++*run->interpreted; \\
        m0_gp32_missed((at), (is_thumb), MISS_CHANGED); \\
        return execute(cpu, (at), physical_, opcode_, is_thumb); \\
    } \\
    cpu->r[15] = (at) + (width); \\
    Arm920tStep result_ = handler; \\
    if (__builtin_expect((stop) || result_ != ARM920T_INSTRUCTION || \\
                         repeats(cpu, run, (at)) || run->left <= 1 || \\
                         run->cost > *run->budget, 0)) { \\
        run->pc = (at); \\
        return result_; \\
    } \\
    --run->left; \\
    ++*run->steps; \\
    *run->budget -= run->cost; \\
    *run->pending += run->cost; \\
    if (cpu->r[15] != (at) + (width)) goto jumped; \\
} while (0)

/* The same step for the instructions not worth inlining: one copy per file,
   the instruction being its specialized function. */
typedef Arm920tStep (*Handler)(Arm920t *, uint32_t);
enum { GO, JUMPED, DRAINED, DONE };

static __attribute__((noinline, unused)) int cold_step(Arm920t *cpu, Arm920tAot *run,
    uint32_t at, uint32_t expected, Handler handler, unsigned flags, Arm920tStep *result)
{
    unsigned width = flags & 6;
    bool is_thumb = width == 2, stop = flags & 1;
    uint32_t opcode, physical;
    COUNT(at);
    drain_word(cpu);
    if (__builtin_expect(interrupted(cpu), 0)) return DRAINED;
    if (__builtin_expect(!fetch_opcode(cpu, at, width, &opcode, &physical), 0)) {
        run->pc = at;
        *result = exception(cpu, ABT, 12, at + 4);
        return DONE;
    }
    if (__builtin_expect(opcode != expected, 0)) {
        run->pc = at;
        ++*run->interpreted;
        m0_gp32_missed(at, is_thumb, MISS_CHANGED);
        *result = execute(cpu, at, physical, opcode, is_thumb);
        return DONE;
    }
    cpu->r[15] = at + width;
    *result = handler(cpu, at);
    if (__builtin_expect(stop || *result != ARM920T_INSTRUCTION ||
                         repeats(cpu, run, at) || run->left <= 1 ||
                         run->cost > *run->budget, 0)) {
        run->pc = at;
        return DONE;
    }
    --run->left;
    ++*run->steps;
    *run->budget -= run->cost;
    *run->pending += run->cost;
    return cpu->r[15] != at + width ? JUMPED : GO;
}

/* The host copy of the code at `at`, when `count` pure instructions of one cache
   line may run at once: no buffered write to drain, no interrupt to take, no
   event within their clocks, and a fetch without any side effect. */
static __attribute__((noinline, unused)) const void *run_ready(const Arm920t *cpu,
    const Arm920tAot *run, uint32_t at, unsigned count)
{
    if (cpu->write_count || interrupted(cpu) || run->left <= count ||
        count * run->cost > *run->budget || (cpu->control & 0x80) || cpu->config.access)
        return 0;
    const S3c2400Bus *bus = cpu->config.bus;
    uint32_t offset = at - S3C2400_SDRAM_BASE;
    if (!(cpu->control & 0x1001u)) {
        if (offset >= S3C2400_BANK_SIZE || (size_t)offset + 4 * count > bus->ram_size ||
            bus->hooks.access || !bus->ram || !gp32_ram_whole(offset, 4 * count))
            return 0;
        return GP32_RAM(bus->ram, offset);
    }
    uint32_t mva = modified_address(cpu, at);
    const Arm920tReadPage *page = &cpu->read_pages[0][(mva >> 10) & 63];
    if (page->page != (mva & ~0x3ffu) || page->generation != cpu->tlb_generation ||
        page->control != cpu->control || page->domains != cpu->domains ||
        page->user != ((cpu->cpsr & 31) == USER))
        return 0;
    const Arm920tCacheLine *line = &cpu->cache[0]
        .lines[(mva >> 5) & 7][cpu->way_hint[0][(mva >> 5) & 1023]];
    if (!line->valid || line->tag != (mva & ~31u))
        return 0;
    return &line->words[(mva >> 2) & 7];
}

/* The host copy of `count` instructions at `at`, when they may run as one fast
   block: nothing buffered, no interrupt to take, and the code mapped in SDRAM.
   The instruction cache is not consulted. */
static __attribute__((noinline, unused)) const void *block_ready(const Arm920t *cpu,
    const Arm920tAot *run, uint32_t at, unsigned count)
{
    if (cpu->write_count || interrupted(cpu) || run->left <= count ||
        (cpu->control & 0x80) || cpu->config.access)
        return 0;
    const S3c2400Bus *bus = cpu->config.bus;
    uint32_t offset = at - S3C2400_SDRAM_BASE;
    if (cpu->control & 0x1001u) {
        uint32_t mva = modified_address(cpu, at);
        const Arm920tReadPage *page = &cpu->read_pages[0][(mva >> 10) & 63];
        if (page->page != (mva & ~0x3ffu) || page->generation != cpu->tlb_generation ||
            page->control != cpu->control || page->domains != cpu->domains ||
            page->user != ((cpu->cpsr & 31) == USER))
            return 0;
        offset = (page->physical | (mva & 0x3ffu)) - S3C2400_SDRAM_BASE;
    }
    if (offset >= S3C2400_BANK_SIZE || (size_t)offset + 4 * count > bus->ram_size ||
        bus->hooks.access || !bus->ram || !gp32_ram_whole(offset, 4 * count))
        return 0;
    return GP32_RAM(bus->ram, offset);
}

/* Instructions of a fast block, counted at once.  The clocks the budget does
   not hold are left to the machine, which then handles the events late. */
static inline __attribute__((always_inline)) void account(Arm920tAot *run, unsigned count)
{
    run->left -= count;
    *run->steps += count;
    unsigned clocks = count * run->cost;
    if (__builtin_expect(clocks > *run->budget, 0)) {
        run->extra += clocks - *run->budget;
        clocks = *run->budget;
    }
    *run->budget -= clocks;
    *run->pending += clocks;
}

/* An instruction inside a fast block which may fault. */
#define MIDDLE(at, handler, done) do { \\
    cpu->r[15] = (at) + 4; \\
    Arm920tStep result_ = handler; \\
    while (cpu->write_count) drain_word(cpu); \\
    if (__builtin_expect(result_ != ARM920T_INSTRUCTION || cpu->r[15] != (at) + 4, 0)) { \\
        account(run, done); \\
        run->pc = (at); \\
        return result_; \\
    } \\
} while (0)

/* The last instruction of a fast block, accounted as by STEP. */
#define LAST(at, handler, stop) do { \\
    cpu->r[15] = (at) + 4; \\
    Arm920tStep result_ = handler; \\
    while (cpu->write_count) drain_word(cpu); \\
    if (__builtin_expect((stop) || result_ != ARM920T_INSTRUCTION || \\
                         repeats(cpu, run, (at)) || run->left <= 1 || \\
                         run->extra || run->cost > *run->budget, 0)) { \\
        run->pc = (at); \\
        return result_; \\
    } \\
    account(run, 1); \\
    if (cpu->r[15] != (at) + 4) goto jumped; \\
} while (0)

#define COLD(at, expected, width, handler, stop) do { \\
    Arm920tStep result_; \\
    int kind_ = cold_step(cpu, run, (at), (expected), handler, (width) | (stop), &result_); \\
    if (__builtin_expect(kind_ != GO, 0)) { \\
        if (kind_ == JUMPED) goto jumped; \\
        if (kind_ == DRAINED) goto drained; \\
        return result_; \\
    } \\
} while (0)

"""


def generate_part(instructions, pages, out, miss, hot=None, entries=frozenset(), fast=False,
                  natives={}):
    """`hot` holds the (mode, address) pairs to inline, None meaning all."""
    # One function per distinct opcode, shared by all of its addresses.
    for mode in ("arm", "thumb"):
        used, inlined = set(), set()
        for key, addresses in pages:
            if key[1] != mode:
                continue
            for address in addresses:
                used.add(instructions[mode][address])
                if hot is None or (mode, address) in hot:
                    inlined.add(instructions[mode][address])
        for opcode in sorted(used):
            if mode == "arm":
                condition = opcode >> 28
            else:
                condition = (opcode >> 8) & 15 if 0xD000 <= opcode < 0xDE00 else 14
            guard = ""
            if condition != 14:
                guard = f"    if (!condition(cpu->cpsr, {condition})) return ARM920T_INSTRUCTION;\n"
            size = "" if opcode in inlined else "__attribute__((cold)) "
            out.write(f"{size}static Arm920tStep {mode}_{opcode:x}(Arm920t *cpu, uint32_t pc)\n{{\n"
                      f"{guard}    return {mode}(cpu, 0x{opcode:x}u, pc);\n}}\n")
    needed = {natives[a] for (_, mode), found in pages if mode == "arm" for a in found if a in natives}
    for name in sorted(needed):
        out.write(f"int {name}(Arm920t *cpu, Arm920tAot *run);\n")
    for (page, mode), addresses in pages:
        width = WIDTH[mode]
        thumb = "true" if mode == "thumb" else "false"
        start = page * PAGE
        cold = hot is not None and not any((mode, address) in hot for address in addresses)
        size = "__attribute__((cold)) " if cold else ""
        out.write(f"""
{size}Arm920tStep page_{mode}_{start:08x}(Arm920t *cpu, Arm920tAot *run)
{{
entry:
    switch (cpu->r[15]) {{
""")
        group = blocks if fast else runs
        grouped = group(instructions, addresses, hot, entries) if mode == "arm" else {}
        first, length = None, 0
        for index, address in enumerate(addresses):
            opcode = instructions[mode][address]
            stop = "true" if leaves(mode, opcode) else "false"
            inline = hot is None or (mode, address) in hot
            entered = mode == "arm" and address in natives
            if entered:
                out.write(f"""    case 0x{address:08x}u:
        switch ({natives[address]}(cpu, run)) {{
        case 1: goto jumped;
        case 2: return ARM920T_INSTRUCTION;
        }}
""")
            if address in grouped:
                first, length = address, grouped[address]
                words = [instructions[mode][first + 4 * i] for i in range(length)]
                case = "" if entered else f"    case 0x{first:08x}u:\n"
                if fast:
                    expected = ", ".join(f"0x{w:x}u" for w in words)
                    out.write(f"""{case}    {{
        const void *code_ = block_ready(cpu, run, 0x{first:08x}u, {length});
        static const uint32_t expected_[] = {{{expected}}};
        if (__builtin_expect(code_ && !memcmp(code_, expected_, sizeof expected_), 1)) {{
{block_body(words, first)}            goto after_{first:08x};
        }}
    }}
""")
                else:
                    out.write(f"""{case}    {{
        const void *code_ = run_ready(cpu, run, 0x{first:08x}u, {length});
        static const uint32_t expected_[] = {{{", ".join(f"0x{w:x}u" for w in words)}}};
        if (__builtin_expect(code_ && !memcmp(code_, expected_, sizeof expected_), 1)) {{
""")
                    for i, word in enumerate(words):
                        out.write(f"            cpu->r[15] = 0x{first + 4 * i + 4:08x}u;\n"
                                  f"            arm_{word:x}(cpu, 0x{first + 4 * i:08x}u);\n")
                    out.write(f"""            run->left -= {length};
            *run->steps += {length};
            *run->budget -= {length} * run->cost;
            *run->pending += {length} * run->cost;
            goto after_{first:08x};
        }}
    }}
""")
            in_run = first is not None and address < first + 4 * length
            if inline and not in_run:
                step = (f"STEP(0x{address:08x}u, 0x{opcode:x}u, {width}, {thumb}, "
                        f"{mode}_{opcode:x}(cpu, 0x{address:08x}u), {stop})")
            else:
                step = f"COLD(0x{address:08x}u, 0x{opcode:x}u, {width}, {mode}_{opcode:x}, {stop})"
            # the first instruction of a run follows the case label of the run
            label = "" if address == first or entered else f"    case 0x{address:08x}u:\n"
            out.write(f"{label}        {step};\n")
            if in_run and address == first + 4 * (length - 1):
                out.write(f"    after_{first:08x}:;\n")
                first = None
            following = addresses[index + 1] if index + 1 < len(addresses) else None
            if following != address + width:
                out.write("        goto jumped;\n")
        test = "!(cpu->cpsr & T)" if mode == "arm" else "(cpu->cpsr & T)"
        out.write(f"""    default:
        return {miss};
    }}
jumped:
    if (cpu->r[15] - 0x{start:08x}u < PAGE_BYTES && {test}) goto entry;
    return AGAIN;
drained:
    run->pc = cpu->r[15];
    count_drained(cpu, run);
    return step_drained(cpu);
}}
""")



# Written in the main file by --hybrid: the code the map covers but which hardly
# ever runs is not worth a copy of its own.  The interpreter, which the binary
# holds anyway, runs it without leaving the loop; one bit tells its addresses.
COLD_LOOP = """
static inline bool is_cold(uint32_t pc, bool is_thumb)
{
    uint32_t slot = pc / PAGE_BYTES - COLD_FIRST;
    if (slot >= COLD_PAGES || !cold_slot[is_thumb][slot]) return false;
    uint32_t bit = (pc % PAGE_BYTES) >> (is_thumb ? 1 : 2);
    return cold_bits[cold_slot[is_thumb][slot] - 1 + bit / 32] >> (bit % 32) & 1;
}

static Arm920tStep cold(Arm920t *cpu, Arm920tAot *run)
{
    for (;;) {
        uint32_t at = cpu->r[15], opcode, physical;
        bool is_thumb = (cpu->cpsr & T) != 0;
        drain_word(cpu);
        if (__builtin_expect(cpu->waiting || (cpu->irq && !(cpu->cpsr & I)) ||
                             (cpu->fiq_line && !(cpu->cpsr & F)), 0)) {
            run->pc = at;
            count_drained(cpu, run);
            return step_drained(cpu);
        }
        if (__builtin_expect(!memory(cpu, S3C2400_FETCH, at, is_thumb ? 2 : 4, false, false,
                                     &opcode, &physical), 0)) {
            run->pc = at;
            return exception(cpu, ABT, 12, at + 4);
        }
        Arm920tStep result = execute(cpu, at, physical, opcode, is_thumb);
        bool stop = is_thumb ? (opcode & 0xff00u) == 0xdf00u
                             : (opcode & 0x0f000000u) == 0x0f000000u;
        bool back = at - cpu->r[15] < 128u && cpu->effects == run->effects;
        if (at - cpu->r[15] < 128u) run->effects = cpu->effects;
        if (__builtin_expect(stop || result != ARM920T_INSTRUCTION || back || run->left <= 1 ||
                             run->cost > *run->budget, 0)) {
            run->pc = at;
            return result;
        }
        --run->left;
        ++*run->steps;
        *run->budget -= run->cost;
        *run->pending += run->cost;
        if (!is_cold(cpu->r[15], (cpu->cpsr & T) != 0)) return AGAIN;
    }
}
"""


def cold_tables(instructions, hot):
    """The C tables of the covered addresses left out of the recompiled pages."""
    found = {}
    for number, mode in enumerate(("arm", "thumb")):
        for address in instructions[mode]:
            if hot is not None and (mode, address) not in hot:
                bits = found.setdefault((number, address // PAGE), [0] * (PAGE // WIDTH[mode] // 32))
                bit = address % PAGE // WIDTH[mode]
                bits[bit // 32] |= 1 << (bit % 32)
    first = min((page for _, page in found), default=0)
    span = max((page for _, page in found), default=0) - first + 1
    slots, words = [{}, {}], []
    for (number, page), bits in sorted(found.items()):
        slots[number][page - first] = len(words) + 1
        words += bits
    text = f"#define COLD_FIRST 0x{first:x}u\n#define COLD_PAGES {span}u\n"
    text += f"static const uint32_t cold_slot[2][{span}] = {{\n"
    for listed in slots:
        text += "    {" + (", ".join(f"[{at}] = {to}" for at, to in listed.items()) or "0") + "},\n"
    text += "};\n"
    text += "static const uint32_t cold_bits[] = {\n"
    for at in range(0, len(words), 8):
        text += "    " + ", ".join(f"0x{w:x}u" for w in words[at:at + 8]) + ",\n"
    return text + ("" if words else "    0,\n") + "};\n"


def generate_pages(instructions, path, hot=None, inline=None, count=False, fast=False,
                   hybrid=False):
    """Write the parts for the covered pages (those in `hot` if given)."""
    entries = frozenset(targets(instructions))
    pages = {}
    # --hybrid leaves the cold instructions to the interpreter.
    kept = inline if hybrid else None
    for mode in ("arm", "thumb"):
        for address in instructions[mode]:
            if kept is None or (mode, address) in kept:
                pages.setdefault((address // PAGE, mode), []).append(address)
    if hot is not None:
        pages = {key: found for key, found in pages.items() if key[0] * PAGE in hot}
    parts, size = [[]], 0
    for key, addresses in sorted(pages.items()):
        if size >= PART:
            parts.append([])
            size = 0
        parts[-1].append((key, sorted(addresses)))
        size += len(addresses)
    stem = os.path.splitext(path)[0]
    for stale in glob.glob(glob.escape(stem) + ".part[0-9][0-9][0-9].c"):
        os.remove(stale)
    def header(index):
        renamed = "".join(f"#define {name} m0_part{index}_{name}\n" for name in PUBLIC)
        counter = ("void m0_gp32_count(uint32_t at);\n#define COUNT(at) m0_gp32_count(at)\n"
                   if count else "#define COUNT(at) ((void)0)\n")
        return renamed + HEADER + counter + PART_HEADER

    natives, total = {}, len(parts)
    if hybrid:
        # The HLE routines trap on their first instruction: a native region around
        # the return alone would only add two changes of function to each call.
        stubs = hle_stubs()
        translated = {a: w for a, w in instructions["arm"].items() if a not in stubs}
        natives, total = native.generate(translated, inline, entries, stem, total, header)
    for index, part in enumerate(parts):
        with open(f"{stem}.part{index:03d}.c", "w", encoding="ascii") as out:
            out.write(header(index))
            generate_part(instructions, part, out,
                          "interpret(cpu, run)" if hot is None and kept is None else "MISS",
                          inline, entries,
                          fast, natives)
    return pages, total


COUNTS = """
#include <stdio.h>
#include <stdlib.h>
static uint64_t counts[(MAP_BYTES + 1) / 2];

static void write_counts(void)
{
    const char *name = getenv("M0_GP32_COUNTS");
    FILE *file = name ? fopen(name, "w") : 0;
    for (size_t i = 0; file && i < sizeof counts / sizeof *counts; ++i)
        if (counts[i])
            fprintf(file, "%08x %llu\\n", (unsigned)(MAP_BASE + 2 * i),
                    (unsigned long long)counts[i]);
    if (file) fclose(file);
}

void m0_gp32_count(uint32_t at);
void m0_gp32_count(uint32_t at)
{
    static bool registered;
    if (!registered) registered = !atexit(write_counts);
    if (at - MAP_BASE < MAP_BYTES) ++counts[(at - MAP_BASE) / 2];
}
"""


def hottest(instructions, files, share):
    """The most executed instructions, making up `share` percent of executions.
    A run weighs the same whatever its length."""
    weight = {}
    for name in files:
        with open(name, encoding="ascii") as counted:
            found = [(int(a, 16), int(n)) for a, n in (line.split() for line in counted)]
        total = sum(n for _, n in found)
        for address, n in found:
            weight[address] = weight.get(address, 0) + n / total
    left = sum(weight.values()) * share / 100
    chosen = set()
    for address in sorted(weight, key=lambda a: (-weight[a], a)):
        if left <= 0:
            break
        left -= weight[address]
        chosen.update((mode, address) for mode in WIDTH if address in instructions[mode])
    return chosen


def generate(instructions, path, inline=None, count=False, fast=False, hybrid=False):
    pages, parts = generate_pages(instructions, path, inline=inline, count=count, fast=fast,
                                  hybrid=hybrid)
    with open(path, "w", encoding="ascii") as out:
        out.write(HEADER + map_macros() + MISSES + (COUNTS if count else ""))
        if hybrid:
            out.write(cold_tables(instructions, inline) + COLD_LOOP)
        for page, mode in sorted(pages):
            out.write(f"Arm920tStep page_{mode}_{page * PAGE:08x}(Arm920t *cpu, Arm920tAot *run);\n")
        regions = sorted({page for page, _ in pages})
        out.write("static Page find(uint32_t pc, bool is_thumb)\n{\n    switch (pc / PAGE_BYTES) {\n")
        for page in regions:
            arm = f"page_arm_{page * PAGE:08x}" if (page, "arm") in pages else "0"
            thumb = f"page_thumb_{page * PAGE:08x}" if (page, "thumb") in pages else "0"
            out.write(f"    case 0x{page:x}u: return is_thumb ? {thumb} : {arm};\n")
        out.write("""    default: return 0;
    }
}

Arm920tStep arm920t_aot_run(Arm920t *cpu, Arm920tAot *run)
{
    for (;;) {
        Page page = find(cpu->r[15], (cpu->cpsr & T) != 0);
        Arm920tStep result;
""" + ("""        result = page ? page(cpu, run) : MISS;
        if (result == MISS && is_cold(cpu->r[15], (cpu->cpsr & T) != 0))
            result = cold(cpu, run);
        if (result == MISS) {
""" if hybrid else """        if (!page) {
""") + """            run->pc = cpu->r[15];
            ++*run->interpreted;
            m0_gp32_missed(run->pc, (cpu->cpsr & T) != 0, MISS_UNCOVERED);
            return arm920t_step(cpu);
        }
""" + ("" if hybrid else """        result = page(cpu, run);
""") + """        if (result != AGAIN) return result;
    }
}
""")
    return parts


COMPACT_LOOP = """
Arm920tStep arm920t_aot_run(Arm920t *cpu, Arm920tAot *run)
{
    for (;;) {
        uint32_t at = cpu->r[15], opcode, physical, width;
        bool is_thumb, stop;
        Arm920tStep result = ARM920T_INSTRUCTION;
@DISPATCH@        drain_word(cpu);
        if (__builtin_expect(cpu->waiting || (cpu->irq && !(cpu->cpsr & I)) ||
                             (cpu->fiq_line && !(cpu->cpsr & F)), 0)) {
            run->pc = at;
            count_drained(cpu, run);
            return step_drained(cpu);
        }
        is_thumb = (cpu->cpsr & T) != 0;
@PROFILE@        width = is_thumb ? 2 : 4;
        if (__builtin_expect(!memory(cpu, S3C2400_FETCH, at, width, false, false,
                                     &opcode, &physical), 0)) {
            run->pc = at;
            return exception(cpu, ABT, 12, at + 4);
        }
        cpu->r[15] = at + width;
        if (!is_thumb) {
            unsigned cond = opcode >> 28;
            if (cond == 14 || condition(cpu->cpsr, cond))
                result = arm_shapes[((opcode >> 16) & 0xff0) | ((opcode >> 4) & 15)](cpu, opcode, at);
            stop = (opcode & 0x0f000000u) == 0x0f000000u;
        } else {
            unsigned cond = (opcode >> 8) & 15;
            if ((opcode & 0xf000u) != 0xd000u || cond >= 14 || condition(cpu->cpsr, cond))
                result = thumb_shapes[(opcode >> 6) & 1023](cpu, opcode, at);
            stop = (opcode & 0xff00u) == 0xdf00u;
        }
        if (__builtin_expect(stop || result != ARM920T_INSTRUCTION || at - cpu->r[15] < 128u ||
                             run->left <= 1 || run->cost > *run->budget, 0)) {
            run->pc = at;
            return result;
        }
        --run->left;
        ++*run->steps;
        *run->budget -= run->cost;
        *run->pending += run->cost;
    }
}
"""


def arm_shape(index):
    """(known opcode bits, their mask) for the instructions of one dispatch slot."""
    bits = ((index & 0xff0) << 16) | ((index & 15) << 4)
    group = index >> 9
    if group in (0, 3):
        mask = 0x0ff000f0
        # bit 7 belongs to the shift amount of an immediate shift
        miscellaneous = group == 0 and (bits & 0x01900000) == 0x01000000
        if not bits & 0x10 and not miscellaneous:
            mask &= ~0x80
    elif group >= 5:
        mask = 0x0f000000
    else:
        mask = 0x0ff00000
    return bits & mask, mask


PROFILE = """
#include <stdio.h>
#include <stdlib.h>
static uint64_t page_count[S3C2400_BANK_SIZE / PAGE_BYTES];

static void write_pages(void)
{
    const char *name = getenv("M0_GP32_PAGES");
    FILE *file = name ? fopen(name, "w") : 0;
    while (file) {
        size_t best = 0;
        for (size_t i = 1; i < sizeof page_count / sizeof *page_count; ++i)
            if (page_count[i] > page_count[best]) best = i;
        if (!page_count[best]) break;
        fprintf(file, "%08x %llu\\n", (unsigned)(S3C2400_SDRAM_BASE + best * PAGE_BYTES),
                (unsigned long long)page_count[best]);
        page_count[best] = 0;
    }
    if (file) fclose(file);
}

static void count_page(uint32_t pc)
{
    static bool registered;
    if (!registered) registered = !atexit(write_pages);
    if (pc - S3C2400_SDRAM_BASE < S3C2400_BANK_SIZE)
        ++page_count[(pc - S3C2400_SDRAM_BASE) / PAGE_BYTES];
}
"""


def generate_compact(path, instructions=None, hot=(), profile=False):
    pages = generate_pages(instructions, path, set(hot))[0] if instructions else {}
    if not instructions:
        stem = os.path.splitext(path)[0]
        for stale in glob.glob(glob.escape(stem) + ".part[0-9][0-9][0-9].c"):
            os.remove(stale)
    with open(path, "w", encoding="ascii") as out:
        out.write(HEADER.replace("FOLD bool condition", "static inline bool condition")
                  .replace("FOLD uint32_t shift", "static uint32_t shift") + map_macros() + MISSES)
        tables = {}
        for mode, shapes in (("arm", [arm_shape(i) for i in range(4096)]),
                             ("thumb", [(i << 6, 0xffc0) for i in range(1024)])):
            names, done = [], set()
            for key, mask in shapes:
                name = f"{mode}_{key:x}"
                names.append(name)
                if name not in done:
                    done.add(name)
                    out.write(f"static Arm920tStep {name}(Arm920t *cpu, uint32_t ins, uint32_t pc)\n{{\n"
                              f"    return {mode}(cpu, (ins & ~0x{mask:x}u) | 0x{key:x}u, pc);\n}}\n")
            tables[mode] = names
        out.write("typedef Arm920tStep (*Shape)(Arm920t *, uint32_t, uint32_t);\n")
        for mode, names in tables.items():
            out.write(f"static const Shape {mode}_shapes[{len(names)}] = {{\n")
            for at in range(0, len(names), 6):
                out.write("    " + ", ".join(names[at:at + 6]) + ",\n")
            out.write("};\n")
        if profile:
            out.write(PROFILE)
        dispatch = ""
        if pages:
            first = min(page for page, _ in pages)
            span = max(page for page, _ in pages) - first + 1
            for page, mode in sorted(pages):
                out.write(f"Arm920tStep page_{mode}_{page * PAGE:08x}(Arm920t *cpu, Arm920tAot *run);\n")
            for mode in ("arm", "thumb"):
                out.write(f"static Arm920tStep (*const {mode}_pages[{span}])(Arm920t *, Arm920tAot *) = {{\n")
                for page, found in sorted(pages):
                    if found == mode:
                        out.write(f"    [{page - first}] = page_{mode}_{page * PAGE:08x},\n")
                out.write("};\n")
            dispatch = f"""        {{
            uint32_t slot = at / PAGE_BYTES - 0x{first:x}u;
            if (slot < {span}u) {{
                Arm920tStep (*page)(Arm920t *, Arm920tAot *) =
                    (cpu->cpsr & T) ? thumb_pages[slot] : arm_pages[slot];
                if (page) {{
                    result = page(cpu, run);
                    if (result == AGAIN) continue;
                    if (result != MISS) return result;
                    result = ARM920T_INSTRUCTION;
                    at = cpu->r[15];
                }}
            }}
        }}
"""
        out.write(COMPACT_LOOP.replace("@DISPATCH@", dispatch)
                  .replace("@PROFILE@", "        count_page(at);\n" if profile else ""))
    return sum(len(set(names)) for names in tables.values())


def main():
    if len(sys.argv) == 3 and sys.argv[1] in ("--compact", "--page-profile"):
        profile = sys.argv[1] == "--page-profile"
        print(f"{generate_compact(sys.argv[2], profile=profile)} instruction shapes")
        return
    if len(sys.argv) == 7 and sys.argv[1] == "--compact":
        instructions = load(sys.argv[3], sys.argv[4])
        with open(sys.argv[5], encoding="ascii") as listed:
            hot = [int(line.split()[0], 16) for line in listed if line.strip()]
        hot = hot[:int(sys.argv[6])]
        shapes = generate_compact(sys.argv[2], instructions, hot)
        print(f"{shapes} instruction shapes, {len(hot)} pages recompiled in full")
        return
    options = sys.argv[4:]
    hybrid = options[:1] == ["--hybrid"]
    fast = options[:1] == ["--fast"]
    if fast or hybrid:
        options = options[1:]
    count = options == ["--count"]
    measured, share = [], 99.5
    if options[:1] == ["--hot"]:
        measured = options[1:]
        if measured[-2:-1] == ["--share"]:
            share = float(measured[-1])
            measured = measured[:-2]
    if len(sys.argv) < 4 or (options and not count and not measured):
        sys.exit(__doc__)
    instructions = load(sys.argv[1], sys.argv[2])
    inline = hottest(instructions, measured, share) if measured else set() if count else None
    if measured:
        # Never measured, since they lie outside the image, and run for every
        # byte a program moves to or from its card.
        inline |= {("arm", address) for address in hle_stubs()}
    if hybrid and count:
        sys.exit("--hybrid cannot count executions")
    parts = generate(instructions, sys.argv[3], inline, count, fast, hybrid)
    counts = {mode: len(found) for mode, found in instructions.items()}
    print(f"{counts['arm']} ARM and {counts['thumb']} Thumb instructions recompiled in {parts} parts"
          + ("" if inline is None else f", {len(inline)} inlined"))


if __name__ == "__main__":
    main()
