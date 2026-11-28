"""The native half of recompile.py --hybrid.

Straight ARM code becomes plain C: the guest registers and flags are local
variables, SDRAM is read and written in place, and a whole block is accounted at
once.  Everything else belongs to the precise machine, which runs it with the
recompiled steps: a block only starts when no peripheral event falls inside it,
and an instruction reaching a device, changing the mode or doing anything not
translated here is left, with the state written back, to those steps.

A few routines of the maker's SDK are also recognised by a fingerprint of their
words and run as host code (see routines()).
"""

import hashlib
import struct

# Instructions of a block at most: a nearer event is reached by precise steps.
BLOCK = 4
# One function holds the native blocks of this many bytes of code.
REGION = 4096
# Instructions of the regions sharing one generated file.
PART = 2500

BASE = 0x0C000000
RAM = 0x800000

SUPPORT = """
/* What a native function did: nothing, so the precise step runs; some blocks,
   the next instruction being at r15; or the same with the last instruction left
   to the machine to account, which then looks for an idle loop. */
enum { NATIVE_NONE, NATIVE_MOVED, NATIVE_LAST };

/* Plain memory: no translation, cache, alignment trap, byte swap, buffered
   write or observer.  Nothing native code does can change this or raise an
   interrupt. */
static inline bool native_ready(const Arm920t *cpu)
{
    const S3c2400Bus *bus = cpu->config.bus;
    return !(cpu->control & 0x1087u) && !cpu->write_count && !interrupted(cpu) &&
           !M0_OBSERVER(cpu->config.access) && !M0_OBSERVER(bus->hooks.access) && bus->ram &&
           bus->ram_size >= 0x800000u;
}

/* Every address given to these helpers is a multiple of the access size (of 4
   for load64).  Saying so matters on hosts without unaligned accesses, where
   memcpy would otherwise move one byte at a time. */
static inline uint32_t load32(const uint8_t *at)
{
    uint32_t value;
    __builtin_memcpy(&value, __builtin_assume_aligned(at, 4), 4);
    return value;
}

static inline uint64_t load64(const uint8_t *at)
{
    uint64_t value;
    __builtin_memcpy(&value, __builtin_assume_aligned(at, 4), 8);
    return value;
}

static inline uint32_t load16(const uint8_t *at)
{
    uint16_t value;
    __builtin_memcpy(&value, __builtin_assume_aligned(at, 2), 2);
    return value;
}

/* The stores return whether memory changed: one writing what it already
   holds has no effect, and a loop making only those repeats identically. */
static inline uint32_t store32(uint8_t *at, uint32_t value)
{
    uint32_t old = load32(at);
    __builtin_memcpy(__builtin_assume_aligned(at, 4), &value, 4);
    return old != value;
}

static inline uint32_t store16(uint8_t *at, uint32_t value)
{
    uint32_t old = load16(at);
    uint16_t half = (uint16_t)value;
    __builtin_memcpy(__builtin_assume_aligned(at, 2), &half, 2);
    return old != half;
}

static inline uint32_t store8(uint8_t *at, uint32_t value)
{
    uint32_t old = *at;
    *at = (uint8_t)value;
    return old != (uint8_t)value;
}
"""

CONDITIONS = ("!fz", "fz", "fc", "!fc", "fn >> 31", "!(fn >> 31)", "fv", "!fv",
              "fc && fz", "!(fc && fz)", "(fn >> 31) == fv", "(fn >> 31) != fv",
              "fz && (fn >> 31) == fv", "!(fz && (fn >> 31) == fv)")


def signed(value, bits):
    sign = 1 << (bits - 1)
    return ((value & (2 * sign - 1)) ^ sign) - sign


def kind(ins):
    """The translation an ARM instruction takes, None when it has none."""
    if ins >> 28 == 15:
        return None
    rd, rn, rs, rm = ins >> 12 & 15, ins >> 16 & 15, ins >> 8 & 15, ins & 15
    group, load = ins >> 25 & 7, ins >> 20 & 1
    if ins & 0x0FFFFFF0 == 0x012FFF10:
        return "exchange"
    if ins & 0x0FBF0FFF == 0x010F0000:
        return None
    if ins & 0x0DB0F000 == 0x0120F000 and (ins & 0x02000000 or not ins & 0xFF0):
        return None
    if ins & 0x0FB00FF0 == 0x01000090:
        return None
    if ins & 0x0F8000F0 == 0x00800090:
        return None if 15 in (rn, rd, rs, rm) or rn == rd else "long"
    if ins & 0x0FC000F0 == 0x00000090:
        return None if 15 in (rn, rs, rm) or ins & 0x00200000 and rd == 15 else "multiply"
    if group == 0 and ins & 0x90 == 0x90:
        if not ins & 0x60 or ins & 0x40 and not load:
            return None
        return "single"
    if group in (2, 3):
        return None if group == 3 and ins & 16 else "single"
    if group < 2:
        op = ins >> 21 & 15
        test = 8 <= op <= 11
        if test and not load or not test and rd == 15 and load:
            return None
        return "data"
    if group == 4:
        if ins & 0x00400000 or not ins & 0xFFFF or rn == 15:
            return None
        return "multiple"
    if group == 5:
        return "branch"
    return None


def writes(ins):
    """The registers a translated instruction may write."""
    what = kind(ins)
    rd, rn = ins >> 12 & 15, ins >> 16 & 15
    if what == "data":
        return () if 8 <= ins >> 21 & 15 <= 11 else (rd,)
    if what == "multiply":
        return (rn,)
    if what == "long":
        return (rd, rn)
    if what == "single":
        back = (rn,) if not ins & 0x01000000 or ins & 0x00200000 else ()
        return back + ((rd,) if ins & 0x00100000 else ())
    if what == "multiple":
        loaded = tuple(r for r in range(16) if ins >> r & 1) if ins & 0x00100000 else ()
        return loaded + ((rn,) if ins & 0x00200000 else ())
    return ()


def step(ins):
    """The register an instruction always moves by a non zero constant, or None."""
    if ins >> 28 != 14:
        return None
    what = kind(ins)
    rd, rn = ins >> 12 & 15, ins >> 16 & 15
    if what == "data":
        # add or subtract an immediate from the register itself
        if ins & 0x02000000 and ins >> 21 & 15 in (2, 4) and rd == rn and ins & 255:
            return rd
    elif what == "single":
        if ins & 0x01000000 and not ins & 0x00200000:
            return None
        if ins >> 25 & 7 == 0:
            offset = ins & 0x00400000 and (ins >> 4 & 0xF0 | ins & 15)
        else:
            offset = not ins & 0x02000000 and ins & 0xFFF
        if offset and not (ins & 0x00100000 and rd == rn):
            return rn
    return None


def progresses(arm, at, to):
    """True for a loop of straight code from `to` to its branch at `at` which
    moves a register on every turn: it never returns to the same state, so the
    machine has no idle loop to skip there."""
    body = [arm.get(a) for a in range(to, at, 4)]
    if not body or any(w is None or kind(w) is None or transfers(w) for w in body):
        return False
    written = [r for w in body for r in writes(w)]
    return any((r := step(w)) is not None and written.count(r) == 1 for w in body)


# The software ECC of a SmartMedia page half: the fingerprint of its words (the
# repository holds none of them), their number, and the two literals which follow
# them, the addresses of a table of parities and of its complement.
ECC_SHA256 = "88d72b7b1a8b1bfda9da8269e902b6705f4a2217019fa26f24844d302b476c0e"
ECC_FIRST = 0xE92D4FF3
ECC_WORDS = 349
ECC_LITERALS = 2

# Shared by the routines.
ROUTINE_SUPPORT = """
static bool same_words(const uint8_t *ram, uint32_t at, const uint32_t *words, unsigned count)
{
    uint32_t differs = 0;
    if (gp32_ram_whole(at, 4 * count)) {
        const uint8_t *code = GP32_RAM(ram, at);
        for (unsigned i = 0; i < count; ++i)
            differs |= load32(code + 4 * i) ^ words[i];
    } else {
        for (unsigned i = 0; i < count; ++i)
            differs |= load32(GP32_RAM(ram, at + 4 * i)) ^ words[i];
    }
    return !differs;
}

/* The whole turns, of `count` instructions each, of a loop of `words` words that
   may run now. */
static uint32_t loop_turns(const Arm920t *cpu, const Arm920tAot *run, uint32_t at,
                           const uint32_t *code, unsigned words, unsigned count)
{
    if (!native_ready(cpu) || !run->left || (cpu->cpsr & T))
        return 0;
    /* This instruction, and those the budget holds besides. */
    uint32_t room = arm920t_quotient(*run->budget, run->cost, run->inverse) + 1;
    if (room > run->left)
        room = (uint32_t)run->left;
    if (room < count || !same_words(cpu->config.bus->ram, at, code, words))
        return 0;
    return room / count;
}

static inline bool loop_span(uint32_t at, uint32_t length, uint32_t ram)
{
    return length <= ram && at <= ram - length;
}

static inline uint32_t loop_compare(uint32_t a, uint32_t b)
{
    uint32_t difference = a - b;
    return (difference & N) | (difference ? 0 : Z) | (a >= b ? C : 0) |
           (((a ^ b) & (a ^ difference)) >> 31 ? V : 0);
}

static inline int loop_done(Arm920t *cpu, Arm920tAot *run, uint32_t done, uint32_t last)
{
    ++cpu->effects; /* not an idle loop */
    account(run, done - 1);
    run->pc = last;
    return 1;
}
"""

ECC_C = """
/* The 3 bytes of the SmartMedia ECC of 256 bytes, computed by the host.  The
   SDK does it bit by bit in some 13000 instructions: they are all counted, up
   to each peripheral event at a time so that events and interrupts stay on
   time, the program counter staying on the entry until the last one: the
   registers are untouched until then.  Left as the routine would: its result,
   the registers it may change and the flags; not the dead stack frame below
   sp. */
static const uint32_t ecc_code_@A@[] = {
@CODE@};
static const uint32_t ecc_tables_@A@[] = {
@TABLES@};

static inline uint32_t ecc_parity(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    return 0x6996u >> (value & 15) & 1;
}

static void ecc_read_@A@(const uint8_t *ram, uint32_t data, uint32_t *words)
{
    if (gp32_ram_whole(data, 256)) {
        __builtin_memcpy(words, GP32_RAM(ram, data), 256);
    } else {
        for (unsigned i = 0; i < 256; ++i)
            ((uint8_t *)words)[i] = *GP32_RAM(ram, data + i);
    }
}

int ecc_@A@(Arm920t *cpu, Arm920tAot *run)
{
    /* The call under way: instructions left, its stack and its data. */
    static uint32_t left, frame, source;
    if (!native_ready(cpu) || !run->left || (cpu->cpsr & T))
        return 0;
    uint8_t *const ram = cpu->config.bus->ram;
    uint32_t data = cpu->r[0] - 0x@BASE@u, out = cpu->r[1] - 0x@BASE@u;
    uint32_t words[64];
    if (!left || frame != cpu->r[13] || source != cpu->r[0]) {
        if (data > 0x@RAM@u - 256u || out > 0x@RAM@u - 3u ||
            !same_words(ram, 0x@AT@u, ecc_code_@A@, @COUNT@u) ||
            !same_words(ram, 0x@T1@u, ecc_tables_@A@, 4) ||
            !same_words(ram, 0x@T0@u, ecc_tables_@A@ + 4, 4))
            return 0;
        /* A byte takes one instruction more when the parity of its high half
           is even, except for 15. */
        ecc_read_@A@(ram, data, words);
        uint32_t all = 0, lanes = 0;
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t w = words[i], odd = w ^ w >> 1, ones = w & w >> 1;
            odd ^= odd >> 2;
            ones &= ones >> 2;
            lanes += (~(odd | ones) & 0x10101010u) >> 4;
            all ^= w;
        }
        lanes = (lanes & 0x00ff00ffu) + (lanes >> 8 & 0x00ff00ffu);
        left = 23 + 256 * 20 + ((lanes + (lanes >> 16)) & 0xffffu) +
               70 + 2304 + 2 + 896 + 2 + 576 + 4935 + 3 +
               (ecc_parity(all) ? 27 : 9) + 73;
        frame = cpu->r[13];
        source = cpu->r[0];
    }
    /* This instruction, and those the budget holds besides. */
    uint32_t take = arm920t_quotient(*run->budget, run->cost, run->inverse) + 1;
    if (take > left)
        take = left;
    if (take > run->left)
        take = (uint32_t)run->left;
    ++cpu->effects; /* not an idle loop */
    account(run, take - 1);
    left -= take;
    if (left) {
        run->pc = 0x@PC@u;
        return 1;
    }
    ecc_read_@A@(ram, data, words);
    /* Bit k of `set`: the parity of the bytes whose index has bit k + 2. */
    uint32_t all = 0, set = 0;
    for (unsigned i = 0; i < 64; ++i) {
        all ^= words[i];
        set ^= i & -ecc_parity(words[i]);
    }
    unsigned whole = ecc_parity(all);
    /* Parities over the bytes whose index has a bit set (d) or clear (c). */
    unsigned d = ecc_parity(all & 0xff00ff00u) | ecc_parity(all & 0xffff0000u) << 1 | set << 2;
    unsigned c = d ^ (whole ? 0xffu : 0u);
    uint32_t low = 0, high = 0;
    for (unsigned k = 0; k < 4; ++k) {
        low |= ((d >> k & 1) << 1 | (c >> k & 1)) << (2 * k);
        high |= ((d >> (k + 4) & 1) << 1 | (c >> (k + 4) & 1)) << (2 * k);
    }
    uint32_t column = all ^ all >> 16;
    column = (column ^ column >> 8) & 0xff;
    uint32_t third = ecc_parity(column & 0xf0) << 7 | ecc_parity(column & 0x0f) << 6 |
                     ecc_parity(column & 0xcc) << 5 | ecc_parity(column & 0x33) << 4 |
                     ecc_parity(column & 0xaa) << 3 | ecc_parity(column & 0x55) << 2;
    *GP32_RAM(ram, out) = (uint8_t)~low;
    *GP32_RAM(ram, out + 1) = (uint8_t)~high;
    *GP32_RAM(ram, out + 2) = (uint8_t)~third;
    uint32_t back = cpu->r[14];
    cpu->r[0] = 0;
    cpu->r[2] = (c >> 6 & 1) << 4;
    cpu->r[3] = (c >> 5 & 1) << 2;
    cpu->r[12] = cpu->r[1];
    cpu->r[14] = c >> 4 & 1;
    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | C | (!whole || (c & 1) ? Z : 0);
    cpu->r[15] = back & ~3u;
    run->pc = 0x@LAST@u;
    return 1;
}
"""


# Inner loops, recognised the same way: the first word, the number of words and
# their fingerprint.  Each turns once per call of its head.
LOOPS = {
    # 8 pixels of 8 bits drawn over the picture, a key colour being left out
    "blit": (0xE3550008, 49,
             "8b7ced3dd4a0aa27eae08c8ac4b0694aad9fe072421147ad3c5788392d1ac88b"),
    # one pixel of a 1 bit per pixel pattern, drawn where it is set
    "mask": (0xE3190001, 12,
             "b45dd10e37cfac74c9f38c0aaeda0dffdf12d7defdb8480df7537975f68f8fcd"),
    # one byte copied
    "copy": (0xE3520000, 6,
             "3bd22466bd95918d91bc986d6d632eafcec9d820407fc1d2cf2cffcc23e97c5a"),
    # one sample of a sound added to a stereo pair of the output; the loop holds
    # the code which wraps the position, never run here
    "mix": (0xE150000C, 26,
            "4a42a0a4ebc09d434778be22bc506a74a253a1381bc186df86e99b58595dc63b"),
}

LOOP_C = """
/* The loop at 0x@A@, run by the host for as many whole turns as fit before the
   next peripheral event: the same registers, flags, memory and instructions.
   Anything else is left to the recompiled steps. */
static const uint32_t loop_code_@A@[] = {
@CODE@};
@BODY@"""

BLIT_C = """
/* r0 the key colour, r1 the destination, r2 the source, r5 the pixels left. */
int blit_@A@(Arm920t *cpu, Arm920tAot *run)
{
    uint32_t *r = cpu->r;
    uint32_t most = loop_turns(cpu, run, 0x@AT@u, loop_code_@A@, @COUNT@u, @COUNT@u);
    if (!most || (int32_t)r[5] < 8)
        return 0;
    uint32_t turns = r[5] >> 3;
    if (turns > most)
        turns = most;
    uint32_t to = r[1] - 0x@BASE@u, from = r[2] - 0x@BASE@u, bytes = 8 * turns;
    if ((to | from) & 3u || !loop_span(to, bytes, 0x@RAM@u) || from > 0x@RAM@u - bytes ||
        (to < 0x@AT@u + 4 * @COUNT@u && 0x@AT@u < to + bytes))
        return 0;
    uint8_t *const ram = cpu->config.bus->ram;
    /* A byte of the source equals the key when it is zero in `source ^ keys`;
       no byte equals a key above 255. */
    const uint32_t keys = r[0] * 0x01010101u, valid = r[0] < 256 ? ~0u : 0;
    uint32_t first = 0, second = 0, low = 0, high = 0, kept = 0, byte = 0;
    for (unsigned turn = 0; turn < turns; ++turn, to += 8, from += 8) {
        first = load32(GP32_RAM(ram, from));
        second = load32(GP32_RAM(ram, from + 4));
        byte = second & 255u; /* compared last */
        low = load32(GP32_RAM(ram, to));
        high = load32(GP32_RAM(ram, to + 4));
        uint32_t x = first ^ keys;
        uint32_t same = ~(((x & 0x7f7f7f7fu) + 0x7f7f7f7fu) | x | 0x7f7f7f7fu);
        same = ((same >> 7) * 255u) & valid;
        low = (low & same) | (first &= ~same);
        x = second ^ keys;
        same = ~(((x & 0x7f7f7f7fu) + 0x7f7f7f7fu) | x | 0x7f7f7f7fu);
        same = ((same >> 7) * 255u) & valid;
        high = (high & same) | (second &= ~same);
        kept = ~same;
        store32(GP32_RAM(ram, to), low);
        store32(GP32_RAM(ram, to + 4), high);
    }
    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | loop_compare(r[0], byte);
    r[1] += bytes;
    r[2] += bytes;
    r[5] -= bytes;
    r[6] = first;
    r[7] = second;
    r[8] = low;
    r[9] = high;
    r[10] = kept;
    return loop_done(cpu, run, turns * @COUNT@u, 0x@LAST@u);
}
"""

MASK_C = """
/* r6 the destination, r7 the pattern, r8 its bits taken, r9 its current byte,
   r10 the colour, r14 the pixels left.  The last pixel, which leaves the loop,
   is not run here. */
int mask_@A@(Arm920t *cpu, Arm920tAot *run)
{
    uint32_t *r = cpu->r;
    uint32_t turns = loop_turns(cpu, run, 0x@AT@u, loop_code_@A@, @COUNT@u, @COUNT@u);
    if (!turns || r[14] < 2 || r[14] > 0x@RAM@u)
        return 0;
    if (turns > r[14] - 1)
        turns = r[14] - 1;
    uint32_t to = r[6] - 0x@BASE@u, from = r[7] - 0x@BASE@u;
    if (!loop_span(to, turns, 0x@RAM@u) || from > 0x@RAM@u - turns ||
        (to < 0x@AT@u + 4 * @COUNT@u && 0x@AT@u < to + turns))
        return 0;
    uint8_t *const ram = cpu->config.bus->ram;
    uint32_t bits = r[9], taken = r[8], done = 0;
    for (uint32_t turn = 0; turn < turns; ++turn, ++to) {
        /* One instruction less when the pixel is not drawn. */
        done += @COUNT@u - 1;
        if (bits & 1u) {
            *GP32_RAM(ram, to) = (uint8_t)r[10];
            ++done;
        }
        bits >>= 1;
        if (!(++taken & 7u))
            bits = *GP32_RAM(ram, from++);
    }
    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | C | (taken & 7u ? 0 : Z);
    r[6] += turns;
    r[7] = from + 0x@BASE@u;
    r[8] = taken;
    r[9] = bits;
    r[14] -= turns;
    return loop_done(cpu, run, done, 0x@LAST@u);
}
"""

COPY_C = """
/* r0 the destination, r1 the source, r2 the bytes left. */
int copy_@A@(Arm920t *cpu, Arm920tAot *run)
{
    uint32_t *r = cpu->r;
    uint32_t turns = loop_turns(cpu, run, 0x@AT@u, loop_code_@A@, @COUNT@u, @COUNT@u);
    if (!turns || !r[2])
        return 0;
    if (turns > r[2])
        turns = r[2];
    uint32_t to = r[0] - 0x@BASE@u, from = r[1] - 0x@BASE@u;
    if (!loop_span(to, turns, 0x@RAM@u) || from > 0x@RAM@u - turns ||
        (to < 0x@AT@u + 4 * @COUNT@u && 0x@AT@u < to + turns))
        return 0;
    uint8_t *const ram = cpu->config.bus->ram;
    uint32_t last = 0;
    for (uint32_t left = turns; left; --left) {
        last = *GP32_RAM(ram, from++);
        *GP32_RAM(ram, to++) = (uint8_t)last;
    }
    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | loop_compare(r[2] - turns + 1, 0);
    r[0] += turns;
    r[1] += turns;
    r[2] -= turns;
    r[4] = last;
    return loop_done(cpu, run, turns * @COUNT@u, 0x@LAST@u);
}
"""

MIX_C = """
/* r0 the position in the samples (12 bits of fraction), r9 its step, r12 their
   length, r4 their address, r6 the pairs left, r14 the 16 bit output, the volume
   at r11 - 64.  A turn which wraps the position, and the last one, are not run
   here.  A turn takes 16 instructions. */
int mix_@A@(Arm920t *cpu, Arm920tAot *run)
{
    uint32_t *r = cpu->r;
    uint32_t most = loop_turns(cpu, run, 0x@AT@u, loop_code_@A@, @COUNT@u, 16u);
    uint32_t position = r[0], end = r[12];
    if (!most || (int32_t)r[6] < 2 || position >= end)
        return 0;
    if (most > r[6] - 1)
        most = r[6] - 1;
    if (most > 0x@RAM@u / 4)
        most = 0x@RAM@u / 4;
    uint32_t to = r[14] - 0x@BASE@u, level = r[11] - 64 - 0x@BASE@u;
    if ((to & 1u) || (level & 3u) || !loop_span(level, 4, 0x@RAM@u) ||
        !loop_span(to, 4 * most, 0x@RAM@u) ||
        (to < 0x@AT@u + 4 * @COUNT@u && 0x@AT@u < to + 4 * most) ||
        (to < level + 4 && level < to + 4 * most))
        return 0;
    uint8_t *const ram = cpu->config.bus->ram;
    const uint32_t volume = load32(GP32_RAM(ram, level));
    const uint32_t samples = r[4] - 0x@BASE@u, step = r[9];
    uint32_t turns = 0, added = 0, right = 0;
    for (; turns < most && position < end; ++turns, to += 4, position += step) {
        uint32_t from = samples + (position >> 12);
        if (from >= 0x@RAM@u)
            break;
        added = volume * (uint32_t)(int8_t)*GP32_RAM(ram, from);
        uint8_t *left = GP32_RAM(ram, to);
        (void)store16(left, load16(left) + added);
        right = load16(GP32_RAM(ram, to + 2));
        (void)store16(GP32_RAM(ram, to + 2), right + added);
    }
    if (!turns)
        return 0;
    r[6] -= turns;
    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | loop_compare(r[6], 0);
    r[0] = position;
    r[1] = added;
    r[2] = right + added;
    r[3] = right;
    r[14] += 4 * turns;
    return loop_done(cpu, run, 16 * turns, 0x@LAST@u);
}
"""


def routines(image, base):
    """{entry address: (function name, C source)} of the recognised routines."""
    found = {}
    first = struct.pack("<I", ECC_FIRST)
    size = 4 * (ECC_WORDS + ECC_LITERALS)
    at = image.find(first)
    while at >= 0:
        body = image[at:at + 4 * ECC_WORDS]
        if at % 4 == 0 and at + size <= len(image) and hashlib.sha256(body).hexdigest() == ECC_SHA256:
            words = struct.unpack_from(f"<{ECC_WORDS + ECC_LITERALS}I", image, at)
            t1, t0 = (address - base for address in words[ECC_WORDS:])
            parity = [bin(i).count("1") & 1 for i in range(16)]
            if all(0 <= t <= len(image) - 16 and not t & 3 for t in (t1, t0)) and \
                    list(image[t1:t1 + 16]) == parity and \
                    list(image[t0:t0 + 16]) == [1 - p for p in parity]:
                address = base + at
                tables = struct.unpack("<8I", image[t1:t1 + 16] + image[t0:t0 + 16])
                values = {
                    "A": f"{address:08x}", "BASE": f"{BASE:08x}", "RAM": f"{RAM:x}",
                    "AT": f"{address - BASE:x}", "T1": f"{base + t1 - BASE:x}",
                    "T0": f"{base + t0 - BASE:x}", "COUNT": str(len(words)),
                    "PC": f"{address:08x}", "LAST": f"{address + 4 * (ECC_WORDS - 1):08x}",
                    "CODE": "".join(f"    0x{w:08x}u,\n" for w in words),
                    "TABLES": "".join(f"    0x{w:08x}u,\n" for w in tables),
                }
                source = ECC_C
                for key, value in values.items():
                    source = source.replace(f"@{key}@", value)
                found[address] = (f"ecc_{address:08x}", source)
        at = image.find(first, at + 1)
    for name, (head, count, fingerprint) in LOOPS.items():
        first = struct.pack("<I", head)
        at = image.find(first)
        while at >= 0:
            body = image[at:at + 4 * count]
            if at % 4 == 0 and hashlib.sha256(body).hexdigest() == fingerprint:
                address = base + at
                values = {
                    "A": f"{address:08x}", "BASE": f"{BASE:08x}", "RAM": f"{RAM:x}",
                    "AT": f"{address - BASE:x}", "COUNT": str(count),
                    "LAST": f"{address + 4 * (count - 1):08x}",
                    "CODE": "".join(f"    0x{w:08x}u,\n"
                                    for w in struct.unpack(f"<{count}I", body)),
                }
                source = LOOP_C.replace("@BODY@", {"blit": BLIT_C, "copy": COPY_C, "mask": MASK_C, "mix": MIX_C}[name])
                for key, value in values.items():
                    source = source.replace(f"@{key}@", value)
                found[address] = (f"{name}_{address:08x}", source)
            at = image.find(first, at + 1)
    return found


class Region:
    """The C function holding the blocks of one region."""

    def __init__(self, blocks, arm):
        self.blocks = blocks  # {first address: [opcodes]}
        self.arm = arm  # every covered instruction
        self.out = []
        self.read, self.written = set(), set()
        self.sets_flags = False

    def reg(self, n, pc, ahead=8):
        if n == 15:
            return f"0x{(pc + ahead) & 0xFFFFFFFF:08x}u"
        self.read.add(n)
        return f"r{n}"

    def target(self, n):
        self.written.add(n)
        return f"r{n}"

    def jump(self, at, to):
        """A branch to a known address; a short backward one without effect since
        the last goes to the machine, which skips idle loops."""
        code = []
        if (at - to) & 0xFFFFFFFF < 128 and not progresses(self.arm, at, to):
            code.append(f"if (fx == run->effects) {{ pc = 0x{to:08x}u; at = 0x{at:08x}u; goto last; }}")
            code.append("run->effects = fx;")
        if to in self.blocks:
            code.append(f"goto L_{to:08x};")
        else:
            code.append(f"pc = 0x{to:08x}u; goto out;")
        return code

    def indirect(self, at, value, exchange=False):
        """A branch to a computed address."""
        if exchange:
            code = [f"{{ uint32_t to_ = {value};",
                    "  thumb = to_ & 1;",
                    "  pc = to_ & (thumb ? ~1u : ~3u); }"]
        else:
            code = [f"pc = ({value}) & ~3u;"]
        code += [f"if (0x{at:08x}u - pc < 128u) {{",
                 f"    if (fx == run->effects) {{ at = 0x{at:08x}u; goto last; }}",
                 "    run->effects = fx;",
                 "}",
                 "goto dispatch;"]
        return code

    def operand(self, ins, pc, flags):
        """(statements, expression of the second operand, of its carry)."""
        if ins & 0x02000000:
            turn = 2 * (ins >> 8 & 15)
            value = ins & 255
            if turn:
                value = ((value >> turn) | (value << (32 - turn))) & 0xFFFFFFFF
            return [], f"0x{value:x}u", str(value >> 31) if turn else "fc"
        rm, kind_ = ins & 15, ins >> 5 & 3
        if ins & 16:
            rs = ins >> 8 & 15
            return ([f"bool sc_ = fc;",
                     f"uint32_t b_ = shift({self.reg(rm, pc, 12)}, {kind_}, "
                     f"{self.reg(rs, pc)} & 255, true, &sc_);"], "b_", "sc_")
        n = ins >> 7 & 31
        x = self.reg(rm, pc)
        if kind_ == 0:
            if not n:
                return [], x, "fc"
            value, carry = f"{x} << {n}", f"{x} >> {32 - n} & 1"
        elif kind_ == 1:
            value = f"{x} >> {n}" if n else "0u"
            carry = f"{x} >> {(n or 32) - 1} & 1"
        elif kind_ == 2:
            value = f"(uint32_t)((int32_t){x} >> {n or 31})"
            carry = f"{x} >> {(n or 32) - 1} & 1"
        elif n:
            value, carry = f"({x} >> {n} | {x} << {32 - n})", f"{x} >> {n - 1} & 1"
        else:
            value, carry = f"({x} >> 1 | (uint32_t)fc << 31)", f"{x} & 1"
        if not flags:
            return [], f"({value})", None
        return [f"uint32_t b_ = {value};", f"bool sc_ = {carry};"], "b_", "sc_"

    def data(self, ins, pc):
        op, rd, rn = ins >> 21 & 15, ins >> 12 & 15, ins >> 16 & 15
        test = 8 <= op <= 11
        flags = bool(ins & 0x00100000) and (test or rd != 15)
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        by_register = not ins & 0x02000000 and ins & 16
        code, b, carry = self.operand(ins, pc, flags and logical)
        a = None
        if op not in (13, 15):
            a = self.reg(rn, pc, 12 if by_register else 8)
        if logical:
            result = {0: "{a} & {b}", 8: "{a} & {b}", 1: "{a} ^ {b}", 9: "{a} ^ {b}",
                      12: "{a} | {b}", 13: "{b}", 14: "{a} & ~{b}",
                      15: "~{b}"}[op].format(a=a, b=b)
            code.append(f"uint32_t v_ = {result};")
            if flags:
                code.append(f"fn = fz = v_; fc = {carry};")
        else:
            x, y = (b, a) if op in (3, 7) else (a, b)
            code.append(f"uint32_t x_ = {x}, y_ = {y};")
            if op in (4, 11):
                code.append("uint32_t v_ = x_ + y_;")
                if flags:
                    code.append("fc = v_ < x_; fv = (~(x_ ^ y_) & (x_ ^ v_)) >> 31;")
            elif op in (2, 3, 10):
                code.append("uint32_t v_ = x_ - y_;")
                if flags:
                    code.append("fc = x_ >= y_; fv = ((x_ ^ y_) & (x_ ^ v_)) >> 31;")
            else:
                if op != 5:
                    code.append("y_ = ~y_;")
                code.append("uint64_t w_ = (uint64_t)x_ + y_ + fc;")
                code.append("uint32_t v_ = (uint32_t)w_;")
                if flags:
                    code.append("fc = w_ >> 32; fv = (~(x_ ^ y_) & (x_ ^ v_)) >> 31;")
            if flags:
                code.append("fn = fz = v_;")
        if flags:
            self.sets_flags = True
        if test:
            code.append("(void)v_;")
        elif rd == 15:
            code += self.indirect(pc, "v_")
        else:
            code.append(f"{self.target(rd)} = v_;")
        return code

    def multiply(self, ins, pc):
        rd, rn = ins >> 16 & 15, ins >> 12 & 15
        value = f"{self.reg(ins & 15, pc)} * {self.reg(ins >> 8 & 15, pc)}"
        if ins & 0x00200000:
            value += f" + {self.reg(rn, pc)}"
        code = [f"{self.target(rd)} = {value};"]
        if ins & 0x00100000:
            self.sets_flags = True
            code.append(f"fn = fz = r{rd};")
        return code

    def long(self, ins, pc):
        high, low = ins >> 16 & 15, ins >> 12 & 15
        m, s = self.reg(ins & 15, pc), self.reg(ins >> 8 & 15, pc)
        if ins & 0x00400000:
            value = f"(uint64_t)((int64_t)(int32_t){m} * (int32_t){s})"
        else:
            value = f"(uint64_t){m} * {s}"
        code = [f"uint64_t w_ = {value};"]
        if ins & 0x00200000:
            code.append(f"w_ += (uint64_t){self.reg(high, pc)} << 32 | {self.reg(low, pc)};")
        code.append(f"{self.target(high)} = (uint32_t)(w_ >> 32); "
                    f"{self.target(low)} = (uint32_t)w_;")
        if ins & 0x00100000:
            self.sets_flags = True
            code.append(f"fn = r{high}; fz = r{high} | r{low};")
        return code

    def single(self, ins, pc, leave):
        group, rd, rn, rm = ins >> 25 & 7, ins >> 12 & 15, ins >> 16 & 15, ins & 15
        load = ins >> 20 & 1
        pre, up, wb = ins >> 24 & 1, ins >> 23 & 1, ins >> 21 & 1
        half = group == 0
        sign = half and ins & 0x40
        if half:
            width = 2 if ins & 0x20 else 1
            if ins & 0x00400000:
                offset = f"0x{(ins >> 4 & 0xf0) | rm:x}u"
            else:
                offset = self.reg(rm, pc)
        else:
            width = 1 if ins & 0x00400000 else 4
            if ins & 0x02000000:
                # a register offset: the shifted operand, by an immediate here
                offset = self.operand(ins & ~0x02000000, pc, False)[1]
            else:
                offset = f"0x{ins & 0xfff:x}u"
        base = self.reg(rn, pc)
        adjusted = f"{base} {'+' if up else '-'} {offset}"
        writeback = (not pre or wb) and rn != 15
        code = [f"uint32_t to_ = {adjusted};",
                f"uint32_t a_ = {'to_' if pre else base};",
                f"uint32_t o_ = a_ - 0x{BASE:08x}u;",
                f"if (__builtin_expect(o_ >= 0x{RAM:x}u, 0)) {{ {leave} }}"]
        if load:
            if width == 4:
                code.append("uint32_t v_ = ror(load32(GP32_RAM(ram, o_ & ~3u)), (a_ & 3) * 8);")
            elif width == 1:
                value = "(uint32_t)(int8_t)*GP32_RAM(ram, o_)" if sign else "*GP32_RAM(ram, o_)"
                code.append(f"uint32_t v_ = {value};")
            elif sign:
                code.append("uint32_t v_ = o_ & 1 ? (uint32_t)(int8_t)*GP32_RAM(ram, o_) "
                            ": (uint32_t)(int16_t)load16(GP32_RAM(ram, o_));")
            else:
                code.append("uint32_t v_ = ror(load16(GP32_RAM(ram, o_ & ~1u)), (o_ & 1) * 8);")
        else:
            value = self.reg(rd, pc, 12)
            if width == 4:
                code.append(f"fx += store32(GP32_RAM(ram, o_ & ~3u), {value});")
            elif width == 2:
                code.append(f"fx += store16(GP32_RAM(ram, o_ & ~1u), {value});")
            else:
                code.append(f"fx += store8(GP32_RAM(ram, o_), {value});")
        if writeback:
            code.append(f"{self.target(rn)} = to_;")
        else:
            code.append("(void)to_;")
        if load:
            if rd == 15:
                code += self.indirect(pc, "v_")
            else:
                code.append(f"{self.target(rd)} = v_;")
        return code

    def multiple(self, ins, pc, leave):
        rn, mask = ins >> 16 & 15, ins & 0xFFFF
        load = ins >> 20 & 1
        pre, up, wb = ins >> 24 & 1, ins >> 23 & 1, ins >> 21 & 1
        listed = [r for r in range(16) if mask >> r & 1]
        size = 4 * len(listed)
        base = self.reg(rn, pc)
        start = (0 if up else -size) + (4 if up == pre else 0)
        code = [f"uint32_t last_ = {base} {'+' if up else '-'} {size}u;",
                f"uint32_t o_ = (({base} + 0x{start & 0xFFFFFFFF:x}u) & ~3u) - 0x{BASE:08x}u;",
                f"if (__builtin_expect(o_ > 0x{RAM - size:x}u || "
                f"!gp32_ram_whole(o_, {size}u), 0)) {{ {leave} }}"]
        # A longer store is a copy or a frame: counting it as an effect without
        # looking only keeps its loop from being skipped.
        look = not load and len(listed) == 1
        for i, r in enumerate(listed):
            where = f"GP32_RAM(ram, o_) + {4 * i}"
            if not load:
                store = f"store32({where}, {self.reg(r, pc, 12)})"
                code.append(f"fx += {store};" if look else f"{store};")
            elif r == 15:
                code.append(f"uint32_t v_ = load32({where});")
            else:
                code.append(f"{self.target(r)} = load32({where});")
        if not load and not look:
            code.append("++fx;")
        if wb and not (load and mask >> rn & 1):
            code.append(f"{self.target(rn)} = last_;")
        else:
            code.append("(void)last_;")
        if load and mask & 0x8000:
            code += self.indirect(pc, "v_")
        return code

    def instruction(self, ins, pc, leave):
        what = kind(ins)
        if what == "branch":
            code = []
            if ins & 0x01000000:
                code.append(f"{self.target(14)} = 0x{pc + 4:08x}u;")
            return code + self.jump(pc, (pc + 8 + 4 * signed(ins, 24)) & 0xFFFFFFFF)
        if what == "exchange":
            return self.indirect(pc, self.reg(ins & 15, pc), True)
        if what in ("single", "multiple"):
            return getattr(self, what)(ins, pc, leave)
        return getattr(self, what)(ins, pc)

    def block(self, first, words):
        length = len(words)
        out = self.out
        leave = f"pc = 0x{first:08x}u; goto out;"
        out.append(f"L_{first:08x}:")
        out.append(f"    if (__builtin_expect(room < {length}u, 0)) {{ {leave} }}")
        offset = first - BASE
        differs = []
        for i in range(0, length - 1, 2):
            both = words[i] | words[i + 1] << 32
            at = offset + 4 * i
            if at & 0x3FFF == 0x3FFC:  # two pages of a paged SDRAM
                differs += [f"(load32(GP32_RAM(ram, 0x{at + 4 * j:x}u)) ^ 0x{words[i + j]:x}u)"
                            for j in (0, 1)]
                continue
            differs.append(f"(load64(GP32_RAM(ram, 0x{at:x}u)) ^ UINT64_C(0x{both:x}))")
        if length & 1:
            differs.append(f"(load32(GP32_RAM(ram, 0x{offset + 4 * (length - 1):x}u)) ^ 0x{words[-1]:x}u)")
        out.append(f"    if (__builtin_expect(({' | '.join(differs)}) != 0, 0)) {{ {leave} }}")
        out.append(f"    room -= {length}u;")
        for i, word in enumerate(words):
            pc = first + 4 * i
            early = f"room += {length - i}u; pc = 0x{pc:08x}u; goto out;"
            code = self.instruction(word, pc, early)
            condition = word >> 28
            if condition < 14:
                out.append(f"    if ({CONDITIONS[condition]}) {{ /* {pc:08x} {word:08x} */")
            else:
                out.append(f"    {{ /* {pc:08x} {word:08x} */")
            out.extend("        " + line for line in code)
            out.append("    }")
        following = first + 4 * length
        # an unconditional transfer never gets here
        last = words[-1]
        if last >> 28 != 14 or not transfers(last):
            if following in self.blocks:
                out.append(f"    goto L_{following:08x};")
            else:
                out.append(f"    pc = 0x{following:08x}u; goto out;")

    def write(self, name, out):
        for first, words in sorted(self.blocks.items()):
            self.block(first, words)
        used = sorted(self.read | self.written)
        out.write(f"\nint {name}(Arm920t *cpu, Arm920tAot *run)\n{{\n"
                  "    if (__builtin_expect(!native_ready(cpu), 0)) return NATIVE_NONE;\n"
                  "    uint8_t *const ram = cpu->config.bus->ram;\n"
                  "    uint64_t most = arm920t_quotient(*run->budget, run->cost, run->inverse);\n"
                  "    if (!run->left) return NATIVE_NONE;\n"
                  "    if (most > run->left - 1) most = run->left - 1;\n"
                  "    const uint32_t start = (uint32_t)most;\n"
                  "    uint32_t room = start, pc = cpu->r[15], at = 0, fx = cpu->effects;\n"
                  "    uint32_t fn = cpu->cpsr, fz = !(cpu->cpsr & Z);\n"
                  "    bool fc = (cpu->cpsr & C) != 0, fv = (cpu->cpsr & V) != 0, thumb = false;\n")
        for r in used:
            out.write(f"    uint32_t r{r} = cpu->r[{r}];\n")
        out.write("    (void)ram; (void)fn; (void)fz; (void)fc; (void)fv;\n")
        out.write("dispatch: __attribute__((unused));\n    if (thumb) goto out;\n    switch (pc) {\n")
        for first in sorted(self.blocks):
            out.write(f"    case 0x{first:08x}u: goto L_{first:08x};\n")
        out.write("    default: goto out;\n    }\n")
        out.write("\n".join(self.out) + "\n")
        out.write("""last: __attribute__((unused));
    if (0) {
out:
        if (room == start) return NATIVE_NONE;
        at = 0;
    }
""")
        for r in sorted(self.written):
            out.write(f"    cpu->r[{r}] = r{r};\n")
        if self.sets_flags:
            out.write("    cpu->cpsr = (cpu->cpsr & ~(N | Z | C | V)) | (fn & N) | (fz ? 0 : Z) |\n"
                      "                (fc ? C : 0) | (fv ? V : 0);\n")
        out.write("""    if (thumb) cpu->cpsr |= T;
    cpu->r[15] = pc;
    cpu->effects = fx;
    unsigned done = start - room - (at != 0);
    account(run, done);
    if (!at) return NATIVE_MOVED;
    run->pc = at;
    return NATIVE_LAST;
}
""")


def transfers(ins):
    """True for an instruction which, when executed, never goes to the next."""
    what = kind(ins)
    if what in ("branch", "exchange"):
        return True
    if what == "data":
        return ins >> 12 & 15 == 15 and not 8 <= ins >> 21 & 15 <= 11
    if what == "single":
        return bool(ins & 0x00100000) and ins >> 12 & 15 == 15
    if what == "multiple":
        return bool(ins & 0x00100000 and ins & 0x8000)
    return False


def split(arm, hot, entries):
    """{first address: opcodes} of the native blocks: translated hot code, cut at
    every address known to be entered and after every possible transfer."""
    found, current = {}, []

    def close():
        if current:
            found[current[0]] = [arm[a] for a in current]
        current.clear()

    for address in sorted(arm):
        if (hot is not None and ("arm", address) not in hot) or kind(arm[address]) is None \
                or not 0 <= address - BASE < RAM:
            close()
            continue
        if current and (address != current[-1] + 4 or address in entries
                        or len(current) == BLOCK or address % REGION == 0):
            close()
        current.append(address)
        if transfers(arm[address]):
            close()
    close()
    return found


def generate(arm, hot, entries, stem, index, header, hosted):
    """Write the region functions to the parts numbered from `index`.  Returns
    ({block address: function name}, the number of the next part)."""
    # returns from a call are entered too
    called = {a + 4 for a, w in arm.items() if w >> 28 != 15 and w & 0x0F000000 == 0x0B000000}
    # as is what follows an instruction left to the precise steps
    resumed = {a + 4 for a, w in arm.items() if kind(w) is None}
    # A routine the host runs is no part of any block.
    blocks = split({a: w for a, w in arm.items() if a not in hosted}, hot,
                   entries | called | resumed)
    regions = {}
    for first, words in blocks.items():
        regions.setdefault(first // REGION, {})[first] = words
    parts, size = [[]], 0
    for number, held in sorted(regions.items()):
        if size >= PART:
            parts.append([])
            size = 0
        parts[-1].append((number, held))
        size += sum(map(len, held.values()))
    names = {}
    for part in parts:
        if not part:
            continue
        with open(f"{stem}.part{index:03d}.c", "w", encoding="ascii") as out:
            out.write(header(index) + SUPPORT)
            for number, held in part:
                name = f"native_{number * REGION:08x}"
                Region(held, arm).write(name, out)
                names.update(dict.fromkeys(held, name))
        index += 1
    if hosted:
        with open(f"{stem}.part{index:03d}.c", "w", encoding="ascii") as out:
            out.write(header(index) + SUPPORT + ROUTINE_SUPPORT)
            for _, (_, source) in sorted(hosted.items()):
                out.write(source)
        index += 1
    return names, index
