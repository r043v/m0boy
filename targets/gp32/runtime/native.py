"""The native half of recompile.py --hybrid.

Straight ARM code becomes plain C: the guest registers and flags are local
variables, SDRAM is read and written in place, and a whole block is accounted at
once.  Everything else belongs to the precise machine, which runs it with the
recompiled steps: a block only starts when no peripheral event falls inside it,
and an instruction reaching a device, changing the mode or doing anything not
translated here is left, with the state written back, to those steps.
"""

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
           !cpu->config.access && !bus->hooks.access && bus->ram &&
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


def generate(arm, hot, entries, stem, index, header):
    """Write the region functions to the parts numbered from `index`.  Returns
    ({block address: function name}, the number of the next part)."""
    # returns from a call are entered too
    called = {a + 4 for a, w in arm.items() if w >> 28 != 15 and w & 0x0F000000 == 0x0B000000}
    # as is what follows an instruction left to the precise steps
    resumed = {a + 4 for a, w in arm.items() if kind(w) is None}
    blocks = split(arm, hot, entries | called | resumed)
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
    return names, index
