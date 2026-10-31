/* HuC6280 -> C for the PCE runtime.
 *
 * After the NES emitter's plan (regions, dispatcher, interpreter fallback),
 * with the PC Engine's bank windows folded in: an instruction instance is
 * an address under one MPR context (the eight page registers, packed).  The
 * walk starts at the vectors under the reset context (all MPR at $00, the
 * cartridge's page 0) and follows LDA #imm / TAM #mask pairs to the
 * contexts the boot really maps; coverage entries recorded with their own
 * context seed more.  A context change the walk cannot prove (TAM with an
 * unknown A, a bank table) leaves that path to the interpreter.
 *
 * Every exit returns to the dispatcher (Thumb-1 GCC never tail-calls): a
 * known target in another region leaves its entry as a hint the lookup
 * checks first; RTS, RTI and JMP through a pointer leave pc to be looked
 * up.  An entry is only entered when the machine's MPR bytes are still the
 * context's — the dispatcher checks them, so re-mapped windows fall back
 * to the interpreter instead of running the wrong bytes.
 *
 * The semantics are the interpreter's (huc6280.c), cycle for cycle: same
 * counts (mednafen's, the decimal and T-mode extras included), the same
 * bus accesses in the same order, ROM reads folded only where the context
 * makes them constants.  Precise mode checks the budget after every
 * instruction and leaves the region after any that may touch the I/O bus
 * or the interrupt controller, so the dispatcher samples the IRQ line
 * exactly where the interpreter does; the fast mode checks it only at
 * labels and at most every `check_cycles` cycles. */
#include "m0boy/pce_emitter.h"
#include "m0boy/sweep.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <sstream>

namespace m0boy::pce {
namespace {

enum class Mode { imp, acc, imm, zp, zpx, zpy, abs, absx, absy, ind, izx, izy, rel,
                  tst_zp, tst_zpx, tst_abs, tst_absx, block };

enum class Op {
    none, lda, ldx, ldy, sta, stx, sty, stz, and_, ora, eor, adc, sbc, cmp, cpx, cpy, bit,
    asl, lsr, rol, ror, inc, dec, ina, dea, inx, iny, dex, dey,
    tsb, trb, tst, rmb, smb, tam, tma, st0, st1, st2,
    bra, bpl, bmi, bvc, bvs, bcc, bcs, bne, beq, bbr, bbs,
    jmp, jsr, bsr, rts, rti,
    pha, php, phx, phy, pla, plp, plx, ply,
    clc, sec, cli, sei, clv, cld, sed, set, nop,
    tax, tay, txa, tya, tsx, txs, cla, clx, cly, sxy, sax, say, csl, csh,
    tii, tdd, tin, tia, tai,
};

enum class Shape {
    none, load, store, logic, arith, cmp, bit, shift, incdec, tsbtrb, tst, rmbsmb,
    tam, tma, stv, block, imp, flow,
};

struct OpInfo {
    Op op = Op::none;
    Shape shape = Shape::none;
    Mode mode = Mode::imp;
    uint8_t cycles = 0;
    uint8_t len = 1;
    bool preserves_a = false; /* the walk's TAM tracking: A provably unchanged */
};

const std::vector<OpInfo>& table() {
    static std::vector<OpInfo> t = [] {
        std::vector<OpInfo> v(256);
        auto set = [&](unsigned code, Op op, Shape shape, Mode mode, unsigned cycles, unsigned len,
                       bool keeps_a = false) {
            v[code] = { op, shape, mode, uint8_t(cycles), uint8_t(len), keeps_a };
        };
        /* the eight-mode ALU groups of the 6280: imm zp zpx abs absx absy (zp) (zp,X) (zp),Y */
        auto group = [&](Op op, Shape shape, const unsigned c[9], bool keeps_a = false) {
            static const unsigned code[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 }; /* unused */
            (void)code;
            set(c[0], op, shape, Mode::imm, 2, 2, keeps_a);
            set(c[1], op, shape, Mode::zp, 4, 2, keeps_a);
            set(c[2], op, shape, Mode::zpx, 4, 2, keeps_a);
            set(c[3], op, shape, Mode::abs, 5, 3, keeps_a);
            set(c[4], op, shape, Mode::absx, 5, 3, keeps_a);
            set(c[5], op, shape, Mode::absy, 5, 3, keeps_a);
            set(c[6], op, shape, Mode::ind, 7, 2, keeps_a);
            set(c[7], op, shape, Mode::izx, 7, 2, keeps_a);
            set(c[8], op, shape, Mode::izy, 7, 2, keeps_a);
        };
        const unsigned lda[9] = { 0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xB2, 0xA1, 0xB1 };
        const unsigned sta[9] = { 0, 0x85, 0x95, 0x8D, 0x9D, 0x99, 0x92, 0x81, 0x91 };
        const unsigned and_[9] = { 0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x32, 0x21, 0x31 };
        const unsigned ora[9] = { 0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x12, 0x01, 0x11 };
        const unsigned eor[9] = { 0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x52, 0x41, 0x51 };
        const unsigned adc[9] = { 0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x72, 0x61, 0x71 };
        const unsigned sbc[9] = { 0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xF2, 0xE1, 0xF1 };
        const unsigned cmp_[9] = { 0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xD2, 0xC1, 0xD1 };
        group(Op::lda, Shape::load, lda);
        group(Op::sta, Shape::store, sta);
        group(Op::and_, Shape::logic, and_);
        group(Op::ora, Shape::logic, ora);
        group(Op::eor, Shape::logic, eor);
        group(Op::adc, Shape::arith, adc);
        group(Op::sbc, Shape::arith, sbc);
        group(Op::cmp, Shape::cmp, cmp_, true);
        set(0xA2, Op::ldx, Shape::load, Mode::imm, 2, 2, true);
        set(0xA6, Op::ldx, Shape::load, Mode::zp, 4, 2, true);
        set(0xB6, Op::ldx, Shape::load, Mode::zpy, 4, 2, true);
        set(0xAE, Op::ldx, Shape::load, Mode::abs, 5, 3, true);
        set(0xBE, Op::ldx, Shape::load, Mode::absy, 5, 3, true);
        set(0xA0, Op::ldy, Shape::load, Mode::imm, 2, 2, true);
        set(0xA4, Op::ldy, Shape::load, Mode::zp, 4, 2, true);
        set(0xB4, Op::ldy, Shape::load, Mode::zpx, 4, 2, true);
        set(0xAC, Op::ldy, Shape::load, Mode::abs, 5, 3, true);
        set(0xBC, Op::ldy, Shape::load, Mode::absx, 5, 3, true);
        set(0x86, Op::stx, Shape::store, Mode::zp, 4, 2);
        set(0x96, Op::stx, Shape::store, Mode::zpy, 4, 2);
        set(0x8E, Op::stx, Shape::store, Mode::abs, 5, 3);
        set(0x84, Op::sty, Shape::store, Mode::zp, 4, 2);
        set(0x94, Op::sty, Shape::store, Mode::zpx, 4, 2);
        set(0x8C, Op::sty, Shape::store, Mode::abs, 5, 3);
        set(0x64, Op::stz, Shape::store, Mode::zp, 4, 2);
        set(0x74, Op::stz, Shape::store, Mode::zpx, 4, 2);
        set(0x9C, Op::stz, Shape::store, Mode::abs, 5, 3);
        set(0x9E, Op::stz, Shape::store, Mode::absx, 5, 3);
        set(0x89, Op::bit, Shape::bit, Mode::imm, 2, 2, true);
        set(0x24, Op::bit, Shape::bit, Mode::zp, 4, 2, true);
        set(0x34, Op::bit, Shape::bit, Mode::zpx, 4, 2, true);
        set(0x2C, Op::bit, Shape::bit, Mode::abs, 5, 3, true);
        set(0x3C, Op::bit, Shape::bit, Mode::absx, 5, 3, true);
        auto shift = [&](Op op, unsigned acc, unsigned zp, unsigned zpx, unsigned ab, unsigned abx) {
            if (acc) set(acc, op, Shape::shift, Mode::acc, 2, 1);
            set(zp, op, Shape::shift, Mode::zp, 6, 2);
            set(zpx, op, Shape::shift, Mode::zpx, 6, 2);
            set(ab, op, Shape::shift, Mode::abs, 7, 3);
            set(abx, op, Shape::shift, Mode::absx, 7, 3);
        };
        shift(Op::asl, 0x0A, 0x06, 0x16, 0x0E, 0x1E);
        shift(Op::lsr, 0x4A, 0x46, 0x56, 0x4E, 0x5E);
        shift(Op::rol, 0x2A, 0x26, 0x36, 0x2E, 0x3E);
        shift(Op::ror, 0x6A, 0x66, 0x76, 0x6E, 0x7E);
        auto rmw = [&](Op op, unsigned zp, unsigned zpx, unsigned ab, unsigned abx) {
            set(zp, op, Shape::incdec, Mode::zp, 6, 2);
            set(zpx, op, Shape::incdec, Mode::zpx, 6, 2);
            set(ab, op, Shape::incdec, Mode::abs, 7, 3);
            set(abx, op, Shape::incdec, Mode::absx, 7, 3);
        };
        rmw(Op::inc, 0xE6, 0xF6, 0xEE, 0xFE);
        rmw(Op::dec, 0xC6, 0xD6, 0xCE, 0xDE);
        set(0x1A, Op::ina, Shape::imp, Mode::imp, 2, 1);
        set(0x3A, Op::dea, Shape::imp, Mode::imp, 2, 1);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0xE8, Op::inx}, {0xC8, Op::iny}, {0xCA, Op::dex}, {0x88, Op::dey}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, true);
        /* RMB/SMB: $07.. $77 reset, $87.. $F7 set, bit in the high nibble */
        for (unsigned i = 0; i < 8; i++) {
            set(0x07 + i * 0x10, Op::rmb, Shape::rmbsmb, Mode::zp, 7, 2, true);
            set(0x87 + i * 0x10, Op::smb, Shape::rmbsmb, Mode::zp, 7, 2, true);
        }
        set(0x04, Op::tsb, Shape::tsbtrb, Mode::zp, 6, 2, true);
        set(0x0C, Op::tsb, Shape::tsbtrb, Mode::abs, 7, 3, true);
        set(0x14, Op::trb, Shape::tsbtrb, Mode::zp, 6, 2, true);
        set(0x1C, Op::trb, Shape::tsbtrb, Mode::abs, 7, 3, true);
        /* TST #imm,zp(X) / #imm,abs(X): the memory operand follows the immediate */
        set(0x83, Op::tst, Shape::tst, Mode::tst_zp, 7, 3, true);
        set(0xA3, Op::tst, Shape::tst, Mode::tst_zpx, 7, 3, true);
        set(0x93, Op::tst, Shape::tst, Mode::tst_abs, 8, 4, true);
        set(0xB3, Op::tst, Shape::tst, Mode::tst_absx, 8, 4, true);
        /* branches: taken +2 at emission */
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x80, Op::bra}, {0x10, Op::bpl}, {0x30, Op::bmi}, {0x50, Op::bvc},
                 {0x70, Op::bvs}, {0x90, Op::bcc}, {0xB0, Op::bcs}, {0xD0, Op::bne}, {0xF0, Op::beq}})
            set(code, op, Shape::flow, Mode::rel, 2, 2, true);
        /* BBR/BBS: zp byte, then relative; taken +2 at emission */
        for (unsigned i = 0; i < 8; i++) {
            set(0x0F + i * 0x10, Op::bbr, Shape::flow, Mode::rel, 6, 3, true);
            set(0x8F + i * 0x10, Op::bbs, Shape::flow, Mode::rel, 6, 3, true);
        }
        set(0x4C, Op::jmp, Shape::flow, Mode::abs, 4, 3, true);
        set(0x6C, Op::jmp, Shape::flow, Mode::ind, 7, 3, true);
        set(0x7C, Op::jmp, Shape::flow, Mode::absx, 7, 3, true);
        set(0x20, Op::jsr, Shape::flow, Mode::abs, 7, 3, true);
        set(0x44, Op::bsr, Shape::flow, Mode::rel, 8, 2, true);
        set(0x60, Op::rts, Shape::flow, Mode::imp, 7, 1, true);
        set(0x40, Op::rti, Shape::flow, Mode::imp, 7, 1, true);
        set(0x48, Op::pha, Shape::imp, Mode::imp, 3, 1, true);
        set(0x08, Op::php, Shape::imp, Mode::imp, 3, 1, true);
        set(0xDA, Op::phx, Shape::imp, Mode::imp, 3, 1, true);
        set(0x5A, Op::phy, Shape::imp, Mode::imp, 3, 1, true);
        set(0x68, Op::pla, Shape::imp, Mode::imp, 4, 1);
        set(0x28, Op::plp, Shape::imp, Mode::imp, 4, 1, true);
        set(0xFA, Op::plx, Shape::imp, Mode::imp, 4, 1);
        set(0x7A, Op::ply, Shape::imp, Mode::imp, 4, 1);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x18, Op::clc}, {0x38, Op::sec}, {0x58, Op::cli}, {0x78, Op::sei},
                 {0xB8, Op::clv}, {0xD8, Op::cld}, {0xF8, Op::sed}, {0xF4, Op::set},
                 {0xEA, Op::nop}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, true);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0xAA, Op::tax}, {0xA8, Op::tay}, {0xBA, Op::tsx}, {0x9A, Op::txs}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, true);
        set(0x8A, Op::txa, Shape::imp, Mode::imp, 2, 1);
        set(0x98, Op::tya, Shape::imp, Mode::imp, 2, 1);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x62, Op::cla}, {0x82, Op::clx}, {0xC2, Op::cly}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, true);
        set(0x02, Op::sxy, Shape::imp, Mode::imp, 3, 1, true);
        set(0x22, Op::sax, Shape::imp, Mode::imp, 3, 1);
        set(0x42, Op::say, Shape::imp, Mode::imp, 3, 1);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x54, Op::csl}, {0xD4, Op::csh}})
            set(code, op, Shape::imp, Mode::imp, 3, 1, true);
        set(0x53, Op::tam, Shape::tam, Mode::imm, 5, 2, true);
        set(0x43, Op::tma, Shape::tma, Mode::imm, 4, 2);
        set(0x03, Op::st0, Shape::stv, Mode::imp, 4, 1, true);
        set(0x13, Op::st1, Shape::stv, Mode::imp, 4, 1, true);
        set(0x23, Op::st2, Shape::stv, Mode::imp, 4, 1, true);
        /* the block moves: seven bytes, 17 cycles of fixed part */
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x73, Op::tii}, {0xC3, Op::tdd}, {0xD3, Op::tin}, {0xE3, Op::tia}, {0xF3, Op::tai}})
            set(code, op, Shape::block, Mode::block, 17, 7, true);
        return v;
    }();
    return t;
}

bool is_branch(Op op) { return op >= Op::bra && op <= Op::beq; }

/* flag bits of the liveness sets: N|Z|C|V — the T/I/D bits stay out of
 * the model (their updates always run) */
enum : uint8_t { FNZ_N = 1, FNZ_Z = 2, FNZ_C = 4, FNZ_V = 8, FNZ_ALL = 0xF };

/* the flags an instruction writes */
uint8_t flag_writes(Op op) {
    switch (op) {
    case Op::lda: case Op::ldx: case Op::ldy: case Op::ora: case Op::and_: case Op::eor:
    case Op::ina: case Op::dea: case Op::inx: case Op::iny: case Op::dex: case Op::dey:
    case Op::tax: case Op::tay: case Op::txa: case Op::tya: case Op::tsx: case Op::pla:
    case Op::plx: case Op::ply:
        return FNZ_N | FNZ_Z;
    case Op::adc: case Op::sbc:
        return FNZ_ALL;
    case Op::cmp: case Op::cpx: case Op::cpy:
    case Op::asl: case Op::lsr: case Op::rol: case Op::ror:
        return FNZ_N | FNZ_Z | FNZ_C;
    case Op::bit: case Op::tst: case Op::tsb: case Op::trb:
        return FNZ_N | FNZ_Z | FNZ_V;
    case Op::clc: case Op::sec:
        return FNZ_C;
    case Op::clv:
        return FNZ_V;
    case Op::plp: case Op::rti:
        return FNZ_ALL;
    default:
        return 0;
    }
}

/* the flags an instruction reads */
uint8_t flag_reads(Op op) {
    switch (op) {
    case Op::bpl: case Op::bmi: return FNZ_N;
    case Op::bvc: case Op::bvs: return FNZ_V;
    case Op::bcc: case Op::bcs: return FNZ_C;
    case Op::bne: case Op::beq: return FNZ_Z;
    case Op::adc: case Op::sbc: case Op::rol: case Op::ror: return FNZ_C;
    case Op::php: return FNZ_ALL;   /* PCE_PUSH joins P together */
    default:
        return 0;
    }
}

std::string hex(unsigned v, int digits = 4) {
    char b[16];
    snprintf(b, sizeof b, "0x%0*X", digits, v);
    return b;
}

std::string hex64(uint64_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%016llXULL", (unsigned long long)v);
    return b;
}

/* one instruction instance: an address under one MPR context */
struct Key {
    uint16_t pc;
    uint64_t ctx;
    bool operator<(const Key& o) const { return pc != o.pc ? pc < o.pc : ctx < o.ctx; }
};

struct Ins {
    uint16_t addr = 0;
    uint64_t ctx = 0;
    uint8_t code = 0;
    uint8_t o[6] = {0};
    OpInfo info;
    unsigned len = 1;
    uint16_t next = 0;
    uint16_t target = 0;   /* branch, JMP abs, JSR, BSR */
    bool has_target = false;
    bool falls = true;
    uint32_t hits = 0;
    bool dynamic = false;
    bool tracked_a = false; /* the walk knew A here: a TAM's bank is provable */
    uint8_t live_out = 0xF; /* flags read after this instruction: N|Z|C|V */
    bool label = false;    /* an entry: switch case, lookup key, budget check (fast) */
    int region = -1;
    unsigned local = 0;
    unsigned global = 0;
};

/* the walk's register knowledge: only LDA #imm kept, the rest forgotten */
struct Seed {
    uint16_t pc;
    uint64_t ctx;
    bool a_known;
    uint8_t a_val;
};

class Emitter {
public:
    Emitter(const std::vector<uint8_t>& rom, const EmitOptions& o) : rom_(rom), opt_(o) {}
    std::string run(const std::vector<CoverageEntry>& coverage, EmitStats& stats);

private:
    const std::vector<uint8_t>& rom_;
    EmitOptions opt_;
    std::map<Key, Ins> ins_;
    std::vector<std::vector<Key>> regions_;

    unsigned pages() const { return unsigned((rom_.size() + 0x1FFFu) / 0x2000u); }
    /* the byte at `at` under `ctx`, only where the window is cartridge ROM */
    std::optional<uint8_t> ctx_byte(uint64_t ctx, uint16_t at) const {
        const unsigned v = unsigned((ctx >> (8 * (at >> 13))) & 0xFFu);
        const uint32_t off = uint32_t(v) * 0x2000u + (at & 0x1FFFu);
        if (v >= pages() || off >= rom_.size()) return std::nullopt;
        return rom_[off];
    }
    /* the window of `at` under `ctx`: 0 ROM, 1 WRAM ($F8-$FB above the ROM's
     * pages, as huc6280_map decodes them), 2 the hardware page or open bus */
    enum class Win { rom, wram, bus };
    Win win(uint64_t ctx, uint16_t at) const {
        const unsigned v = unsigned((ctx >> (8 * (at >> 13))) & 0xFFu);
        if (v < pages()) return Win::rom;
        if (v >= 0xF8u && v <= 0xFBu) return Win::wram;
        return Win::bus;
    }
    bool decode(uint16_t at, uint64_t ctx, Ins& in) const;
    void select(const std::vector<CoverageEntry>& coverage, EmitStats& stats);
    void plan();
    void emit_region(std::ostream& o, int r);
    bool emit_ins(std::ostream& o, const Ins& in, bool t_zero, bool tail);
    std::string go(const Ins& from, uint16_t to) const;
    int hint_of(const Ins& from, uint16_t to) const;
    std::string operand(const Ins& in, std::string& pre) const;
    std::string ea(const Ins& in, std::string& pre) const;
    bool may_bus(const Ins& in) const;
};

bool Emitter::decode(uint16_t at, uint64_t ctx, Ins& in) const {
    const auto op_b = ctx_byte(ctx, at);
    if (!op_b) return false;
    in = Ins{};
    in.addr = at;
    in.ctx = ctx;
    in.code = *op_b;
    in.info = table()[in.code];
    if (in.info.shape == Shape::none) return false;
    in.len = in.info.len;
    if (unsigned(at) + in.len - 1 > 0xFFFFu) return false;
    for (unsigned i = 1; i < in.len; i++) {
        const auto b = ctx_byte(ctx, uint16_t(at + i));
        if (!b) return false;
        in.o[i - 1] = *b;
    }
    in.next = uint16_t(at + in.len);
    const unsigned w = in.o[0] | (in.o[1] << 8);
    const Op op = in.info.op;
    if (is_branch(op)) {
        in.target = uint16_t(in.next + int8_t(in.o[0]));
        in.has_target = true;
    } else if (op == Op::bbr || op == Op::bbs) {
        in.target = uint16_t(in.next + int8_t(in.o[1]));
        in.has_target = true;
    } else if (op == Op::jmp && in.info.mode == Mode::abs) {
        in.target = uint16_t(w);
        in.has_target = true;
        in.falls = false;
    } else if (op == Op::jmp) {
        in.falls = false; /* through a pointer: the dispatcher looks it up */
    } else if (op == Op::jsr) {
        in.target = uint16_t(w);
        in.has_target = true; /* falls: the return site */
    } else if (op == Op::bsr) {
        in.target = uint16_t(in.next + int8_t(in.o[0]));
        in.has_target = true;
    } else if (op == Op::rts || op == Op::rti) {
        in.falls = false;
    }
    return true;
}

void Emitter::select(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    /* every reachable instance, the played ones first: the coverage's
     * contexted addresses, then the vectors and what they reach */
    std::vector<CoverageEntry> hot(coverage);
    std::stable_sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.hits > b.hits; });
    std::deque<Seed> work;
    std::vector<Key> order;
    auto visit = [&](uint16_t at, uint64_t ctx, bool a_known, uint8_t a_val,
                     std::deque<Seed>& sink, m0::Sweep* budget) {
        const Key key{at, ctx};
        if (ins_.count(key)) return;
        Ins in;
        if (!decode(at, ctx, in)) return;
        if (budget && !budget->take()) return;
        ins_[key] = in;
        order.push_back(key);
        sink.push_back({at, ctx, a_known, a_val});
    };
    for (const auto& e : hot)
        for (uint64_t ctx : e.contexts) visit(e.pc, ctx, false, 0, work, nullptr);
    const uint64_t reset_ctx = 0; /* every MPR at $00: the cartridge's page 0 */
    for (uint16_t v : { uint16_t(0xFFFE), uint16_t(0xFFFC), uint16_t(0xFFFA), uint16_t(0xFFF8), uint16_t(0xFFF6) }) {
        const auto lo = ctx_byte(reset_ctx, v);
        const auto hi = ctx_byte(reset_ctx, uint16_t(v + 1));
        if (lo && hi) visit(uint16_t(*lo | *hi << 8), reset_ctx, false, 0, work, nullptr);
    }
    const size_t covered = order.size();
    /* the sweep's own budget: a code budget's leftovers, else its modest
     * cap — a byte $20/$4C is mostly data, an unbounded sweep compiles
     * the whole card under every context */
    m0::Sweep swept_budget(m0::sweep_room(covered, opt_.max_instructions));
    std::deque<Seed> swept;
    if (opt_.sweep)
        /* the sweep: every window's mapping of every page, a JSR or a JMP
         * abs whose target decodes seeds the walk — code the reachability
         * walk cannot prove (jump-table handlers, callers behind a bank
         * table) compiled the same way.  Nothing lands there until the
         * game does: an entry is entered only under its own MPR bytes,
         * and the compiled semantics stay the interpreter's.  Swept
         * entries rank after the played ones (order is visit order) and
         * take what the code budget has left, the walk itself never
         * being capped (m0/sweep.h). */
        for (unsigned w = 0; w < 8; w++)
            for (unsigned page = 0; page < pages(); page++) {
                const uint64_t ctx = uint64_t(page) << (8 * w);
                for (unsigned at = w << 13; at < (w << 13) + 0x2000u; at++) {
                    const auto op_b = ctx_byte(ctx, uint16_t(at));
                    if (!op_b || (*op_b != 0x20 && *op_b != 0x4C)) continue;
                    if (at + 2 > 0xFFFFu) break;
                    const auto lo = ctx_byte(ctx, uint16_t(at + 1));
                    const auto hi = ctx_byte(ctx, uint16_t(at + 2));
                    if (lo && hi) visit(uint16_t(*lo | *hi << 8), ctx, false, 0, swept, &swept_budget);
                }
            }
    auto expand = [&](std::deque<Seed>& queue, m0::Sweep* budget) {
        while (!queue.empty()) {
            const Seed s = queue.front();
            queue.pop_front();
            Ins& in = ins_[Key{s.pc, s.ctx}];
            in.tracked_a = s.a_known;
            uint64_t nctx = s.ctx;
            if (in.info.op == Op::tam && s.a_known) {
                for (unsigned i = 0; i < 8; i++)
                    if (in.o[0] & (1u << i)) nctx |= uint64_t(s.a_val) << (8 * i);
            }
            bool nak;
            uint8_t nav = s.a_val;
            if (in.info.op == Op::lda && in.info.mode == Mode::imm) {
                nak = true;
                nav = in.o[0];
            } else {
                nak = in.info.preserves_a && s.a_known;
            }
            if (in.falls) visit(in.next, nctx, nak, nav, queue, budget);
            if (in.has_target) visit(in.target, nctx, nak, nav, queue, budget);
        }
    };
    expand(work, nullptr);
    expand(swept, &swept_budget);
    const size_t keep = opt_.max_instructions ? std::min(order.size(), opt_.max_instructions) : order.size();
    for (size_t i = keep; i < order.size(); i++) ins_.erase(order[i]);
    /* the profile: exact contexts attach to their instance, context-less
     * addresses rank and label every instance found there */
    std::map<Key, const CoverageEntry*> exact;
    std::map<uint16_t, std::vector<const CoverageEntry*>> loose;
    for (const auto& e : hot) {
        for (uint64_t ctx : e.contexts) exact[{e.pc, ctx}] = &e;
        if (e.contexts.empty()) loose[e.pc].push_back(&e);
    }
    for (auto& [key, in] : ins_) {
        uint32_t hits = 0;
        bool dyn = false;
        if (auto p = exact.find(key); p != exact.end()) {
            hits = p->second->hits;
            dyn = p->second->dynamic;
        }
        for (const CoverageEntry* e : loose[key.pc]) {
            hits = std::max(hits, e->hits);
            dyn = dyn || e->dynamic;
        }
        in.hits = hits;
        in.dynamic = dyn;
    }
    stats.covered = std::min(covered, keep);
    stats.compiled = ins_.size();
}

void Emitter::plan() {
    /* entries: what a jump from elsewhere, the dispatcher or a return can
     * land on.  Precise mode: every instruction (the slice may end after
     * any of them). */
    auto mark = [&](uint16_t at, uint64_t ctx) {
        if (auto p = ins_.find(Key{at, ctx}); p != ins_.end()) p->second.label = true;
    };
    const uint64_t reset_ctx = 0;
    for (uint16_t v : { uint16_t(0xFFFE), uint16_t(0xFFFC), uint16_t(0xFFFA), uint16_t(0xFFF8), uint16_t(0xFFF6) }) {
        const auto lo = ctx_byte(reset_ctx, v);
        const auto hi = ctx_byte(reset_ctx, uint16_t(v + 1));
        if (lo && hi) mark(uint16_t(*lo | *hi << 8), reset_ctx);
    }
    for (auto& [key, in] : ins_) {
        if (opt_.precise || in.dynamic) in.label = true;
        if (in.has_target) mark(in.target, key.ctx);
        const Op op = in.info.op;
        /* the instruction after an exit resumes there: a return site, an
         * IRQ window, a mapping switch, the end of a block move or a
         * register write to the VDC */
        if (op == Op::jsr || op == Op::bsr || op == Op::cli || op == Op::plp ||
            op == Op::tam || op == Op::st0 || op == Op::st1 || op == Op::st2 ||
            in.info.shape == Shape::block)
            mark(in.next, key.ctx);
    }
    /* regions: runs of neighbouring instructions under one context — a
     * TAM whose bank the walk could not prove ends its run (its exit
     * hands the new mapping back to the dispatcher's check).  The runs
     * are built context first, then pc: the map's (pc, ctx) order
     * interleaves the instances of one span under its several contexts
     * and shatters the regions down to ~1 instruction, one function
     * call per executed instruction.  A run never spans two contexts:
     * the dispatcher only enters an entry whose MPR bytes still match,
     * and the joins below keep requiring back.ctx == key.ctx. */
    const size_t cap = 256;
    std::vector<Key> order;
    order.reserve(ins_.size());
    for (const auto& [key, in] : ins_) order.push_back(key);
    std::sort(order.begin(), order.end(), [](const Key& a, const Key& b) {
        return a.ctx != b.ctx ? a.ctx < b.ctx : a.pc < b.pc;
    });
    for (const Key& key : order) {
        Ins& in = ins_.at(key);
        const bool joins = !regions_.empty() && regions_.back().size() < cap &&
                           [&] {
                               const Ins& back = ins_.at(regions_.back().back());
                               return back.next == key.pc && back.ctx == key.ctx &&
                                      !(back.info.op == Op::tam && !back.tracked_a);
                           }();
        if (!joins) {
            regions_.emplace_back();
            in.label = true;
        }
        in.region = int(regions_.size() - 1);
        regions_.back().push_back(key);
    }
    if (!opt_.precise)
        for (const auto& r : regions_) {
            unsigned run = 0;
            for (const Key& k : r) {
                Ins& in = ins_.at(k);
                if (in.label) run = 0;
                if (run > opt_.check_cycles) { in.label = true; run = 0; }
                run += in.info.cycles + 2;
            }
        }
    for (const auto& r : regions_) {
        unsigned local = 0;
        for (const Key& k : r) {
            Ins& in = ins_.at(k);
            if (in.label) in.local = local++;
        }
    }
    unsigned global = 0;
    for (auto& [key, in] : ins_)
        if (in.label) in.global = global++;
    /* the fast mode can prune a dead flag write: walk every region
     * backwards, keeping all flags live at labels (the budget check may
     * leave) and at every exit; precise mode keeps the default 0xF */
    if (!opt_.precise)
        for (const auto& r : regions_) {
            uint8_t live = FNZ_ALL;
            for (size_t i = r.size(); i-- > 0;) {
                Ins& in = ins_.at(r[i]);
                const bool all_out = in.has_target || !in.falls || i + 1 == r.size() ||
                                     ins_.at(r[i + 1]).label;
                in.live_out = all_out ? FNZ_ALL : live;
                live = (uint8_t)((in.live_out & ~flag_writes(in.info.op)) | flag_reads(in.info.op));
                if (in.label) live = FNZ_ALL;
            }
        }
}

/* where control goes to reach `to` in the from-instance's context.  Every
 * exit shares one epilogue (`out`), which stores `ni` into the dispatcher's
 * hint: a known entry lands as a hint, anything else leaves -1. */
std::string Emitter::go(const Ins& from, uint16_t to) const {
    auto p = ins_.find(Key{to, from.ctx});
    if (p == ins_.end() || !p->second.label) return "{ pc_ = " + hex(to) + "; goto out; }";
    if (p->second.region == from.region) {
        char b[96];
        /* precise mode checks the budget after every instruction, taken
         * branches and jumps within the region included (the labels
         * check only in the fast mode) */
        if (opt_.precise)
            snprintf(b, sizeof b, "if (budget <= 0) { pc_ = 0x%04X; ni = %u; goto out; } goto L_%04X;",
                     to, p->second.global, to);
        else
            snprintf(b, sizeof b, "goto L_%04X;", to);
        return b;
    }
    return "{ pc_ = " + hex(to) + "; ni = " + std::to_string(p->second.global) + "; goto out; }";
}

/* the entry's global index when `to` is one, else -1: a budget end with a
 * hint saves the dispatcher's hash lookup on the next slice */
int Emitter::hint_of(const Ins& from, uint16_t to) const {
    auto p = ins_.find(Key{to, from.ctx});
    return p != ins_.end() && p->second.label ? int(p->second.global) : -1;
}

/* the operand's address as C, computed once into `pre`'s locals when the
 * addressing goes through the zero page */
std::string Emitter::ea(const Ins& in, std::string& pre) const {
    switch (in.info.mode) {
    case Mode::zp: return "(uint16_t)(0x2000u | " + hex(in.o[0], 2) + "u)";
    case Mode::zpx: return "PCE_ZP_AT((uint8_t)(" + hex(in.o[0], 2) + "u + x_))";
    case Mode::zpy: return "PCE_ZP_AT((uint8_t)(" + hex(in.o[0], 2) + "u + y_))";
    case Mode::abs: case Mode::tst_abs: {
        const unsigned w = in.info.mode == Mode::abs ? in.o[0] | (in.o[1] << 8)
                                                     : in.o[1] | (in.o[2] << 8);
        return hex(w);
    }
    case Mode::absx: case Mode::tst_absx: {
        const unsigned w = in.info.mode == Mode::absx ? in.o[0] | (in.o[1] << 8)
                                                      : in.o[1] | (in.o[2] << 8);
        return "(uint16_t)(" + hex(w) + "u + x_)";
    }
    case Mode::absy: {
        const unsigned w = in.o[0] | (in.o[1] << 8);
        return "(uint16_t)(" + hex(w) + "u + y_)";
    }
    case Mode::ind: {
        pre += "const unsigned o_ = " + hex(in.o[0], 2) + "u; ";
        pre += "const unsigned lo_ = PCE_RD(PCE_ZP_AT((uint8_t)o_)); ";
        pre += "const unsigned hi_ = PCE_RD(PCE_ZP_AT((uint8_t)(o_ + 1u))); ";
        pre += "const uint16_t t_ = (uint16_t)(lo_ | hi_ << 8); ";
        return "t_";
    }
    case Mode::izx: {
        pre += "const unsigned o_ = (uint8_t)(" + hex(in.o[0], 2) + "u + x_); ";
        pre += "const unsigned lo_ = PCE_RD(PCE_ZP_AT((uint8_t)o_)); ";
        pre += "const unsigned hi_ = PCE_RD(PCE_ZP_AT((uint8_t)(o_ + 1u))); ";
        pre += "const uint16_t t_ = (uint16_t)(lo_ | hi_ << 8); ";
        return "t_";
    }
    case Mode::izy: {
        pre += "const unsigned o_ = " + hex(in.o[0], 2) + "u; ";
        pre += "const unsigned lo_ = PCE_RD(PCE_ZP_AT((uint8_t)o_)); ";
        pre += "const unsigned hi_ = PCE_RD(PCE_ZP_AT((uint8_t)(o_ + 1u))); ";
        pre += "const uint16_t t_ = (uint16_t)(lo_ | hi_ << 8); ";
        return "(uint16_t)(t_ + y_)"; /* the pointer, then Y */
    }
    default: return "0";
    }
}

/* the operand's value as C: immediates and ROM-backed windows are constants,
 * the rest goes through the bus windows as the interpreter sees them */
std::string Emitter::operand(const Ins& in, std::string& pre) const {
    switch (in.info.mode) {
    case Mode::imm: return hex(in.o[0], 2);
    case Mode::zp: {
        if (win(in.ctx, uint16_t(0x2000u | in.o[0])) == Win::rom) {
            const auto b = ctx_byte(in.ctx, uint16_t(0x2000u | in.o[0]));
            if (b) return hex(*b, 2);
        }
        return "PCE_RD((uint16_t)(0x2000u | " + hex(in.o[0], 2) + "u))";
    }
    case Mode::zpx: return "PCE_RD(PCE_ZP_AT((uint8_t)(" + hex(in.o[0], 2) + "u + x_)))";
    case Mode::zpy: return "PCE_RD(PCE_ZP_AT((uint8_t)(" + hex(in.o[0], 2) + "u + y_)))";
    case Mode::abs: {
        const unsigned w = in.o[0] | (in.o[1] << 8);
        if (win(in.ctx, uint16_t(w)) == Win::rom) {
            const auto b = ctx_byte(in.ctx, uint16_t(w));
            if (b) return hex(*b, 2);
        }
        return "PCE_RD(" + hex(w) + ")";
    }
    case Mode::absx: {
        const unsigned w = in.o[0] | (in.o[1] << 8);
        return "PCE_RD((uint16_t)(" + hex(w) + "u + x_))";
    }
    case Mode::absy: {
        const unsigned w = in.o[0] | (in.o[1] << 8);
        return "PCE_RD((uint16_t)(" + hex(w) + "u + y_))";
    }
    case Mode::ind: case Mode::izx: case Mode::izy: {
        const std::string at = ea(in, pre);
        if (in.info.mode == Mode::izy) return "PCE_RD((uint16_t)(t_ + y_))";
        return "PCE_RD(" + at + ")";
    }
    case Mode::tst_zp: return "PCE_RD(PCE_ZP_AT(" + hex(in.o[1], 2) + "u))";
    case Mode::tst_zpx: return "PCE_RD(PCE_ZP_AT((uint8_t)(" + hex(in.o[1], 2) + "u + x_)))";
    case Mode::tst_abs: {
        const unsigned w = in.o[1] | (in.o[2] << 8);
        if (win(in.ctx, uint16_t(w)) == Win::rom) {
            const auto b = ctx_byte(in.ctx, uint16_t(w));
            if (b) return hex(*b, 2);
        }
        return "PCE_RD(" + hex(w) + ")";
    }
    case Mode::tst_absx: {
        const unsigned w = in.o[1] | (in.o[2] << 8);
        return "PCE_RD((uint16_t)(" + hex(w) + "u + x_))";
    }
    default: return "0";
    }
}

/* precise mode: true when the instruction's bus accesses may land on the
 * I/O page (the IRQ sources move there), so the region must leave and let
 * the dispatcher sample the line where the interpreter would */
bool Emitter::may_bus(const Ins& in) const {
    switch (in.info.shape) {
    case Shape::tam: case Shape::stv: case Shape::block: return true;
    case Shape::imp: {
        switch (in.info.op) {
        case Op::pha: case Op::php: case Op::phx: case Op::phy:
            return win(in.ctx, 0x2100) != Win::wram;
        case Op::pla: case Op::plp: case Op::plx: case Op::ply:
        case Op::rts: case Op::rti:
            return win(in.ctx, 0x2100) == Win::bus;
        default: return false;
        }
    }
    case Shape::flow: {
        if (in.info.op == Op::jsr || in.info.op == Op::bsr)
            return win(in.ctx, 0x2100) != Win::wram;
        if (in.info.op == Op::rts || in.info.op == Op::rti)
            return win(in.ctx, 0x2100) == Win::bus;
        if (in.info.op == Op::jmp && in.info.mode != Mode::abs)
            return true; /* through a pointer */
        return false;
    }
    case Shape::load: case Shape::cmp: {
        /* reads: only the hardware page and open bus answer through the bus */
        switch (in.info.mode) {
        case Mode::zp: case Mode::zpx: case Mode::zpy:
            return win(in.ctx, 0x2000) == Win::bus;
        case Mode::abs: return win(in.ctx, uint16_t(in.o[0] | (in.o[1] << 8))) == Win::bus;
        case Mode::absx: case Mode::absy: {
            const unsigned w = in.o[0] | (in.o[1] << 8);
            for (unsigned at = w; at <= w + 0xFFu && at <= 0xFFFFu; at += 0x2000u - (at & 0x1FFFu))
                if (win(in.ctx, uint16_t(at)) == Win::bus) return true;
            return win(in.ctx, uint16_t(std::min(w + 0xFFu, 0xFFFFu))) == Win::bus;
        }
        case Mode::ind: case Mode::izx: case Mode::izy: return true;
        case Mode::tst_zp: case Mode::tst_zpx: return win(in.ctx, 0x2000) == Win::bus;
        case Mode::tst_abs: return win(in.ctx, uint16_t(in.o[1] | (in.o[2] << 8))) == Win::bus;
        case Mode::tst_absx: return true;
        default: return false;
        }
    }
    case Shape::bit: {
        /* the same reads, the immediate excepted */
        if (in.info.mode == Mode::imm) return false;
        Ins read = in;
        read.info.shape = Shape::load;
        return may_bus(read);
    }
    case Shape::logic: case Shape::arith: {
        /* the T mode writes the zero page at X whatever the addressing */
        switch (in.info.mode) {
        case Mode::imm: return win(in.ctx, 0x2000) != Win::wram;
        case Mode::zp: case Mode::zpx:
            return win(in.ctx, 0x2000) != Win::wram;
        case Mode::abs: return win(in.ctx, uint16_t(in.o[0] | (in.o[1] << 8))) != Win::wram;
        case Mode::absx: case Mode::absy: case Mode::ind: case Mode::izx: case Mode::izy:
            return true;
        default: return false;
        }
    }
    case Shape::store: {
        switch (in.info.mode) {
        case Mode::zp: case Mode::zpx: case Mode::zpy:
            return win(in.ctx, 0x2000) != Win::wram;
        case Mode::abs: return win(in.ctx, uint16_t(in.o[0] | (in.o[1] << 8))) != Win::wram;
        default: return true; /* indexed and pointed stores may leave the window */
        }
    }
    case Shape::shift: case Shape::incdec: case Shape::tsbtrb: case Shape::rmbsmb: {
        /* read-modify-writes */
        switch (in.info.mode) {
        case Mode::acc: return false;
        case Mode::zp: case Mode::zpx: return win(in.ctx, 0x2000) != Win::wram;
        case Mode::abs: return win(in.ctx, uint16_t(in.o[0] | (in.o[1] << 8))) != Win::wram;
        default: return true;
        }
    }
    case Shape::tst: {
        switch (in.info.mode) {
        case Mode::tst_zp: case Mode::tst_zpx: return win(in.ctx, 0x2000) == Win::bus;
        case Mode::tst_abs: return win(in.ctx, uint16_t(in.o[1] | (in.o[2] << 8))) == Win::bus;
        default: return true;
        }
    }
    default: return false;
    }
}

bool Emitter::emit_ins(std::ostream& o, const Ins& in, bool t_zero, bool tail) {
    const Op op = in.info.op;
    const Mode m = in.info.mode;
    const std::string next = go(in, in.next);
    const int next_hint = hint_of(in, in.next);
    std::string pre, body;
    unsigned cycles = in.info.cycles;
    std::string after;  /* after the budget: an exit the instruction asks for */
    /* the interpreter clears T after every instruction but SET, RTI and
     * PLP; when T is already known zero on every path to here, the clear
     * is dead (an entry — a label — knows nothing: t_zero starts false) */
    const bool clears_t = op != Op::set && op != Op::plp;
    const std::string t_clear = t_zero && clears_t ? "" : " p_ &= (uint8_t)~PCE_P_T;";
    /* the two exits at a transfer and at a spent budget do the same thing
     * since the epilogue took the hint: one statement each is dead */
    auto budget_exit = [&](void) {
        return " if (budget <= 0) { pc_ = " + hex(in.next) + "; ni = " + std::to_string(next_hint) + "; goto out; }";
    };
    /* the region's last instruction: its fallthrough continues with the
     * transfer emit_region appends — dead check when that transfer leaves */
    const bool fall_exits = tail && next.compare(0, 6, "goto L") != 0;
    /* the flags still read after this instruction (the liveness pass
     * keeps 0xF in precise mode and at every exit) */
    const uint8_t lv = in.live_out;
    /* the macros of pce_gen.h take the mask in the bits of P (N=$80, V=$40,
     * Z=2, C=1), not in the liveness encoding above */
    const std::string lvm = hex(uint8_t((lv & FNZ_N ? 0x80 : 0) | (lv & FNZ_V ? 0x40 : 0) |
                                        (lv & FNZ_Z ? 0x02 : 0) | (lv & FNZ_C ? 0x01 : 0)), 2);
    const bool nzl = (lv & (FNZ_N | FNZ_Z)) != 0;
    auto store = [&](const std::string& value) {
        std::string pre2;
        const std::string at = ea(in, pre2);
        if (!pre2.empty()) pre += pre2;
        return "PCE_WR(" + at + ", (uint8_t)(" + value + "));";
    };
    switch (in.info.shape) {
    case Shape::load: {
        const char* reg = op == Op::lda ? "a_" : op == Op::ldx ? "x_" : "y_";
        body = std::string(reg) + " = " + operand(in, pre) + ";" +
               (nzl ? " PCE_ZN(" + std::string(reg) + ");" : "");
        break;
    }
    case Shape::store:
        body = store(op == Op::sta ? "a_" : op == Op::stx ? "x_" : op == Op::sty ? "y_" : "0");
        break;
    case Shape::logic: {
        const char* tok = op == Op::and_ ? "&" : op == Op::ora ? "|" : "^";
        body = std::string("PCE_LOGIC(") + tok + ", " + operand(in, pre) + ", " + lvm + ");";
        break;
    }
    case Shape::arith:
        body = std::string(op == Op::adc ? "PCE_ARITH(PCE_ADC_CORE, " : "PCE_ARITH(PCE_SBC_CORE, ") +
                operand(in, pre) + ", " + lvm + ");";
        break;
    case Shape::cmp: {
        const char* reg = op == Op::cmp ? "a_" : op == Op::cpx ? "x_" : "y_";
        body = std::string("PCE_CMP(") + reg + ", " + operand(in, pre) + ", " + lvm + ");";
        break;
    }
    case Shape::bit:
        body = "PCE_BIT(" + operand(in, pre) + ", " + lvm + ");";
        break;
    case Shape::shift: {
        const char* f = op == Op::asl ? "PCE_ASL" : op == Op::lsr ? "PCE_LSR" : op == Op::rol ? "PCE_ROL" : "PCE_ROR";
        if (m == Mode::acc) {
            body = std::string("a_ = ") + f + "(a_, " + lvm + ");";
            break;
        }
        const std::string at = ea(in, pre);
        body = "{ const uint16_t at_ = " + at + "; const uint8_t r_ = " + f + "(PCE_RD(at_), " + lvm +
               "); PCE_WR(at_, r_); }";
        break;
    }
    case Shape::incdec: {
        const std::string at = ea(in, pre);
        body = "{ const uint16_t at_ = " + at + "; const uint8_t r_ = (uint8_t)(PCE_RD(at_) " +
               (op == Op::inc ? "+" : "-") + " 1u); PCE_WR(at_, r_);" +
               (nzl ? " PCE_ZN(r_);" : "") + " }";
        break;
    }
    case Shape::tsbtrb: {
        const std::string at = ea(in, pre);
        if (op == Op::tsb)
            body = "{ const uint16_t at_ = " + at + "; const uint8_t m_ = PCE_RD(at_); "
                   "p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ | a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u)); "
                   "PCE_WR(at_, (uint8_t)(m_ | a_)); }";
        else
            body = "{ const uint16_t at_ = " + at + "; const uint8_t m_ = PCE_RD(at_); "
                   "p_ = (uint8_t)((p_ & ~(PCE_P_Z | PCE_P_V | PCE_P_N)) | ((m_ & (uint8_t)~a_) ? 0 : PCE_P_Z) | (m_ & 0xC0u)); "
                   "PCE_WR(at_, (uint8_t)(m_ & (uint8_t)~a_)); }";
        break;
    }
    case Shape::tst:
        body = "{ const uint8_t m_ = " + operand(in, pre) + "; PCE_TST(" + hex(in.o[0], 2) + ", m_, " + lvm + "); }";
        break;
    case Shape::rmbsmb: {
        const unsigned bit = (in.code >> 4) & 7;
        const bool setbit = (in.code & 0x80u) != 0;
        const std::string one = setbit ? "r_ |= (uint8_t)(1u << " + std::to_string(bit) + "u);"
                                       : "r_ &= (uint8_t)~(1u << " + std::to_string(bit) + "u);";
        body = "{ const uint16_t at_ = (uint16_t)(0x2000u | " + hex(in.o[0], 2) + "u); uint8_t r_ = PCE_RD(at_); " +
               one + " PCE_WR(at_, r_); }";
        break;
    }
    case Shape::tam:
        /* a provable TAM (LDA #imm before it) only refreshes the region's
         * window tables; the rest changes the mapping out from under the
         * region's compiled constants: it leaves, and the dispatcher's MPR
         * check decides what the new windows still compile for */
        body = "{ for (unsigned i_ = 0; i_ < 8; i_++) if (" + hex(in.o[0], 2) +
               "u & (1u << i_)) machine.cpu.mpr[i_] = a_; huc6280_map(&machine.cpu); }";
        /* both kinds leave at the epilogue (may_bus): the region split in
         * plan() is what tells an unproven TAM apart, not the exit */
        break;
    case Shape::tma:
        body = "{ for (unsigned i_ = 0; i_ < 8; i_++) if (" + hex(in.o[0], 2) +
               "u & (1u << i_)) a_ = machine.cpu.mpr[i_]; }";
        break;
    case Shape::stv: {
        const char* port = op == Op::st0 ? "0x0000" : op == Op::st1 ? "0x0002" : "0x0003";
        body = std::string("pce_bus_write(") + port + ", a_);";
        break;
    }
    case Shape::block: {
        static const char* loop_body[] = {
            /* TII */ "const uint8_t v_ = PCE_RD(src); PCE_WR(dst, v_); src++; dst++;",
            /* TDD */ "const uint8_t v_ = PCE_RD(src); PCE_WR(dst, v_); src--; dst--;",
            /* TIN */ "const uint8_t v_ = PCE_RD(src); PCE_WR(dst, v_); src++;",
            /* TIA */ "const uint8_t v_ = PCE_RD(src); PCE_WR((uint16_t)(dst + alt), v_); src++;",
            /* TAI */ "const uint8_t v_ = PCE_RD((uint16_t)(src + alt)); PCE_WR(dst, v_); dst++;",
        };
        const int which = op == Op::tii ? 0 : op == Op::tdd ? 1 : op == Op::tin ? 2 : op == Op::tia ? 3 : 4;
        const unsigned sl = in.o[0] | (in.o[1] << 8), dl = in.o[2] | (in.o[3] << 8), ln = in.o[4] | (in.o[5] << 8);
        body = "{ const unsigned sl_ = " + hex(sl) + ", dl_ = " + hex(dl) + ", ln_ = " + hex(ln) + "; "
               "PCE_PUSH(y_); PCE_PUSH(a_); PCE_PUSH(x_); "
               "uint16_t src = (uint16_t)sl_, dst = (uint16_t)dl_, len = (uint16_t)ln_; "
               "unsigned alt = 0; uint32_t n_ = 0; "
               "do { " + std::string(loop_body[which]) + "; alt ^= 1; len--; n_++; } while (len); "
               "x_ = PCE_POP(); a_ = PCE_POP(); y_ = PCE_POP(); "
               "budget -= 17u + 6u * n_; }";
        cycles = 0; /* subtracted inside */
        break;
    }
    case Shape::flow:
        if (is_branch(op)) {
            static const char* cond[] = { "1", "!(p_ & PCE_P_N)", "(p_ & PCE_P_N)", "!(p_ & PCE_P_V)",
                                          "(p_ & PCE_P_V)", "!(p_ & PCE_P_C)", "(p_ & PCE_P_C)",
                                          "!(p_ & PCE_P_Z)", "(p_ & PCE_P_Z)" };
            const unsigned c = op == Op::bra ? 0 : 1 + unsigned(op) - unsigned(Op::bpl);
            o << "    if (" << cond[c] << ") {" << t_clear << " budget -= 4;"
              << " " << go(in, in.target) << " }\n";
            o << "   " << t_clear << " budget -= 2;";
            /* the not-taken path falls into the next instruction inline;
             * when the region ends here with a transfer to the same
             * address, that transfer is the same exit and the check is
             * dead */
            if (opt_.precise && !fall_exits) o << budget_exit();
            o << "\n";
            return true;
        }
        if (op == Op::bbr || op == Op::bbs) {
            const unsigned bit = (in.code >> 4) & 7;
            const std::string take = std::string(op == Op::bbs ? "" : "!") + "((m_ >> " +
                                     std::to_string(bit) + "u) & 1u)";
            o << "    { const uint8_t m_ = PCE_RD(PCE_ZP_AT(" << hex(in.o[0], 2) << "u));\n"
              << "    if (" << take << ") {" << t_clear << " budget -= 8;"
              << " " << go(in, in.target) << " }\n"
              << "   " << t_clear << " budget -= 6;";
            if (opt_.precise && !fall_exits) o << budget_exit();
            o << " }\n";
            return true;
        }
        if (op == Op::jmp && in.has_target) {
            if (in.target == in.addr) {
                /* the idle loop the other runtimes spend at once
                 * (m0_idle.h): left with pc on it, as the interpreter */
                o << "   " << t_clear << " budget = PCE_SPIN4(budget); { pc_ = " << hex(in.addr)
                  << "; goto out; }\n";
                return true;
            }
            o << "   " << t_clear << " budget -= 4;" << " " << go(in, in.target) << "\n";
            return true;
        }
        if (op == Op::jmp && m == Mode::ind) {
            o << "    { const uint16_t ptr_ = " << hex(in.o[0] | (in.o[1] << 8)) << "; "
              << "const unsigned lo_ = PCE_RD(ptr_); "
              << "const unsigned hi_ = PCE_RD((uint16_t)((ptr_ & 0xFF00u) | ((ptr_ + 1u) & 0xFFu))); "
              << "pc_ = (uint16_t)(lo_ | hi_ << 8); }" << t_clear << " budget -= 7; goto out;\n";
            return true;
        }
        if (op == Op::jmp) { /* (abs,X) */
            o << "    { const uint16_t ptr_ = (uint16_t)(" << hex(in.o[0] | (in.o[1] << 8)) << "u + x_); "
              << "const unsigned lo_ = PCE_RD(ptr_); const unsigned hi_ = PCE_RD((uint16_t)(ptr_ + 1u)); "
              << "pc_ = (uint16_t)(lo_ | hi_ << 8); }" << t_clear << " budget -= 7; goto out;\n";
            return true;
        }
        if (op == Op::jsr || op == Op::bsr) {
            const unsigned ret = op == Op::jsr ? unsigned(in.addr) + 2u : unsigned(in.addr) + 1u;
            o << "    PCE_PUSH(" << hex(ret >> 8, 2) << "); PCE_PUSH(" << hex(ret & 0xFF, 2)
              << ");" << t_clear << " budget -= " << (op == Op::jsr ? 7 : 8) << ";"
              << " " << go(in, in.target) << "\n";
            return true;
        }
        if (op == Op::rts) {
            o << "    { const uint8_t lo8_ = PCE_POP(); const uint8_t hi8_ = PCE_POP(); "
              << "pc_ = (uint16_t)((uint16_t)(lo8_ | hi8_ << 8) + 1u); }" << t_clear
              << " budget -= 7; goto out;\n";
            return true;
        }
        /* RTI: P back (T kept), then pc */
        o << "    p_ = PCE_POP(); { const uint8_t lo8_ = PCE_POP(); const uint8_t hi8_ = PCE_POP(); "
          << "pc_ = (uint16_t)(lo8_ | hi8_ << 8); } budget -= 7; goto out;\n";
        return false;
    case Shape::imp:
        switch (op) {
        case Op::ina: body = nzl ? "a_ = (uint8_t)(a_ + 1); PCE_ZN(a_);" : "a_ = (uint8_t)(a_ + 1);"; break;
        case Op::dea: body = nzl ? "a_ = (uint8_t)(a_ - 1); PCE_ZN(a_);" : "a_ = (uint8_t)(a_ - 1);"; break;
        case Op::inx: body = nzl ? "x_ = (uint8_t)(x_ + 1); PCE_ZN(x_);" : "x_ = (uint8_t)(x_ + 1);"; break;
        case Op::dex: body = nzl ? "x_ = (uint8_t)(x_ - 1); PCE_ZN(x_);" : "x_ = (uint8_t)(x_ - 1);"; break;
        case Op::iny: body = nzl ? "y_ = (uint8_t)(y_ + 1); PCE_ZN(y_);" : "y_ = (uint8_t)(y_ + 1);"; break;
        case Op::dey: body = nzl ? "y_ = (uint8_t)(y_ - 1); PCE_ZN(y_);" : "y_ = (uint8_t)(y_ - 1);"; break;
        case Op::tax: body = nzl ? "x_ = a_; PCE_ZN(x_);" : "x_ = a_;"; break;
        case Op::tay: body = nzl ? "y_ = a_; PCE_ZN(y_);" : "y_ = a_;"; break;
        case Op::txa: body = nzl ? "a_ = x_; PCE_ZN(a_);" : "a_ = x_;"; break;
        case Op::tya: body = nzl ? "a_ = y_; PCE_ZN(a_);" : "a_ = y_;"; break;
        case Op::tsx: body = nzl ? "x_ = s_; PCE_ZN(x_);" : "x_ = s_;"; break;
        case Op::txs: body = "s_ = x_;"; break;
        case Op::cla: body = "a_ = 0;"; break;
        case Op::clx: body = "x_ = 0;"; break;
        case Op::cly: body = "y_ = 0;"; break;
        case Op::sxy: body = "{ const uint8_t t_ = x_; x_ = y_; y_ = t_; }"; break;
        case Op::sax: body = "{ const uint8_t t_ = x_; x_ = a_; a_ = t_; }"; break;
        case Op::say: body = "{ const uint8_t t_ = y_; y_ = a_; a_ = t_; }"; break;
        case Op::csl: body = "machine.cpu.speed = 0;"; break;
        case Op::csh: body = "machine.cpu.speed = 1;"; break;
        case Op::nop: body = ""; break;
        case Op::clc: body = (lv & FNZ_C) ? "p_ &= (uint8_t)~PCE_P_C;" : ""; break;
        case Op::sec: body = (lv & FNZ_C) ? "p_ |= PCE_P_C;" : ""; break;
        case Op::cli: body = "p_ &= (uint8_t)~PCE_P_I;"; break;
        case Op::sei: body = "p_ |= PCE_P_I;"; break;
        case Op::clv: body = (lv & FNZ_V) ? "p_ &= (uint8_t)~PCE_P_V;" : ""; break;
        case Op::cld: body = "p_ &= (uint8_t)~PCE_P_D;"; break;
        case Op::sed: body = "p_ |= PCE_P_D;"; break;
        case Op::set: body = "p_ |= PCE_P_T;"; break;
        case Op::pha: body = "PCE_PUSH(a_);"; break;
        case Op::phx: body = "PCE_PUSH(x_);"; break;
        case Op::phy: body = "PCE_PUSH(y_);"; break;
        case Op::php: body = "p_ &= (uint8_t)~PCE_P_T; PCE_PUSH((uint8_t)(p_ | PCE_P_B));"; break;
        case Op::pla: body = nzl ? "a_ = PCE_POP(); PCE_ZN(a_);" : "a_ = PCE_POP();"; break;
        case Op::plx: body = nzl ? "x_ = PCE_POP(); PCE_ZN(x_);" : "x_ = PCE_POP();"; break;
        case Op::ply: body = nzl ? "y_ = PCE_POP(); PCE_ZN(y_);" : "y_ = PCE_POP();"; break;
        case Op::plp:
            body = "p_ = PCE_POP();";
            /* the mask may have opened: the interpreter samples at once */
            after = "if (machine.cpu.irq_low & machine.cpu.irq_mask && !(p_ & PCE_P_I)) { pc_ = " +
                    hex(in.next) + "; ni = " + std::to_string(next_hint) + "; goto out; }";
            break;
        default: break;
        }
        break;
    default:
        o << "    { pc_ = " << hex(in.addr) << "; goto out; }\n";
        return true;
    }
    o << "    { " << pre << body << t_clear;
    if (cycles) o << " budget -= " << cycles << ";";
    o << " }";
    if (opt_.precise)
        /* precise: the dispatcher regains control after every
         * instruction — the budget and the IRQ line are sampled exactly
         * where the interpreter samples them, whatever the region's
         * length (the fast mode keeps its regions whole) */
        o << " { pc_ = " << hex(in.next) << "; ni = " << std::to_string(next_hint) << "; goto out; }";
    if (op == Op::cli)
        after = "if (machine.cpu.irq_low & machine.cpu.irq_mask) { pc_ = " + hex(in.next) +
                "; ni = " + std::to_string(next_hint) + "; goto out; }";
    if (!after.empty()) o << " " << after;
    o << "\n";
    return clears_t;
}

void Emitter::emit_region(std::ostream& o, int r) {
    const auto& keys = regions_[size_t(r)];
    o << "\n" << (opt_.regions_per_file ? "" : "static ") << "int32_t r_" << r << "(int32_t budget, unsigned entry) {\n"
      << "    PCE_LOAD();\n    uint16_t pc_;\n    int ni = -1;\n    switch (entry) {\n";
    for (const Key& k : keys) {
        const Ins& in = ins_.at(k);
        if (in.label) {
            char b[16];
            snprintf(b, sizeof b, "%04X", in.addr);
            o << "    case " << in.local << ": goto L_" << b << ";\n";
        }
    }
    o << "    default: __builtin_unreachable();\n    }\n";
    /* T is whatever the entering flow left: only a fallthrough from an
     * instruction that clears it knows it zero */
    bool t_zero = false;
    for (size_t i = 0; i < keys.size(); i++) {
        const Ins& in = ins_.at(keys[i]);
        if (in.label) {
            char b[16];
            snprintf(b, sizeof b, "%04X", in.addr);
            o << "L_" << b << ":";
            if (!opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.addr) << "; goto out; }";
            o << "\n";
            t_zero = false;
        }
        /* the region's last instruction falls into the next one's place */
        const bool tail = i + 1 == keys.size() && in.falls && in.info.op != Op::jmp;
        t_zero = emit_ins(o, in, t_zero, tail);
        if (tail) o << "    " << go(in, in.next) << "\n";
    }
    /* every exit left the hint in ni: -1 where the target is no entry */
    o << "out:\n    pce_gen_next = ni; PCE_SAVE(); machine.cpu.pc = pc_; return budget;\n}\n";
}

std::string Emitter::run(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    select(coverage, stats);
    plan();
    std::ostringstream o;
    std::ostringstream pre;
    pre << "/* Generated by gbrecomp --system pce" << (opt_.precise ? " --meta-precise" : "")
        << ". Do not edit. */\n"
        << "#define PCE_GEN_IMPL\n"
        << "#include \"pce_gen.h\"\n"
        << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
        << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
        << "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n"
        << "#ifdef PCE_GEN_O2\n"
        << "#pragma GCC optimize(\"O2\") /* the HuCard left the app slot: the code fits */\n"
        << "#elif defined(__arm__)\n"
        << "#pragma GCC optimize(\"Os\") /* a fifth smaller than -O2 on Thumb-1 */\n"
        << "#endif\n\n";
    o << pre.str();
    if (opt_.regions_per_file) {
        /* the regions live in other files: one function each, called through region_fn */
        o << "/* the next entry a region left for the dispatcher (checked against pc and MPR) */\n"
          << "int pce_gen_next = -1;\n";
        for (size_t r = 0; r < regions_.size(); r++)
            o << "int32_t r_" << r << "(int32_t budget, unsigned entry);\n";
        for (size_t first = 0; first < regions_.size(); first += opt_.regions_per_file) {
            std::ostringstream part;
            part << pre.str() << "extern int pce_gen_next;\n";
            const size_t last = std::min(regions_.size(), first + opt_.regions_per_file);
            for (size_t r = first; r < last; r++) emit_region(part, int(r));
            stats.parts.push_back(part.str());
        }
    } else {
        o << "/* the next entry a region left for the dispatcher (checked against pc and MPR) */\n"
          << "static int pce_gen_next = -1;\n";
        /* noinline: one function per region, called by the dispatcher only */
        for (size_t r = 0; r < regions_.size(); r++)
            o << "static int32_t r_" << r << "(int32_t budget, unsigned entry) __attribute__((noinline));\n";
        for (size_t r = 0; r < regions_.size(); r++) emit_region(o, int(r));
    }

    std::vector<const Ins*> entries;
    for (const auto& [key, in] : ins_)
        if (in.label) entries.push_back(&in);
    stats.regions = regions_.size();
    stats.entries = entries.size();
    const size_t count = std::max<size_t>(entries.size(), 1);
    o << "\nstatic const pce_region_fn region_fn[" << std::max<size_t>(regions_.size(), 1) << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 8 ? " " : "\n    ") << "r_" << r << ",";
    if (regions_.empty()) o << "0";
    /* the region and slot indexes outgrow 16 bits on a big profile (one
     * entry per address and MPR context): 0xFFFF stays the empty slot */
    const bool wide = regions_.size() >= 0xFFFFu || entries.size() >= 0xFFFFu;
    const char* idx_t = wide ? "uint32_t" : "uint16_t";
    const uint32_t empty = wide ? 0xFFFFFFFFu : 0xFFFFu;
    o << "\n};\nstatic const uint16_t entry_pc[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 12 ? " " : "\n    ") << hex(entries[i]->addr) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint64_t entry_mpr[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 4 ? " " : "\n    ") << hex64(entries[i]->ctx) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const " << idx_t << " entry_region[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->region << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_local[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->local << ",";
    if (entries.empty()) o << "0";
    /* open addressing over (pc, MPR context), twice as many slots as
     * entries: one multiply and usually one probe */
    unsigned bits = 4;
    while ((size_t(1) << bits) < entries.size() * 2) bits++;
    const size_t slots = size_t(1) << bits;
    auto slot_of = [&](uint16_t pc, uint64_t mpr) {
        const uint64_t h64 = uint64_t(pc) * 0x9E3779B97F4A7C15ull + mpr * 0xFF51AFD7ED558CCDull;
        return unsigned((uint32_t(h64) ^ uint32_t(h64 >> 32)) >> (32 - bits));
    };
    std::vector<uint32_t> slot(slots, empty);
    for (size_t i = 0; i < entries.size(); i++) {
        unsigned h = slot_of(entries[i]->addr, entries[i]->ctx);
        while (slot[h] != empty) h = (h + 1) & (slots - 1);
        slot[h] = uint32_t(i);
    }
    o << "\n};\nstatic const " << idx_t << " entry_slot[" << slots << "] = {";
    for (size_t i = 0; i < slots; i++) o << (i % 12 ? " " : "\n    ") << hex(slot[i]) << ",";
    o << "\n};\n\n"
      << "int pce_gen_lookup(uint16_t pc, uint64_t mpr) {\n"
      << "    const int hint = pce_gen_next;\n"
      << "    if (hint >= 0 && entry_pc[hint] == pc && entry_mpr[hint] == mpr) return hint;\n"
      << "    unsigned h = ((uint32_t)((uint64_t)pc * 0x9E3779B97F4A7C15ull + mpr * 0xFF51AFD7ED558CCDull) ^\n"
      << "                  (uint32_t)((uint64_t)pc * 0x9E3779B97F4A7C15ull + mpr * 0xFF51AFD7ED558CCDull >> 32)) >> "
      << (32 - bits) << ";\n"
      << "    for (;;) {\n"
      << "        const unsigned i = entry_slot[h];\n"
      << "        if (i == " << (wide ? "0xFFFFFFFFu" : "0xFFFFu") << ") return -1;\n"
      << "        if (entry_pc[i] == pc && entry_mpr[i] == mpr) return (int)i;\n"
      << "        h = (h + 1u) & " << (slots - 1) << "u;\n"
      << "    }\n}\n\n"
      << "int32_t pce_gen_enter(int entry, int32_t budget) {\n"
      << "    return region_fn[entry_region[entry]](budget, entry_local[entry]);\n}\n";
    return o.str();
}

} // namespace

std::string emit_code(const std::vector<uint8_t>& rom, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats) {
    Emitter e(rom, options);
    return e.run(coverage, stats);
}

} // namespace m0boy::pce
