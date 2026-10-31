/* 65C816 -> C for the SNES runtime.
 *
 * After the PCE emitter's plan (regions, dispatcher, interpreter fallback),
 * with the 65C816's own banked world folded in: an instruction instance is
 * a 24-bit address (K:PC) under one context — the data bank, the direct
 * page and the M/X/E flags packed with K (snes_ctx_key's layout, K lowest).
 * The walk starts at the reset vector under the reset context (K and DBR
 * at $00, the direct page at $0000, the emulation mode with its 8-bit
 * widths) and follows what it can prove: SEP/REP with known flags, XCE
 * with a known carry (CLC/SEC before it), LDA #imm (16 bits only) then
 * TCD, JML/JSL/JMP [abs] to their operand banks.  PLB, PLD, a TCD with an
 * unknown A, an XCE with an unknown carry and a PLP outside the emulation
 * mode end their run — the path falls back to the interpreter, and the
 * dispatcher only enters an entry whose context still matches the
 * machine's.
 *
 * Every exit returns to the dispatcher (Thumb-1 GCC never tail-calls): a
 * known target leaves its entry as a hint the lookup checks first; RTS,
 * RTL, RTI, BRK, COP and the jumps through pointers leave K:PC to be
 * looked up.  BRK and COP push and jump inside the region (their vectors
 * come from the context's emulation bit), WAI and STP spend the rest of
 * the slice — the same bytes the interpreter would run.
 *
 * The semantics are the interpreter's (snes_cpu.c), cycle for cycle: the
 * same base counts and the same penalties — the direct page's low byte
 * and the widths are context constants, the page crossings of the indexed
 * reads stay runtime conditions on X and Y.  Precise mode checks the
 * budget after every instruction and leaves the region after any that may
 * touch the I/O window, so the dispatcher samples the interrupt lines
 * where the interpreter does; the fast mode checks only at labels and at
 * most every `check_cycles` cycles. */
#include "m0boy/snes_emitter.h"
#include <cstdio>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <sstream>

namespace m0boy::snes {
namespace {

enum class Mode {
    imp, imm, dp, dpx, dpy, ind, indx, indy, indlp, indlpy,
    abs, absx, absy, long_, longx, longy, sr, sry, rel, rell, block,
};

enum class Op {
    none, ora, and_, eor, adc, sbc, cmp, cpx, cpy, lda, sta, ldx, ldy, stx, sty, stz,
    bit, asl, lsr, rol, ror, inc, dec, ina, dea, inx, dex, iny, dey, tsb, trb,
    mvn, mvp, jmp, jml, jsr, jsl, rts, rtl, rti, brk, cop,
    pha, php, phx, phy, phb, phd, phk, pla, plp, plx, ply, plb, pld, pea, pei, per,
    clc, sec, cli, sei, clv, cld, sed, sep, rep, xce, xba, nop, wdm, wai, stp,
    tax, tay, txa, tya, tsx, txs, tcd, tdc, tcs, tsc, txy, tyx,
};

enum class Shape {
    none, load, store, logic, arith, cmp, bit, shift, incdec, tsbtrb,
    imp, flag, sep, rep, xce, xba, stack, block, flow, wai, stp,
};

/* the cycle penalties the context knows: +1 when the accumulator or the
 * index registers are 16-bit, +1 when the direct page's low byte is not 0;
 * the page crossings of the indexed reads are runtime conditions (X and Y
 * live), emitted as such */
enum { P_M = 1, P_X = 2, P_DP = 4, P_CROSS_X = 8, P_CROSS_Y = 16 };

struct OpInfo {
    Op op = Op::none;
    Shape shape = Shape::none;
    Mode mode = Mode::imp;
    uint8_t base = 0;
    uint8_t len = 1;
    uint8_t pen = 0;
    bool keeps_a = false; /* CMP, BIT, the X/Y and flag work: A provably unchanged */
};

const std::vector<OpInfo>& table() {
    static std::vector<OpInfo> t = [] {
        std::vector<OpInfo> v(256);
        auto set = [&](unsigned code, Op op, Shape shape, Mode mode, unsigned base, unsigned len,
                       unsigned pen = 0, bool keeps_a = false) {
            v[code] = { op, shape, mode, uint8_t(base), uint8_t(len), uint8_t(pen), keeps_a };
        };
        /* the A operand's thirteen forms: imm dp dpx abs absx absy ind indx
         * indy indlp indlpy sr sriy */
        auto group = [&](Op op, Shape shape, const unsigned c[13], unsigned dp_base,
                         unsigned abs_base, bool keeps_a = false) {
            set(c[0], op, shape, Mode::imm, 2, 2, P_M, keeps_a);
            set(c[1], op, shape, Mode::dp, dp_base, 2, P_M | P_DP, keeps_a);
            set(c[2], op, shape, Mode::dpx, dp_base, 2, P_M | P_DP, keeps_a);
            set(c[3], op, shape, Mode::abs, abs_base, 3, P_M, keeps_a);
            set(c[4], op, shape, Mode::absx, abs_base, 3, P_M | P_CROSS_X, keeps_a);
            set(c[5], op, shape, Mode::absy, abs_base, 3, P_M | P_CROSS_Y, keeps_a);
            set(c[6], op, shape, Mode::ind, 5, 2, P_M | P_DP, keeps_a);
            set(c[7], op, shape, Mode::indx, 6, 2, P_M | P_DP, keeps_a);
            set(c[8], op, shape, Mode::indy, 5, 2, P_M | P_DP | P_CROSS_Y, keeps_a);
            set(c[9], op, shape, Mode::indlp, 6, 2, P_M | P_DP, keeps_a);
            set(c[10], op, shape, Mode::indlpy, 6, 2, P_M | P_DP, keeps_a);
            set(c[11], op, shape, Mode::sr, 4, 2, P_M | P_DP, keeps_a);
            set(c[12], op, shape, Mode::sry, 7, 2, P_M | P_DP | P_CROSS_Y, keeps_a);
        };
        const unsigned ora[13] = { 0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x12, 0x01, 0x11, 0x07, 0x17, 0x03, 0x13 };
        const unsigned and_[13] = { 0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x32, 0x21, 0x31, 0x27, 0x37, 0x23, 0x33 };
        const unsigned eor[13] = { 0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x52, 0x41, 0x51, 0x47, 0x57, 0x43, 0x53 };
        const unsigned adc[13] = { 0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x72, 0x61, 0x71, 0x67, 0x77, 0x63, 0x73 };
        const unsigned sbc[13] = { 0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xF2, 0xE1, 0xF1, 0xE7, 0xF7, 0xE3, 0xF3 };
        const unsigned cmp_[13] = { 0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xD2, 0xC1, 0xD1, 0xC7, 0xD7, 0xC3, 0xD3 };
        const unsigned lda[13] = { 0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xB2, 0xA1, 0xB1, 0xA7, 0xB7, 0xA3, 0xB3 };
        const unsigned sta[12] = { 0, 0x85, 0x95, 0x8D, 0x9D, 0x99, 0x92, 0x81, 0x91, 0x87, 0x97, 0x83 };
        const unsigned sta_sry = 0x93;
        group(Op::ora, Shape::logic, ora, 4, 4);
        group(Op::and_, Shape::logic, and_, 4, 4);
        group(Op::eor, Shape::logic, eor, 4, 4);
        group(Op::adc, Shape::arith, adc, 4, 4);
        group(Op::sbc, Shape::arith, sbc, 4, 4);
        group(Op::cmp, Shape::cmp, cmp_, 4, 4, true);
        group(Op::lda, Shape::load, lda, 4, 4);
        /* the long forms: the operand is the 24-bit address itself, no page
         * crossing to pay (the interpreter counts 5 + width only) */
        set(0x0F, Op::ora, Shape::logic, Mode::long_, 5, 4, P_M);
        set(0x1F, Op::ora, Shape::logic, Mode::longx, 5, 4, P_M);
        set(0x2F, Op::and_, Shape::logic, Mode::long_, 5, 4, P_M);
        set(0x3F, Op::and_, Shape::logic, Mode::longx, 5, 4, P_M);
        set(0x4F, Op::eor, Shape::logic, Mode::long_, 5, 4, P_M);
        set(0x5F, Op::eor, Shape::logic, Mode::longx, 5, 4, P_M);
        set(0x6F, Op::adc, Shape::arith, Mode::long_, 5, 4, P_M);
        set(0x7F, Op::adc, Shape::arith, Mode::longx, 5, 4, P_M);
        set(0x8F, Op::sta, Shape::store, Mode::long_, 5, 4, P_M);
        set(0x9F, Op::sta, Shape::store, Mode::longx, 5, 4, P_M);
        set(0xAF, Op::lda, Shape::load, Mode::long_, 5, 4, P_M);
        set(0xBF, Op::lda, Shape::load, Mode::longx, 5, 4, P_M);
        set(0xCF, Op::cmp, Shape::cmp, Mode::long_, 5, 4, P_M, true);
        set(0xDF, Op::cmp, Shape::cmp, Mode::longx, 5, 4, P_M, true);
        /* the stores pay no page-cross penalty; (sr,S),Y closes the set */
        set(sta[1], Op::sta, Shape::store, Mode::dp, 4, 2, P_M | P_DP);
        set(sta[2], Op::sta, Shape::store, Mode::dpx, 4, 2, P_M | P_DP);
        set(sta[3], Op::sta, Shape::store, Mode::abs, 4, 3, P_M);
        set(sta[4], Op::sta, Shape::store, Mode::absx, 5, 3, P_M);
        set(sta[5], Op::sta, Shape::store, Mode::absy, 5, 3, P_M);
        set(sta[6], Op::sta, Shape::store, Mode::ind, 5, 2, P_M | P_DP);
        set(sta[7], Op::sta, Shape::store, Mode::indx, 6, 2, P_M | P_DP);
        set(sta[8], Op::sta, Shape::store, Mode::indy, 6, 2, P_M | P_DP);
        set(sta[9], Op::sta, Shape::store, Mode::indlp, 6, 2, P_M | P_DP);
        set(sta[10], Op::sta, Shape::store, Mode::indlpy, 7, 2, P_M | P_DP);
        set(sta[11], Op::sta, Shape::store, Mode::sr, 4, 2, P_M | P_DP);
        set(sta_sry, Op::sta, Shape::store, Mode::sry, 7, 2, P_M | P_DP);
        set(0xA2, Op::ldx, Shape::load, Mode::imm, 2, 2, P_X, true);
        set(0xA6, Op::ldx, Shape::load, Mode::dp, 4, 2, P_X | P_DP, true);
        set(0xB6, Op::ldx, Shape::load, Mode::dpy, 4, 2, P_X | P_DP, true);
        set(0xAE, Op::ldx, Shape::load, Mode::abs, 4, 3, P_X, true);
        set(0xBE, Op::ldx, Shape::load, Mode::absy, 4, 3, P_CROSS_Y, true);
        set(0xA0, Op::ldy, Shape::load, Mode::imm, 2, 2, P_X, true);
        set(0xA4, Op::ldy, Shape::load, Mode::dp, 4, 2, P_X | P_DP, true);
        set(0xB4, Op::ldy, Shape::load, Mode::dpx, 4, 2, P_X | P_DP, true);
        set(0xAC, Op::ldy, Shape::load, Mode::abs, 4, 3, P_X, true);
        set(0xBC, Op::ldy, Shape::load, Mode::absx, 4, 3, P_CROSS_X, true);
        set(0x86, Op::stx, Shape::store, Mode::dp, 4, 2, P_X | P_DP);
        set(0x96, Op::stx, Shape::store, Mode::dpy, 4, 2, P_X | P_DP);
        set(0x8E, Op::stx, Shape::store, Mode::abs, 4, 3, P_X);
        set(0x84, Op::sty, Shape::store, Mode::dp, 4, 2, P_X | P_DP);
        set(0x94, Op::sty, Shape::store, Mode::dpx, 4, 2, P_X | P_DP);
        set(0x8C, Op::sty, Shape::store, Mode::abs, 4, 3, P_X);
        set(0xE0, Op::cpx, Shape::cmp, Mode::imm, 2, 2, P_X, true);
        set(0xE4, Op::cpx, Shape::cmp, Mode::dp, 4, 2, P_X | P_DP, true);
        set(0xEC, Op::cpx, Shape::cmp, Mode::abs, 4, 3, P_X, true);
        set(0xC0, Op::cpy, Shape::cmp, Mode::imm, 2, 2, P_X, true);
        set(0xC4, Op::cpy, Shape::cmp, Mode::dp, 4, 2, P_X | P_DP, true);
        set(0xCC, Op::cpy, Shape::cmp, Mode::abs, 4, 3, P_X, true);
        set(0x64, Op::stz, Shape::store, Mode::dp, 4, 2, P_M | P_DP);
        set(0x74, Op::stz, Shape::store, Mode::dpx, 4, 2, P_M | P_DP);
        set(0x9C, Op::stz, Shape::store, Mode::abs, 4, 3, P_M);
        set(0x9E, Op::stz, Shape::store, Mode::absx, 5, 3, P_M);
        set(0x89, Op::bit, Shape::bit, Mode::imm, 2, 2, P_M, true);
        set(0x24, Op::bit, Shape::bit, Mode::dp, 4, 2, P_M | P_DP, true);
        set(0x34, Op::bit, Shape::bit, Mode::dpx, 4, 2, P_M | P_DP, true);
        set(0x2C, Op::bit, Shape::bit, Mode::abs, 4, 3, P_M, true);
        set(0x3C, Op::bit, Shape::bit, Mode::absx, 4, 3, P_M | P_CROSS_X, true);
        auto shift = [&](Op op, unsigned acc, unsigned dp, unsigned dpx, unsigned ab, unsigned abx) {
            if (acc) set(acc, op, Shape::shift, Mode::imp, 2, 1, P_M);
            set(dp, op, Shape::shift, Mode::dp, 5, 2, P_M | P_DP);
            set(dpx, op, Shape::shift, Mode::dpx, 6, 2, P_M | P_DP);
            set(ab, op, Shape::shift, Mode::abs, 6, 3, P_M);
            set(abx, op, Shape::shift, Mode::absx, 7, 3, P_M);
        };
        shift(Op::asl, 0x0A, 0x06, 0x16, 0x0E, 0x1E);
        shift(Op::lsr, 0x4A, 0x46, 0x56, 0x4E, 0x5E);
        shift(Op::rol, 0x2A, 0x26, 0x36, 0x2E, 0x3E);
        shift(Op::ror, 0x6A, 0x66, 0x76, 0x6E, 0x7E);
        auto rmw = [&](Op op, unsigned dp, unsigned dpx, unsigned ab, unsigned abx) {
            set(dp, op, Shape::incdec, Mode::dp, 5, 2, P_M | P_DP);
            set(dpx, op, Shape::incdec, Mode::dpx, 6, 2, P_M | P_DP);
            set(ab, op, Shape::incdec, Mode::abs, 6, 3, P_M);
            set(abx, op, Shape::incdec, Mode::absx, 7, 3, P_M);
        };
        rmw(Op::inc, 0xE6, 0xF6, 0xEE, 0xFE);
        rmw(Op::dec, 0xC6, 0xD6, 0xCE, 0xDE);
        set(0x1A, Op::ina, Shape::imp, Mode::imp, 2, 1, P_M);
        set(0x3A, Op::dea, Shape::imp, Mode::imp, 2, 1, P_M);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0xE8, Op::inx}, {0xC8, Op::iny}, {0xCA, Op::dex}, {0x88, Op::dey}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, 0, true);
        set(0x04, Op::tsb, Shape::tsbtrb, Mode::dp, 5, 2, P_M | P_DP, true);
        set(0x0C, Op::tsb, Shape::tsbtrb, Mode::abs, 6, 3, P_M, true);
        set(0x14, Op::trb, Shape::tsbtrb, Mode::dp, 5, 2, P_M | P_DP, true);
        set(0x1C, Op::trb, Shape::tsbtrb, Mode::abs, 6, 3, P_M, true);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x54, Op::mvn}, {0x44, Op::mvp}})
            set(code, op, Shape::block, Mode::block, 0, 3, 0, true);
        set(0x4C, Op::jmp, Shape::flow, Mode::abs, 3, 3, 0, true);
        set(0x6C, Op::jmp, Shape::flow, Mode::ind, 5, 3, 0, true);
        set(0x7C, Op::jmp, Shape::flow, Mode::absx, 6, 3, 0, true);
        set(0xDC, Op::jmp, Shape::flow, Mode::indlp, 6, 3, 0, true);
        set(0x5C, Op::jml, Shape::flow, Mode::long_, 4, 4, 0, true);
        set(0x20, Op::jsr, Shape::flow, Mode::abs, 6, 3, 0, true);
        set(0xFC, Op::jsr, Shape::flow, Mode::absx, 8, 3, 0, true);
        set(0x22, Op::jsl, Shape::flow, Mode::long_, 8, 4, 0, true);
        set(0x60, Op::rts, Shape::flow, Mode::imp, 6, 1, 0, true);
        set(0x6B, Op::rtl, Shape::flow, Mode::imp, 6, 1, 0, true);
        set(0x40, Op::rti, Shape::flow, Mode::imp, 0, 1, 0, true);
        set(0x00, Op::brk, Shape::flow, Mode::imp, 0, 2, 0, true);
        set(0x02, Op::cop, Shape::flow, Mode::imp, 0, 2, 0, true);
        for (auto [code, base] : std::initializer_list<std::pair<unsigned, unsigned>>{
                 {0x80, 2}, {0x10, 2}, {0x30, 2}, {0x50, 2}, {0x70, 2},
                 {0x90, 2}, {0xB0, 2}, {0xD0, 2}, {0xF0, 2}})
            set(code, Op::jmp, Shape::flow, Mode::rel, base, 2, 0, true);
        set(0x82, Op::jmp, Shape::flow, Mode::rell, 3, 3, 0, true);
        set(0x48, Op::pha, Shape::stack, Mode::imp, 3, 1, P_M, true);
        set(0x08, Op::php, Shape::stack, Mode::imp, 3, 1, 0, true);
        set(0xDA, Op::phx, Shape::stack, Mode::imp, 3, 1, P_X, true);
        set(0x5A, Op::phy, Shape::stack, Mode::imp, 3, 1, P_X, true);
        set(0x8B, Op::phb, Shape::stack, Mode::imp, 3, 1, 0, true);
        set(0x0B, Op::phd, Shape::stack, Mode::imp, 4, 1, 0, true);
        set(0x4B, Op::phk, Shape::stack, Mode::imp, 3, 1, 0, true);
        set(0x68, Op::pla, Shape::stack, Mode::imp, 4, 1, P_M);
        set(0x28, Op::plp, Shape::stack, Mode::imp, 4, 1, 0, true);
        set(0xFA, Op::plx, Shape::stack, Mode::imp, 4, 1, P_X);
        set(0x7A, Op::ply, Shape::stack, Mode::imp, 4, 1, P_X);
        set(0xAB, Op::plb, Shape::stack, Mode::imp, 4, 1, 0);
        set(0x2B, Op::pld, Shape::stack, Mode::imp, 5, 1, 0);
        set(0xF4, Op::pea, Shape::stack, Mode::abs, 5, 3, 0, true);
        set(0xD4, Op::pei, Shape::stack, Mode::dp, 6, 2, P_DP, true);
        set(0x62, Op::per, Shape::stack, Mode::rell, 6, 3, 0, true);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x18, Op::clc}, {0x38, Op::sec}, {0x58, Op::cli}, {0x78, Op::sei},
                 {0xB8, Op::clv}, {0xD8, Op::cld}, {0xF8, Op::sed}, {0xEA, Op::nop}})
            set(code, op, Shape::flag, Mode::imp, 2, 1, 0, true);
        set(0xC2, Op::rep, Shape::sep, Mode::imm, 3, 2, 0, true);
        set(0xE2, Op::sep, Shape::sep, Mode::imm, 3, 2, 0, true);
        set(0xFB, Op::xce, Shape::xce, Mode::imp, 2, 1, 0, true);
        set(0xEB, Op::xba, Shape::xba, Mode::imp, 3, 1, 0);
        set(0x42, Op::wdm, Shape::imp, Mode::imp, 2, 2, 0, true);
        set(0xCB, Op::wai, Shape::wai, Mode::imp, 3, 1, 0, true);
        set(0xDB, Op::stp, Shape::stp, Mode::imp, 3, 1, 0, true);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0xAA, Op::tax}, {0xA8, Op::tay}, {0xBA, Op::tsx}, {0x9A, Op::txs},
                 {0x5B, Op::tcd}, {0x7B, Op::tdc}, {0x1B, Op::tcs}, {0x3B, Op::tsc},
                 {0x9B, Op::txy}, {0xBB, Op::tyx}})
            set(code, op, Shape::imp, Mode::imp, 2, 1, 0, true);
        set(0x8A, Op::txa, Shape::imp, Mode::imp, 2, 1, 0);
        set(0x98, Op::tya, Shape::imp, Mode::imp, 2, 1, 0);
        return v;
    }();
    return t;
}

bool is_branch(Op op, Mode m) { return op == Op::jmp && m == Mode::rel; }

std::string hex(unsigned v, int digits = 4) {
    char b[16];
    snprintf(b, sizeof b, "0x%0*X", digits, v);
    return b;
}
std::string hex24(uint32_t v) { return hex(v, 6); }
std::string hex64(uint64_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%010llXULL", (unsigned long long)v);
    return b;
}

/* one instruction instance: a 24-bit address under one context */
struct Key {
    uint32_t pc;
    uint64_t ctx;
    bool operator<(const Key& o) const { return pc != o.pc ? pc < o.pc : ctx < o.ctx; }
};

struct Ins {
    uint32_t addr = 0;
    uint64_t ctx = 0;
    uint8_t code = 0;
    uint8_t o[3] = {0};
    OpInfo info;
    unsigned len = 1;
    uint32_t next = 0;
    uint32_t target = 0;   /* branch, JMP/JML/JSR/JSL's operand, BRL, PER */
    bool has_target = false;
    bool falls = true;
    bool breaks = false;   /* the walk could not prove what follows runs under */
    uint64_t follow_ctx = 0; /* the context what follows runs under (the walk's
        nctx, the instance's own on a break) */
    uint32_t hits = 0;
    bool dynamic = false;
    bool tracked_a = false; /* the walk knew A here: a TCD's page is provable */
    bool tracked_c = false; /* the walk knew the carry: an XCE is provable */
    bool label = false;
    int region = -1;
    unsigned local = 0;
    unsigned global = 0;
};

/* the walk's register knowledge: the accumulator (16 bits, immediates
 * only), the carry (CLC/SEC and the SEP/REP bits), the rest lives in the
 * context's own bytes */
struct Seed {
    uint32_t pc;
    uint64_t ctx;
    bool a_known;
    uint16_t a_val;
    bool c_known;
    uint8_t c_val;
};

/* the context's bytes: K, DBR, D lo, D hi, then M (bit 0), X (bit 1),
 * E (bit 2) */
constexpr unsigned ctx_k(uint64_t c) { return unsigned(c & 0xFF); }
constexpr unsigned ctx_dbr(uint64_t c) { return unsigned((c >> 8) & 0xFF); }
constexpr unsigned ctx_d(uint64_t c) { return unsigned((c >> 16) & 0xFFFF); }
constexpr bool ctx_m(uint64_t c) { return c & (1ULL << 32); }
constexpr bool ctx_x(uint64_t c) { return c & (2ULL << 32); }
constexpr bool ctx_e(uint64_t c) { return c & (4ULL << 32); }
constexpr uint64_t ctx_with_k(uint64_t c, unsigned k) { return (c & ~0xFFULL) | k; }
constexpr uint64_t ctx_with_d(uint64_t c, unsigned d) {
    return (c & ~(0xFFFFULL << 16)) | (uint64_t)(d & 0xFFFF) << 16;
}

class Emitter {
public:
    Emitter(const std::vector<uint8_t>& rom, const EmitOptions& o) : rom_(rom), opt_(o) {}
    std::string run(const std::vector<CoverageEntry>& coverage, EmitStats& stats);

private:
    const std::vector<uint8_t>& rom_;
    EmitOptions opt_;
    std::map<Key, Ins> ins_;
    std::vector<std::vector<Key>> regions_;

    /* the cartridge's window: the same decode as the runtime's bus, a
     * byte only where the file backs the address */
    std::optional<uint8_t> rom_at(uint32_t at) const {
        const unsigned bank = at >> 16, off = at & 0xFFFF;
        if (bank == 0x7E || bank == 0x7F) return std::nullopt;
        if (off < 0x2000) return std::nullopt;                    /* the WRAM mirror */
        if (off < 0x8000) return std::nullopt;                    /* I/O and SRAM */
        if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0) {
            if (!opt_.hirom) return std::nullopt;                 /* LoROM: open bus */
        }
        const uint32_t fo = opt_.hirom ? (uint32_t)(bank & 0x3F) * 0x10000u + off
                                       : (uint32_t)(bank & 0x3F) * 0x8000u + (off & 0x7FFFu);
        if (fo >= rom_.size()) return std::nullopt;
        return rom_[fo];
    }
    std::optional<uint32_t> rom_word(uint32_t at) const {
        const auto lo = rom_at(at);
        const auto hi = rom_at(at + 1);
        if (!lo || !hi) return std::nullopt;
        return uint32_t(*lo | *hi << 8);
    }

    bool decode(uint32_t at, uint64_t ctx, Ins& in) const;
    void select(const std::vector<CoverageEntry>& coverage, EmitStats& stats);
    void plan();
    void emit_region(std::ostream& o, int r);
    void emit_ins(std::ostream& o, const Ins& in);
    std::string go(const Ins& from, uint32_t to24) const;
    std::string ea(const Ins& in, std::string& pre) const;
    std::string operand(const Ins& in, std::string& pre) const;
    std::string cross(const Ins& in, const char* base, const char* idx) const;
    unsigned cycles(const Ins& in) const;
    bool may_io(const Ins& in) const;
    unsigned width_a(const Ins& in) const { return ctx_m(in.ctx) ? 8 : 16; }
    unsigned width_x(const Ins& in) const { return ctx_x(in.ctx) ? 8 : 16; }
};

bool Emitter::decode(uint32_t at, uint64_t ctx, Ins& in) const {
    const auto op_b = rom_at(at);
    if (!op_b) return false;
    in = Ins{};
    in.addr = at;
    in.ctx = ctx;
    in.code = *op_b;
    in.info = table()[in.code];
    if (in.info.shape == Shape::none) return false;
    in.len = in.info.len;
    /* the immediate's width is a context fact: the A operand's and BIT's
     * follow M, the index registers' and the compares' follow X */
    if (in.info.mode == Mode::imm &&
        (in.info.shape == Shape::load || in.info.shape == Shape::cmp ||
         in.info.shape == Shape::logic || in.info.shape == Shape::arith ||
         in.info.op == Op::bit)) {
        const bool xop = in.info.op == Op::ldx || in.info.op == Op::ldy ||
                         in.info.op == Op::cpx || in.info.op == Op::cpy;
        in.len = (xop ? ctx_x(ctx) : ctx_m(ctx)) ? 2 : 3;
    }
    for (unsigned i = 1; i < in.len; i++) {
        const auto b = rom_at(at + i);
        if (!b) return false;
        in.o[i - 1] = *b;
    }
    in.next = at + in.len;
    const unsigned w = in.o[0] | in.o[1] << 8;
    const Op op = in.info.op;
    if (op == Op::jmp && in.info.mode == Mode::rel) {
        in.target = at + 2 + (uint32_t)(int8_t)in.o[0];
        in.has_target = true;
        if (in.code == 0x80) in.falls = false; /* BRA is unconditional */
    } else if (op == Op::jmp && in.info.mode == Mode::rell) { /* BRL */
        in.target = at + 3 + (uint32_t)(int16_t)w;
        in.has_target = true;
        in.falls = false;
    } else if (op == Op::jmp && in.info.mode == Mode::abs) {
        in.target = (at & 0xFF0000u) | w;
        in.has_target = true;
        in.falls = false;
    } else if (op == Op::jmp && in.info.mode == Mode::indlp) {
        in.falls = false;
    } else if (op == Op::jml) {
        in.target = (uint32_t)in.o[2] << 16 | w;
        in.has_target = true;
        in.falls = false;
    } else if (op == Op::jsr && in.info.mode == Mode::abs) {
        in.target = (at & 0xFF0000u) | w;
        in.has_target = true; /* falls: the return site */
    } else if (op == Op::jsl) {
        in.target = (uint32_t)in.o[2] << 16 | w;
        in.has_target = true; /* falls: the return site */
    } else if (op == Op::rts || op == Op::rtl || op == Op::rti || op == Op::brk || op == Op::cop ||
               (op == Op::jmp && (in.info.mode == Mode::ind || in.info.mode == Mode::absx)) ||
               (op == Op::jsr && in.info.mode == Mode::absx)) {
        in.falls = false; /* the dispatcher looks the landing up */
    }
    return true;
}

void Emitter::select(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    /* every reachable instance, the played ones first: the coverage's
     * contexted addresses, then the reset vector and what it reaches */
    std::vector<CoverageEntry> hot(coverage);
    std::stable_sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.hits > b.hits; });
    std::deque<Seed> work;
    std::vector<Key> order;
    auto visit = [&](uint32_t at, uint64_t ctx, bool a_known, uint16_t a_val, bool c_known, uint8_t c_val) {
        const Key key{at, ctx};
        if (ins_.count(key)) return;
        Ins in;
        if (!decode(at, ctx, in)) return;
        ins_[key] = in;
        order.push_back(key);
        work.push_back({at, ctx, a_known, a_val, c_known, c_val});
    };
    for (const auto& e : hot)
        for (uint64_t ctx : e.contexts) visit(e.pc, ctx, false, 0, false, 0);
    const uint64_t reset_ctx = 7ULL << 32; /* K, DBR, D at 0, E with its 8-bit widths */
    {
        const auto lo = rom_at(0x00FFFC);
        const auto hi = rom_at(0x00FFFD);
        if (lo && hi) {
            const uint32_t pc24 = uint32_t(*lo | *hi << 8);
            visit(pc24, reset_ctx, false, 0, false, 0);
        }
    }
    const size_t covered = order.size();
    while (!work.empty()) {
        const Seed s = work.front();
        work.pop_front();
        Ins& in = ins_[Key{s.pc, s.ctx}];
        in.tracked_a = s.a_known;
        in.tracked_c = s.c_known;
        uint64_t nctx = s.ctx;
        bool na_known = false;
        uint16_t nav = s.a_val;
        bool nc_known = s.c_known;
        uint8_t ncv = s.c_val;
        bool brk = false;
        const unsigned w16 = in.o[0] | in.o[1] << 8;
        switch (in.info.op) {
        case Op::lda:
            if (in.info.mode == Mode::imm) {
                if (ctx_m(s.ctx)) {
                    if (s.a_known) { na_known = true; nav = (uint16_t)((s.a_val & 0xFF00u) | in.o[0]); }
                } else {
                    na_known = true;
                    nav = (uint16_t)w16;
                }
            } else na_known = false;
            break;
        case Op::cmp: case Op::cpx: case Op::cpy: case Op::bit:
        case Op::tax: case Op::tay: case Op::tsx: case Op::txs: case Op::tcs:
        case Op::inx: case Op::dex: case Op::iny: case Op::dey:
        case Op::ldx: case Op::ldy: case Op::sta: case Op::stx: case Op::sty: case Op::stz:
        case Op::clc:
            nc_known = true; ncv = 0; na_known = s.a_known;
            break;
        case Op::sec:
            nc_known = true; ncv = 1; na_known = s.a_known;
            break;
        case Op::cli: case Op::sei: case Op::clv:
        case Op::cld: case Op::sed: case Op::nop: case Op::wdm:
        case Op::pha: case Op::php: case Op::phx: case Op::phy: case Op::phb:
        case Op::phd: case Op::phk: case Op::pea: case Op::pei: case Op::per:
        case Op::tsb: case Op::trb: case Op::asl: case Op::lsr: case Op::rol: case Op::ror:
        case Op::inc: case Op::dec:
            na_known = s.a_known;
            break;
        case Op::ora: case Op::and_: case Op::eor: case Op::adc: case Op::sbc:
        case Op::pla: case Op::txa: case Op::tya: case Op::tsc:
        case Op::ina: case Op::dea: case Op::xba:
            na_known = false;
            break;
        case Op::tdc: /* A := D, and D is a context constant */
            na_known = true;
            nav = (uint16_t)ctx_d(s.ctx);
            break;
        case Op::mvn: case Op::mvp: /* the block move's done mark */
            na_known = true;
            nav = 0xFFFF;
            brk = true; /* destination bank changes DBR; recheck the context */
            break;
        case Op::tcd:
            if (s.a_known) {
                nctx = ctx_with_d(nctx, s.a_val);
                na_known = true;
            } else brk = true;
            break;
        case Op::plb: case Op::pld:
            brk = true;
            break;
        case Op::plp:
            if (!(ctx_e(s.ctx) && s.c_known)) brk = true; /* the widths may change */
            nc_known = false; /* the carry comes off the stack */
            break;
        case Op::xce:
            if (s.c_known) {
                const unsigned e_old = ctx_e(s.ctx) ? 1 : 0;
                const unsigned e_new = s.c_val ? 1 : 0;
                /* E' = the carry; entering the emulation forces M and X,
                 * leaving it keeps the widths' last value */
                const unsigned flags = e_new ? 0x07u
                                             : (ctx_m(s.ctx) ? 1u : 0u) | (ctx_x(s.ctx) ? 2u : 0u);
                nctx = (s.ctx & ~(0xFFULL << 32)) | (uint64_t)flags << 32;
                nc_known = true;
                ncv = (uint8_t)e_old;
            } else {
                brk = true;
                nc_known = false;
            }
            break;
        case Op::sep: {
            const unsigned imm = in.o[0];
            unsigned flags = unsigned((s.ctx >> 32) & 0xFF);
            if (imm & 0x01) { nc_known = true; ncv = 1; }
            /* P's M (0x20) and X (0x10) land on the context's bits 0 and 1 */
            flags |= ((imm & 0x20) ? 1u : 0u) | ((imm & 0x10) ? 2u : 0u);
            if (ctx_e(s.ctx)) flags |= 0x03;
            nctx = (nctx & ~(0xFFULL << 32)) | (uint64_t)flags << 32;
            break;
        }
        case Op::rep: {
            const unsigned imm = in.o[0];
            unsigned flags = unsigned((s.ctx >> 32) & 0xFF);
            if (imm & 0x01) { nc_known = true; ncv = 0; }
            flags &= ~((imm & 0x20) ? 1u : 0u) & ~((imm & 0x10) ? 2u : 0u);
            if (ctx_e(s.ctx)) flags |= 0x03;
            nctx = (nctx & ~(0xFFULL << 32)) | (uint64_t)flags << 32;
            break;
        }
        case Op::jml: case Op::jmp:
            if (in.info.mode == Mode::indlp || in.info.mode == Mode::long_)
                nctx = ctx_with_k(nctx, in.o[2]);
            break;
        case Op::jsl:
            nctx = ctx_with_k(nctx, in.o[2]);
            break;
        default:
            break;
        }
        in.breaks = brk;
        if (getenv("SNES_EMIT_DEBUG"))
            fprintf(stderr, "walk %06X ctx=%012llX op=%d brk=%d falls=%d -> next=%06X nctx=%012llX\n",
                    s.pc, (unsigned long long)s.ctx, (int)in.info.op, brk, in.falls, in.next, (unsigned long long)nctx);
        uint64_t fctx = nctx;
        /* a break hands the mapping question back to the dispatcher:
         * what follows is visited under the context it was decoded
         * with, and entered only where the machine still agrees */
        if (brk) fctx = s.ctx;
        in.follow_ctx = fctx;
        if (in.falls) {
            visit(in.next, fctx, brk ? false : na_known, nav, brk ? false : nc_known, ncv);
        }
        if (in.has_target) {
            uint64_t tctx = nctx;
            if (in.info.op == Op::jml || in.info.op == Op::jsl ||
                (in.info.op == Op::jmp && in.info.mode == Mode::indlp))
                tctx = ctx_with_k(nctx, in.o[2]);
            visit(in.target, tctx, brk ? false : na_known, nav, brk ? false : nc_known, ncv);
        }
    }
    const size_t keep = opt_.max_instructions ? std::min(order.size(), opt_.max_instructions) : order.size();
    for (size_t i = 0; i < keep; i++) ins_.at(order[i]).label = false;
    /* the profile: exact contexts attach to their instance, context-less
     * addresses rank and label every instance found there */
    std::map<Key, const CoverageEntry*> exact;
    std::map<uint32_t, std::vector<const CoverageEntry*>> loose;
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
    auto mark = [&](uint32_t at, uint64_t ctx) {
        if (auto p = ins_.find(Key{at, ctx}); p != ins_.end()) p->second.label = true;
    };
    const uint64_t reset_ctx = 7ULL << 32;
    {
        const auto lo = rom_at(0x00FFFC);
        const auto hi = rom_at(0x00FFFD);
        if (lo && hi) mark(uint32_t(*lo | *hi << 8), reset_ctx);
    }
    for (auto& [key, in] : ins_) {
        if (opt_.precise || in.dynamic) in.label = true;
        if (in.has_target) mark(in.target, [&] {
            uint64_t c = key.ctx;
            if (in.info.op == Op::jml || in.info.op == Op::jsl ||
                (in.info.op == Op::jmp && in.info.mode == Mode::indlp))
                c = ctx_with_k(c, in.o[2]);
            return c;
        }());
        const Op op = in.info.op;
        /* the instruction after an exit resumes there: a return site, a
         * software interrupt's signature, the end of a sleep */
        if (op == Op::jsr || op == Op::jsl || op == Op::brk || op == Op::cop ||
            op == Op::wai || op == Op::stp)
            mark(in.next, key.ctx);
    }
    /* regions: runs of neighbouring instructions under one context — a
     * mapping the walk could not prove (PLB, PLD, an unknown TCD or XCE,
     * a native PLP) ends its run */
    const size_t cap = 256;
    for (auto& [key, in] : ins_) {
        const bool joins = !regions_.empty() && regions_.back().size() < cap &&
                           [&] {
                               const Ins& back = ins_.at(regions_.back().back());
                               return back.falls && !back.breaks && back.next == key.pc &&
                                      back.follow_ctx == key.ctx;
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
                run += cycles(in) + 1;
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
}

/* where control goes to reach `to24` from the from-instance's context
 * (a long target changes K, so its own context) */
std::string Emitter::go(const Ins& from, uint32_t to24) const {
    /* the landing runs under the from-instance's resulting context (a SEP
     * or XCE changes it): the walk's follow_ctx, not the instance's own */
    uint64_t ctx = from.follow_ctx;
    const Ins* src = &from;
    if ((to24 >> 16) != ctx_k(from.follow_ctx)) {
        ctx = ctx_with_k(from.follow_ctx, to24 >> 16);
        src = nullptr; /* the K write is the caller's job */
    }
    auto p = ins_.find(Key{to24, ctx});
    const bool bank = (to24 >> 16) != ctx_k(from.ctx);
    const std::string set = bank ? "{ k_ = " + hex(to24 >> 16, 2) + "; pc_ = " + hex(to24 & 0xFFFF) + "; "
                                 : "{ pc_ = " + hex(to24) + "; ";
    if (p == ins_.end() || !p->second.label) return set + "goto out; }";
    if (src && p->second.region == from.region) {
        char b[24];
        snprintf(b, sizeof b, "goto L_0x%06X;", to24);
        return b;
    }
    return set + "ni = " + std::to_string(p->second.global) + "; goto chain; }";
}

/* the operand's address as C, computed once into `pre`'s locals when the
 * addressing goes through the direct page or the stack */
std::string Emitter::ea(const Ins& in, std::string& pre) const {
    const unsigned d = ctx_d(in.ctx);
    const unsigned dbr = ctx_dbr(in.ctx);
    const bool xf = ctx_x(in.ctx);
    const char* xi = xf ? "(x_ & 0xFF)" : "x_";
    const char* yi = xf ? "(y_ & 0xFF)" : "y_";
    switch (in.info.mode) {
    case Mode::dp:
        return hex24((d + in.o[0]) & 0xFFFF);
    case Mode::dpx:
        return (ctx_e(in.ctx) && !(d & 0xff)) ? "(uint32_t)(uint16_t)(" + hex(d) + "u + (uint8_t)(" + hex(in.o[0], 2) + "u + x_))"
                  : "(uint32_t)(uint16_t)(" + hex(d) + "u + " + hex(in.o[0], 2) + "u + x_)";
    case Mode::dpy:
        return (ctx_e(in.ctx) && !(d & 0xff)) ? "(uint32_t)(uint16_t)(" + hex(d) + "u + (uint8_t)(" + hex(in.o[0], 2) + "u + y_))"
                  : "(uint32_t)(uint16_t)(" + hex(d) + "u + " + hex(in.o[0], 2) + "u + y_)";
    case Mode::ind:
        pre += "const unsigned w_ = SNES_RD(" + hex24((d + in.o[0]) & 0xFFFF) + "u) | SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 1)) + "u) << 8; ";
        return hex24((uint32_t)dbr << 16) + "u | w_";
    case Mode::indx: {
        const std::string at = (ctx_e(in.ctx) && !(d & 0xff))
            ? "(uint32_t)(uint16_t)(" + hex(d) + "u + (uint8_t)(" + hex(in.o[0], 2) + "u + x_))"
            : "(uint32_t)(uint16_t)(" + hex(d) + "u + " + hex(in.o[0], 2) + "u + x_)";
        pre += "const unsigned w_ = SNES_RD(" + at + ") | SNES_RD((uint32_t)(uint16_t)(" + at + " + 1u)) << 8; ";
        return hex24((uint32_t)dbr << 16) + "u | w_";
    }
    case Mode::indy:
        pre += "const unsigned w_ = SNES_RD(" + hex24((d + in.o[0]) & 0xFFFF) + "u) | SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 1)) + "u) << 8; ";
        return hex24((uint32_t)dbr << 16) + "u | (uint16_t)(w_ + y_)";
    case Mode::indlp:
        pre += "const uint32_t p24_ = (uint32_t)SNES_RD(" + hex24((d + in.o[0]) & 0xFFFF) + "u) | (uint32_t)SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 1)) + "u) << 8 | (uint32_t)SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 2)) + "u) << 16; ";
        return "p24_";
    case Mode::indlpy:
        pre += "const uint32_t p24_ = (uint32_t)SNES_RD(" + hex24((d + in.o[0]) & 0xFFFF) + "u) | (uint32_t)SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 1)) + "u) << 8 | (uint32_t)SNES_RD(" +
               hex24((uint16_t)(d + in.o[0] + 2)) + "u) << 16; ";
        return "(p24_ + y_)";
    case Mode::abs: {
        const uint32_t at = (uint32_t)dbr << 16 | in.o[0] | in.o[1] << 8;
        return hex24(at);
    }
    case Mode::absx:
        return "(uint32_t)" + hex((uint32_t)dbr << 16) + "u | (uint16_t)(" + hex(in.o[0] | in.o[1] << 8) + "u + " + xi + ")";
    case Mode::absy:
        return "(uint32_t)" + hex((uint32_t)dbr << 16) + "u | (uint16_t)(" + hex(in.o[0] | in.o[1] << 8) + "u + " + yi + ")";
    case Mode::long_:
        return hex24(in.o[0] | in.o[1] << 8 | in.o[2] << 16);
    case Mode::longx:
        return "(" + hex24(in.o[0] | in.o[1] << 8 | in.o[2] << 16) + "u + " + xi + ")";
    case Mode::longy:
        return "(" + hex24(in.o[0] | in.o[1] << 8 | in.o[2] << 16) + "u + " + yi + ")";
    case Mode::sr:
        return "(uint32_t)(uint16_t)(s_ + " + hex(in.o[0], 2) + "u)";
    case Mode::sry:
        pre += "const unsigned w_ = SNES_RD((uint32_t)(uint16_t)(s_ + " + hex(in.o[0], 2) + "u)) | SNES_RD((uint32_t)(uint16_t)(s_ + " +
               hex((uint8_t)(in.o[0] + 1), 2) + "u)) << 8; ";
        return hex24((uint32_t)dbr << 16) + "u | (uint16_t)(w_ + y_)";
    default:
        return "0";
    }
}

/* the operand's value as C: immediates and ROM-backed constant addresses
 * are constants, the rest goes through the bus as the interpreter sees it */
std::string Emitter::operand(const Ins& in, std::string& pre) const {
    /* 16 bits when the width flag is CLEAR (the flag marks 8-bit mode) */
    const bool wide = in.info.op == Op::ldx || in.info.op == Op::ldy || in.info.op == Op::cpx ||
                      in.info.op == Op::cpy ? !ctx_x(in.ctx) : !ctx_m(in.ctx);
    const auto fold = [&](uint32_t at) -> std::string {
        if (wide) {
            const auto w = rom_word(at);
            if (!w) return std::string();
            /* the folded read never touches the bus, so the MDR's last
             * byte comes from the read itself, as the interpreter's rd() */
            pre += "machine.cpu.mdr = " + hex(*w >> 8 & 0xFF, 2) + "; ";
            return hex(*w, 4) + "u";
        }
        const auto b = rom_at(at);
        if (!b) return std::string();
        pre += "machine.cpu.mdr = " + hex(*b, 2) + "; ";
        return hex(*b, 2) + "u";
    };
    switch (in.info.mode) {
    case Mode::imm:
        return wide ? hex(in.o[0] | in.o[1] << 8, 4) + "u" : hex(in.o[0], 2) + "u";
    case Mode::dp: {
        const uint32_t at = (ctx_d(in.ctx) + in.o[0]) & 0xFFFF;
        if (const std::string f = fold(at); !f.empty()) return f;
        return wide ? "SNES_RDW(" + hex24(at) + "u)" : "SNES_RD(" + hex24(at) + "u)";
    }
    case Mode::abs: {
        const uint32_t at = (uint32_t)ctx_dbr(in.ctx) << 16 | in.o[0] | in.o[1] << 8;
        if (const std::string f = fold(at); !f.empty()) return f;
        return wide ? "SNES_RDW(" + hex24(at) + "u)" : "SNES_RD(" + hex24(at) + "u)";
    }
    case Mode::long_: {
        const uint32_t at = in.o[0] | in.o[1] << 8 | in.o[2] << 16;
        if (const std::string f = fold(at); !f.empty()) return f;
        return wide ? "SNES_RD24(" + hex24(at) + "u)" : "SNES_RD(" + hex24(at) + "u)";
    }
    case Mode::longx: {
        /* long,X reaches a runtime address; its 16-bit read carries into the
         * next bank, as the interpreter's RD24 */
        return wide ? "SNES_RD24(" + ea(in, pre) + ")" : "SNES_RD(" + ea(in, pre) + ")";
    }
    default: {
        const std::string at = ea(in, pre);
        return wide ? "SNES_RDW(" + at + ")" : "SNES_RD(" + at + ")";
    }
    }
}

/* the page-cross penalty of an indexed read, as the interpreter's CROSS:
 * one cycle when the 8-bit index crosses the page, one always in 16-bit
 * index mode */
std::string Emitter::cross(const Ins& in, const char* base, const char* idx) const {
    if (!ctx_x(in.ctx)) return "budget -= 1;";
    return std::string("budget -= ((") + base + " & 0xFFu) + " + idx + " > 0xFFu) ? 1 : 0;";
}

unsigned Emitter::cycles(const Ins& in) const {
    unsigned c = in.info.base;
    if (in.info.pen & P_M) c += ctx_m(in.ctx) ? 0 : 1;
    if (in.info.pen & P_X) c += ctx_x(in.ctx) ? 0 : 1;
    if (in.info.pen & P_DP) c += (ctx_d(in.ctx) & 0xFF) ? 1 : 0;
    return c;
}

/* precise mode: true when the instruction's accesses may land on the I/O
 * window or an indirect address, so the region leaves and the dispatcher
 * samples the lines where the interpreter would */
bool Emitter::may_io(const Ins& in) const {
    switch (in.info.shape) {
    case Shape::stack: return true; /* the stack's window is S's, unknown here */
    case Shape::block: case Shape::wai: case Shape::stp: return true;
    case Shape::flow: {
        const Op op = in.info.op;
        if (op == Op::brk || op == Op::cop) return true;
        if (op == Op::jmp && (in.info.mode == Mode::ind || in.info.mode == Mode::absx)) return true;
        if (op == Op::jsr && in.info.mode == Mode::absx) return true;
        return false;
    }
    case Shape::load: case Shape::cmp: case Shape::bit:
        switch (in.info.mode) {
        case Mode::imm: return false;
        case Mode::dp: {
            const unsigned at = (ctx_d(in.ctx) + in.o[0]) & 0xFFFF;
            return at >= 0x2000 && at < 0x6000;
        }
        case Mode::dpx: case Mode::dpy: return true;
        case Mode::abs: {
            const unsigned off = in.o[0] | in.o[1] << 8;
            return off >= 0x2000 && off < 0x6000;
        }
        case Mode::absx: case Mode::absy: {
            const unsigned off = in.o[0] | in.o[1] << 8;
            const unsigned lo = off, hi = std::min(off + 0xFFu, 0xFFFFu);
            return lo <= 0x5FFFu && hi >= 0x2000u;
        }
        default: return true; /* pointed and long forms reach anywhere */
        }
    case Shape::logic: case Shape::arith:
        switch (in.info.mode) {
        case Mode::imm: return false;
        case Mode::dp: {
            const unsigned at = (ctx_d(in.ctx) + in.o[0]) & 0xFFFF;
            return at >= 0x2000 && at < 0x6000;
        }
        case Mode::abs: {
            const unsigned off = in.o[0] | in.o[1] << 8;
            return off >= 0x2000 && off < 0x6000;
        }
        default: return true;
        }
    case Shape::store: case Shape::shift: case Shape::incdec: case Shape::tsbtrb:
        switch (in.info.mode) {
        case Mode::dp: {
            const unsigned at = (ctx_d(in.ctx) + in.o[0]) & 0xFFFF;
            return at >= 0x2000 && at < 0x6000;
        }
        case Mode::abs: {
            const unsigned off = in.o[0] | in.o[1] << 8;
            return off >= 0x2000 && off < 0x6000;
        }
        default: return true;
        }
    default: return false;
    }
}

void Emitter::emit_ins(std::ostream& o, const Ins& in) {
    const Op op = in.info.op;
    const Mode m = in.info.mode;
    const bool mf = ctx_m(in.ctx), xf = ctx_x(in.ctx), ef = ctx_e(in.ctx);
    const std::string next = go(in, in.next);
    std::string pre, body;
    unsigned cyc = cycles(in);
    std::string after;
    /* the fetch: the interpreter's bus reads update the MDR byte for byte
     * (the opcode, then the operands; BRK and COP never fetch their
     * signature byte).  The generated operands are constants, so the
     * fetches shrink to this one store — the open-bus reads read back
     * exactly what the interpreter's would. */
    const unsigned fetched = op == Op::brk || op == Op::cop || in.len == 1
                                 ? in.code : in.o[in.len - 2];
    o << "    machine.cpu.mdr = " << hex(fetched, 2) << ";\n";
    auto store = [&](const std::string& value) {
        std::string pre2;
        const std::string at = ea(in, pre2);
        if (!pre2.empty()) pre += pre2;
        const bool wide = (op == Op::sta || op == Op::stz) ? !mf : !xf;
        if (!wide) return "SNES_WR(" + at + ", (uint8_t)(" + value + "));";
        /* the long forms' second byte carries into the next bank, as the
         * interpreter's WR24 */
        return m == Mode::long_ || m == Mode::longx
                   ? "SNES_WR24(" + at + ", (uint16_t)(" + value + "));"
                   : "SNES_WRW(" + at + ", (uint16_t)(" + value + "));";
    };
    auto rmw_at = [&](std::string& expr8, std::string& expr16) {
        const std::string at = ea(in, pre);
        expr8 = "{ const uint32_t e_ = " + at + "; const uint8_t r_ = ";
        expr16 = "{ const uint32_t e_ = " + at + "; const uint16_t r_ = ";
    };
    switch (in.info.shape) {
    case Shape::load: {
        const char* reg = op == Op::lda ? "a_" : op == Op::ldx ? "x_" : "y_";
        const bool wide = op == Op::lda ? !mf : !xf;
        std::string val = operand(in, pre);
        if (op == Op::lda)
            body = wide ? "SNES_WR_A16(" + val + ");" : "SNES_WR_A8(" + val + ");";
        else
            body = std::string(reg == "x_" ? "SNES_WR_X(" : "SNES_WR_Y(") + val + ");";
        break;
    }
    case Shape::store:
        body = store(op == Op::sta ? "a_" : op == Op::stx ? "x_" : op == Op::sty ? "y_" : "0");
        break;
    case Shape::logic: {
        const char* tok = op == Op::and_ ? "&" : op == Op::ora ? "|" : "^";
        const std::string val = operand(in, pre);
        body = mf ? "SNES_WR_A8((uint8_t)(a_ " + std::string(tok) + " " + val + "));"
                  : "SNES_WR_A16((uint16_t)(a_ " + std::string(tok) + " " + val + "));";
        break;
    }
    case Shape::arith:
        body = std::string(mf ? (op == Op::adc ? "SNES_ADC8(" : "SNES_SBC8(")
                              : (op == Op::adc ? "SNES_ADC16(" : "SNES_SBC16(")) +
              operand(in, pre) + ");";
        break;
    case Shape::cmp: {
        const char* reg = op == Op::cmp ? "a_" : op == Op::cpx ? "x_" : "y_";
        const bool wide = op == Op::cmp ? !mf : !xf;
        body = std::string(wide ? "SNES_CMP16(" : "SNES_CMP8(") + reg + ", " + operand(in, pre) + ");";
        break;
    }
    case Shape::bit:
        if (m == Mode::imm)
            body = "p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((a_ & " + operand(in, pre) + ") ? 0 : SNES_P_Z));";
        else
            body = "SNES_BIT(" + operand(in, pre) + ", " + (mf ? "0" : "1") + ");";
        break;
    case Shape::shift: {
        const char* f8 = op == Op::asl ? "SNES_ASL8" : op == Op::lsr ? "SNES_LSR8" : op == Op::rol ? "SNES_ROL8" : "SNES_ROR8";
        const char* f16 = op == Op::asl ? "SNES_ASL16" : op == Op::lsr ? "SNES_LSR16" : op == Op::rol ? "SNES_ROL16" : "SNES_ROR16";
        if (m == Mode::imp) { /* the accumulator */
            body = mf ? "{ const uint8_t r_ = " + std::string(f8) + "(a_); a_ = (uint16_t)((a_ & 0xFF00u) | r_); SNES_ZN8(r_); }"
                      : "{ a_ = " + std::string(f16) + "(a_); SNES_ZN16(a_); }";
            break;
        }
        const std::string at = ea(in, pre);
        body = mf ? "{ const uint32_t e_ = " + at + "; const uint8_t r_ = " + std::string(f8) +
                        "(SNES_RD(e_)); SNES_WR(e_, r_); SNES_ZN8(r_); }"
                  : "{ const uint32_t e_ = " + at + "; const uint16_t r_ = " + std::string(f16) +
                        "(SNES_RDW(e_)); SNES_WRW(e_, r_); SNES_ZN16(r_); }";
        break;
    }
    case Shape::incdec: {
        const std::string at = ea(in, pre);
        const char* aop = op == Op::inc ? "+" : "-";
        body = mf ? "{ const uint32_t e_ = " + at + "; const uint8_t r_ = (uint8_t)(SNES_RD(e_) " + aop + " 1u); SNES_WR(e_, r_); SNES_ZN8(r_); }"
                  : "{ const uint32_t e_ = " + at + "; const uint16_t r_ = (uint16_t)(SNES_RDW(e_) " + aop + " 1u); SNES_WRW(e_, r_); SNES_ZN16(r_); }";
        break;
    }
    case Shape::tsbtrb: {
        const std::string at = ea(in, pre);
        if (op == Op::tsb)
            body = mf ? "{ const uint32_t e_ = " + at + "; const uint8_t m_ = SNES_RD(e_); "
                        "p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((m_ & a_) ? 0 : SNES_P_Z)); SNES_WR(e_, (uint8_t)(m_ | (uint8_t)a_)); }"
                      : "{ const uint32_t e_ = " + at + "; const uint16_t m_ = SNES_RDW(e_); "
                        "p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((m_ & a_) ? 0 : SNES_P_Z)); SNES_WRW(e_, (uint16_t)(m_ | a_)); }";
        else
            body = mf ? "{ const uint32_t e_ = " + at + "; const uint8_t m_ = SNES_RD(e_); "
                        "p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((m_ & a_) ? 0 : SNES_P_Z)); SNES_WR(e_, (uint8_t)(m_ & (uint8_t)~a_)); }"
                      : "{ const uint32_t e_ = " + at + "; const uint16_t m_ = SNES_RDW(e_); "
                        "p_ = (uint8_t)((p_ & ~SNES_P_Z) | ((m_ & a_) ? 0 : SNES_P_Z)); SNES_WRW(e_, (uint16_t)(m_ & (uint16_t)~a_)); }";
        break;
    }
    case Shape::imp:
        switch (op) {
        case Op::ina: body = mf ? "SNES_WR_A8((uint8_t)(a_ + 1));" : "SNES_WR_A16((uint16_t)(a_ + 1));"; break;
        case Op::dea: body = mf ? "SNES_WR_A8((uint8_t)(a_ - 1));" : "SNES_WR_A16((uint16_t)(a_ - 1));"; break;
        case Op::inx: body = "if (SNES_XF) { x_ = (uint16_t)((x_ + 1) & 0xFF); SNES_ZN8((uint8_t)x_); } else { x_++; SNES_ZN16(x_); }"; break;
        case Op::dex: body = "if (SNES_XF) { x_ = (uint16_t)((x_ - 1) & 0xFF); SNES_ZN8((uint8_t)x_); } else { x_--; SNES_ZN16(x_); }"; break;
        case Op::iny: body = "if (SNES_XF) { y_ = (uint16_t)((y_ + 1) & 0xFF); SNES_ZN8((uint8_t)y_); } else { y_++; SNES_ZN16(y_); }"; break;
        case Op::dey: body = "if (SNES_XF) { y_ = (uint16_t)((y_ - 1) & 0xFF); SNES_ZN8((uint8_t)y_); } else { y_--; SNES_ZN16(y_); }"; break;
        case Op::tax: body = "SNES_WR_X(SNES_MF ? (a_ & 0xFF) : a_);"; break;
        case Op::tay: body = "SNES_WR_Y(SNES_MF ? (a_ & 0xFF) : a_);"; break;
        case Op::txa: body = mf ? "SNES_WR_A8(x_);" : "{ a_ = x_; SNES_ZN16(a_); }"; break;
        case Op::tya: body = mf ? "SNES_WR_A8(y_);" : "{ a_ = y_; SNES_ZN16(a_); }"; break;
        case Op::tsx: body = "SNES_WR_X(s_);"; break;
        case Op::txs: body = "s_ = SNES_XF ? (uint16_t)((s_ & 0xFF00u) | (x_ & 0xFF)) : x_;"
                             " if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF));"; break;
        case Op::tcd: body = "{ d_ = a_; SNES_ZN16(d_); }"; break;
        case Op::tdc: body = "{ a_ = d_; SNES_ZN16(a_); }"; break;
        case Op::tcs: body = "{ s_ = a_; if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); }"; break;
        case Op::tsc: body = "{ a_ = s_; SNES_ZN16(a_); }"; break;
        case Op::txy: body = "SNES_WR_Y(x_);"; break;
        case Op::tyx: body = "SNES_WR_X(y_);"; break;
        case Op::wdm: body = ""; break;
        default: body = ""; break;
        }
        break;
    case Shape::flag:
        switch (op) {
        case Op::clc: body = "p_ &= (uint8_t)~SNES_P_C;"; break;
        case Op::sec: body = "p_ |= SNES_P_C;"; break;
        case Op::cli: body = "p_ &= (uint8_t)~SNES_P_I;"; break;
        case Op::sei: body = "p_ |= SNES_P_I;"; break;
        case Op::clv: body = "p_ &= (uint8_t)~SNES_P_V;"; break;
        case Op::cld: body = "p_ &= (uint8_t)~SNES_P_D;"; break;
        case Op::sed: body = "p_ |= SNES_P_D;"; break;
        default: body = ""; break;
        }
        break;
    case Shape::sep: {
        const unsigned imm = in.o[0];
        if (op == Op::sep) {
            body = "p_ |= " + hex(imm, 2) + "u;";
            if (ef) body += " p_ |= SNES_P_M | SNES_P_X;";
            if (imm & 0x10) body += " x_ &= 0xFF; y_ &= 0xFF;";
        } else {
            body = "p_ &= (uint8_t)~" + hex(imm, 2) + "u;";
            if (ef) body += " p_ |= SNES_P_M | SNES_P_X;";
        }
        break;
    }
    case Shape::xce:
        body = "{ const unsigned c_ = p_ & SNES_P_C;"
               " p_ = (uint8_t)((p_ & ~SNES_P_C) | (e_ ? SNES_P_C : 0));"
               " e_ = c_ ? 1 : 0;"
               " if (e_) { p_ |= SNES_P_M | SNES_P_X; x_ &= 0xFF; y_ &= 0xFF;"
               " s_ = (uint16_t)(0x0100u | (s_ & 0xFF)); } }";
        break;
    case Shape::xba:
        body = "{ a_ = (uint16_t)((a_ >> 8) | (a_ << 8)); SNES_ZN8((uint8_t)a_); }";
        break;
    case Shape::stack:
        switch (op) {
        case Op::pha:
            body = mf ? "SNES_PUSH((uint8_t)a_);"
                      : "SNES_PUSH((uint8_t)(a_ >> 8)); SNES_PUSH((uint8_t)a_);";
            break;
        case Op::phx: body = xf ? "SNES_PUSH((uint8_t)x_);" : "SNES_PUSH16(x_);"; break;
        case Op::phy: body = xf ? "SNES_PUSH((uint8_t)y_);" : "SNES_PUSH16(y_);"; break;
        case Op::php: body = "SNES_PUSH(p_);"; break; /* the register as it is */
        case Op::phb: body = "SNES_PUSH(dbr_);"; break;
        case Op::phd: body = "SNES_PUSH((uint8_t)(d_ >> 8)); SNES_PUSH((uint8_t)d_);"; break;
        case Op::phk: body = "SNES_PUSH(k_);"; break;
        case Op::pla:
            body = mf ? "SNES_WR_A8(SNES_POP());" : "{ a_ = (uint16_t)SNES_POP16(); SNES_ZN16(a_); }";
            break;
        case Op::plx: body = xf ? "SNES_WR_X(SNES_POP());" : "{ x_ = (uint16_t)SNES_POP16(); SNES_ZN16(x_); }"; break;
        case Op::ply: body = xf ? "SNES_WR_Y(SNES_POP());" : "{ y_ = (uint16_t)SNES_POP16(); SNES_ZN16(y_); }"; break;
        case Op::plp:
            body = "{ const uint8_t v_ = SNES_POP(); p_ = e_ ? (uint8_t)(v_ | 0x30u) : v_;"
                   " if (SNES_XF) { x_ &= 0xFF; y_ &= 0xFF; } }";
            break;
        case Op::plb: body = "{ dbr_ = SNES_POP(); SNES_ZN8((uint8_t)dbr_); }"; break;
        case Op::pld: body = "{ d_ = (uint16_t)SNES_POP16(); SNES_ZN16(d_); }"; break;
        case Op::pea: body = "SNES_PUSH16(" + hex(in.o[0] | in.o[1] << 8, 4) + "u);"; break;
        case Op::pei: {
            const unsigned d = ctx_d(in.ctx);
            body = "SNES_PUSH16(SNES_RD(" + hex24((d + in.o[0]) & 0xFFFF) + "u) | SNES_RD(" +
                   hex24((uint16_t)(d + in.o[0] + 1)) + "u) << 8);";
            break;
        }
        case Op::per:
            body = "SNES_PUSH16((uint16_t)(" + hex((in.addr + 3) & 0xFFFF) + "u + " +
                   hex(in.o[0] | in.o[1] << 8, 4) + "u));";
            break;
        default: body = ""; break;
        }
        break;
    case Shape::block: {
        const int back = op == Op::mvp;
        body = "{ const uint8_t db_ = " + hex(in.o[0], 2) + "u, sb_ = " + hex(in.o[1], 2) + "u;"
               " const uint32_t bytes_ = (uint32_t)a_ + 1u;"
               " for (uint32_t i_ = 0; i_ < bytes_; i_++) {"
               " const uint32_t s_at = (uint32_t)sb_ << 16 | (SNES_XF ? (uint8_t)(x_ " +
               std::string(back ? "- (uint16_t)i_" : "+ (uint16_t)i_") + ")"
               " : (uint16_t)(x_ " + std::string(back ? "- (uint16_t)i_" : "+ (uint16_t)i_") + "));"
               " const uint32_t d_at = (uint32_t)db_ << 16 | (SNES_XF ? (uint8_t)(y_ " +
               std::string(back ? "- (uint16_t)i_" : "+ (uint16_t)i_") + ")"
               " : (uint16_t)(y_ " + std::string(back ? "- (uint16_t)i_" : "+ (uint16_t)i_") + "));"
               " SNES_WR(d_at, SNES_RD(s_at)); }"
               " const uint16_t step_ = (uint16_t)(bytes_ & 0xFFFFu);"
               " if (SNES_XF) { x_ = (uint8_t)(x_ " + std::string(back ? "-" : "+") + " step_);"
               " y_ = (uint8_t)(y_ " + std::string(back ? "-" : "+") + " step_); }"
               " else { x_ = (uint16_t)(x_ " + std::string(back ? "-" : "+") + " step_);"
               " y_ = (uint16_t)(y_ " + std::string(back ? "-" : "+") + " step_); }"
               " a_ = 0xFFFF; dbr_ = db_;"
               " budget -= 7u * bytes_; }";
        cyc = 0; /* subtracted inside */
        after = "{ pc_ = " + hex(in.next & 0xFFFF) + "; goto out; }";
        break;
    }
    case Shape::wai:
        /* the sleep spends the rest of the slice with pc past it, as the
         * interpreter; the frame's NMI lands at the dispatcher's boundary */
        o << "    { machine.cpu.waiting = 1; pc_ = " << hex(in.next & 0xFFFF) << "; budget -= 3; goto out; }\n";
        return;
    case Shape::stp:
        o << "    { machine.cpu.halted = 1; pc_ = " << hex(in.next & 0xFFFF) << "; budget -= 3; goto out; }\n";
        return;
    case Shape::flow:
        if (is_branch(op, m)) {
            static const char* cond[] = { "1", "!(p_ & SNES_P_N)", "(p_ & SNES_P_N)", "!(p_ & SNES_P_V)",
                                          "(p_ & SNES_P_V)", "!(p_ & SNES_P_C)", "(p_ & SNES_P_C)",
                                          "!(p_ & SNES_P_Z)", "(p_ & SNES_P_Z)" };
            const unsigned c = in.code == 0x80 ? 0 : ((in.code >> 4) + 1) / 2;
            unsigned taken = 3;
            if (ef) {
                const uint32_t t = in.addr + 2 + (uint32_t)(int8_t)in.o[0];
                if (((in.addr + 2) & 0xFF00u) != (t & 0xFF00u)) taken = 4;
            }
            o << "    if (" << cond[c] << ") { budget -= " << taken << ";";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target & 0xFFFF) << "; goto out; }";
            o << " " << go(in, in.target) << " }\n";
            o << "    budget -= 2;";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.next & 0xFFFF) << "; goto out; }";
            o << "\n";
            return;
        }
        if (op == Op::jmp && m == Mode::abs) {
            if (in.target == in.addr) {
                /* the idle loop the other runtimes spend at once
                 * (m0_idle.h): left with pc on it, as the interpreter;
                 * the MDR carries the fetch store's target high byte */
                o << "    budget = SNES_SPIN3(budget); { pc_ = " << hex(in.addr & 0xFFFF) << "; goto out; }\n";
                return;
            }
            o << "    budget -= 3;";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target & 0xFFFF) << "; goto out; }";
            o << " " << go(in, in.target) << "\n";
            return;
        }
        if (op == Op::jmp && m == Mode::rell) { /* BRL */
            o << "    budget -= 3;";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target & 0xFFFF) << "; goto out; }";
            o << " " << go(in, in.target) << "\n";
            return;
        }
        if (op == Op::jmp && m == Mode::ind) {
            /* the pointer table sits in the program bank, not DBR */
            o << "    { const uint16_t ptr_ = " << hex(in.o[0] | in.o[1] << 8) << ";"
              << " pc_ = (uint16_t)(SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16) << "u | ptr_) |"
              << " SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16)
              << "u | (uint16_t)((ptr_ & 0xFF00u) | ((ptr_ + 1u) & 0xFFu))) << 8); } budget -= 5;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::jmp && m == Mode::absx) {
            o << "    { const uint16_t ptr_ = (uint16_t)(" << hex(in.o[0] | in.o[1] << 8) << "u + "
              << (xf ? "(x_ & 0xFF)" : "x_") << ");"
              << " pc_ = (uint16_t)(SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16) << "u | ptr_) |"
              << " SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16)
              << "u | (uint16_t)(ptr_ + 1u)) << 8); } budget -= 6;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::jmp && m == Mode::indlp) {
            o << "    { const uint32_t q_ = (uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16)
              << "u | (uint16_t)(" << hex(in.o[0] | in.o[1] << 8) << "u);"
              << " const unsigned lo_ = SNES_RD(q_), hi_ = SNES_RD(q_ + 1);"
              << " k_ = SNES_RD(q_ + 2); pc_ = (uint16_t)(lo_ | hi_ << 8); } budget -= 6;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::jml) {
            const bool far = (in.target >> 16) != ctx_k(in.ctx);
            o << "    budget -= " << (op == Op::jml ? 4 : 6) << ";";
            if (opt_.precise)
                o << " if (budget <= 0) " << (far ? "{ k_ = " + hex(in.target >> 16, 2) + "; pc_ = " +
                                                       hex(in.target & 0xFFFF) + "; goto out; }"
                                                  : "{ pc_ = " + hex(in.target & 0xFFFF) + "; goto out; }");
            o << " " << go(in, in.target) << "\n";
            return;
        }
        if (op == Op::jsr && m == Mode::abs) {
            const unsigned ret = unsigned(in.addr & 0xFFFF) + 2;
            o << "    SNES_PUSH16(" << hex(ret) << "u); budget -= 6;";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target & 0xFFFF) << "; goto out; }";
            o << " " << go(in, in.target) << "\n";
            return;
        }
        if (op == Op::jsl) {
            const unsigned ret = unsigned(in.addr & 0xFFFF) + 3;
            const bool far = (in.target >> 16) != ctx_k(in.ctx);
            o << "    SNES_PUSH(k_); SNES_PUSH16(" << hex(ret) << "u); budget -= 8;";
            if (opt_.precise)
                o << " if (budget <= 0) " << (far ? "{ k_ = " + hex(in.target >> 16, 2) + "; pc_ = " +
                                                       hex(in.target & 0xFFFF) + "; goto out; }"
                                                  : "{ pc_ = " + hex(in.target & 0xFFFF) + "; goto out; }");
            o << " " << go(in, in.target) << "\n";
            return;
        }
        if (op == Op::jsr && m == Mode::absx) {
            /* the pointer table sits in the program bank, not DBR */
            o << "    { const uint16_t ptr_ = (uint16_t)(" << hex(in.o[0] | in.o[1] << 8) << "u + "
              << (xf ? "(x_ & 0xFF)" : "x_") << ");"
              << " const unsigned lo_ = SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16) << "u | ptr_);"
              << " SNES_PUSH16(" << hex(unsigned(in.addr & 0xFFFF) + 3) << "u);"
              << " pc_ = (uint16_t)(lo_ | SNES_RD((uint32_t)" << hex((uint32_t)ctx_k(in.ctx) << 16)
              << "u | (uint16_t)(ptr_ + 1u)) << 8); } budget -= 8;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::rts) {
            o << "    { const unsigned w_ = SNES_POP16(); pc_ = (uint16_t)(w_ + 1u); } budget -= 6;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::rtl) {
            o << "    { const unsigned w_ = SNES_POP16(); k_ = SNES_POP(); pc_ = (uint16_t)(w_ + 1u); } budget -= 6;\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::rti) {
            o << "    { const uint8_t v_ = SNES_POP(); p_ = e_ ? (uint8_t)(v_ | 0x30u) : v_;"
              << " if (SNES_XF) { x_ &= 0xFF; y_ &= 0xFF; }"
              << " const unsigned w_ = SNES_POP16(); pc_ = (uint16_t)w_;"
              << " if (!e_) k_ = SNES_POP(); } budget -= " << (ef ? 6 : 7) << ";\n"
              << "    goto dyn;\n";
            return;
        }
        if (op == Op::brk || op == Op::cop) {
            const unsigned vector = op == Op::brk ? (ef ? 0xFFE6 : 0xFFF6) : (ef ? 0xFFE4 : 0xFFF4);
            o << "    { pc_ = (uint16_t)(" << hex(unsigned(in.addr & 0xFFFF) + 2) << "u);"
              << " if (!e_) SNES_PUSH(k_); SNES_PUSH16(pc_); SNES_PUSH((uint8_t)(e_ ? (p_ | 0x30u) : p_));"
              << " p_ |= SNES_P_I; p_ &= (uint8_t)~SNES_P_D;"
              << " k_ = 0;"
              << " if (e_) s_ = (uint16_t)(0x0100u | (s_ & 0xFF));"
              << " pc_ = (uint16_t)(SNES_RD(" << hex(vector) << "u) | SNES_RD(" << hex(vector + 1) << "u) << 8); }"
              << " budget -= " << (ef ? 7 : 8) << ";\n"
              << "    goto dyn;\n";
            return;
        }
        break;
    default:
        o << "    { pc_ = " << hex24(in.addr) << "; goto out; }\n";
        return;
    }
    /* the page-cross penalties of the indexed reads: a constant when the
     * context knows the width is 16-bit, a runtime condition otherwise.
     * The (dp),Y and (sr,S),Y forms read their base inside the body's
     * braces: theirs adjusts inside. */
    if ((in.info.pen & P_CROSS_Y) && (m == Mode::indy || m == Mode::sry))
        body += " " + cross(in, "w_", "y_");
    o << "    { " << pre << body;
    if (cyc) o << " budget -= " << cyc << ";";
    o << " }";
    if (in.info.pen & P_CROSS_X) o << " " << cross(in, hex(in.o[0] | in.o[1] << 8).c_str(), "x_");
    else if ((in.info.pen & P_CROSS_Y) && m != Mode::indy && m != Mode::sry)
        o << " " << cross(in, hex(in.o[0] | in.o[1] << 8).c_str(), "y_");
    if (opt_.precise) {
        o << " if (budget <= 0) { pc_ = " << hex(in.next & 0xFFFF) << "; goto out; }";
        if (may_io(in)) o << " { pc_ = " << hex(in.next & 0xFFFF) << "; goto out; }";
    }
    if (!after.empty()) o << " " << after;
    o << "\n";
}

void Emitter::emit_region(std::ostream& o, int r) {
    const auto& keys = regions_[size_t(r)];
    o << "\nstatic int32_t r_" << r << "(int32_t budget, unsigned entry) {\n"
      << "    SNES_LOAD();\n    int ni;\n    switch (entry) {\n";
    for (const Key& k : keys) {
        const Ins& in = ins_.at(k);
        if (in.label) {
            char b[16];
            snprintf(b, sizeof b, "%u", in.local);
            o << "    case " << b << ": goto L_" << hex(in.addr, 6) << ";\n";
        }
    }
    o << "    default: __builtin_unreachable();\n    }\n";
    for (size_t i = 0; i < keys.size(); i++) {
        const Ins& in = ins_.at(keys[i]);
        if (in.label) {
            o << "L_" << hex(in.addr, 6) << ":";
            if (!opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.addr) << "; goto out; }";
            o << "\n";
        }
        emit_ins(o, in);
        /* the region's last instruction falls into the next one's place
         * (the branches' not-taken path and the return sites included;
         * the exits and the breaks never fall).  A breaks instruction the
         * walk keeps falling from (a context change: SEP/REP/XCE/PLB/PLD/
         * PLP/TCD/MVN) hands the pc to the dispatcher explicitly — its
         * body never touches pc_, and falling into out: would save the
         * region's entry pc and re-run the whole region. */
        if (i + 1 == keys.size() && in.falls && !in.breaks)
            o << "    " << go(in, in.next) << "\n";
        else if (i + 1 == keys.size() && in.breaks && in.falls)
            o << "    pc_ = " << hex(in.next & 0xFFFF) << "; goto out;\n";
    }
    o << "out:\n    SNES_SAVE(); machine.cpu.pc = pc_; return budget;\n"
      << "chain:\n    SNES_SAVE(); machine.cpu.pc = pc_; snes_gen_next = ni; return budget;\n"
      << "dyn:\n    SNES_SAVE(); machine.cpu.pc = pc_; return budget;\n}\n";
}

std::string Emitter::run(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    select(coverage, stats);
    plan();
    std::ostringstream o;
    o << "/* Generated by gbrecomp --system snes" << (opt_.precise ? " --meta-precise" : "")
      << ". Do not edit. */\n"
      << "#define SNES_GEN_IMPL\n"
      << "#include \"snes_gen.h\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n"
      << "#if defined(__arm__) && !defined(SNES_GEN_O2)\n"
      << "#pragma GCC optimize(\"Os\") /* a fifth smaller than -O2 on Thumb-1 */\n"
      << "#endif\n\n"
      << "/* the next entry a region left for the dispatcher (checked against pc and context) */\n"
      << "static int snes_gen_next = -1;\n";
    /* noinline: one function per region, called by the dispatcher only */
    for (size_t r = 0; r < regions_.size(); r++)
        o << "static int32_t r_" << r << "(int32_t budget, unsigned entry) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, int(r));

    std::vector<const Ins*> entries;
    for (const auto& [key, in] : ins_)
        if (in.label) entries.push_back(&in);
    stats.regions = regions_.size();
    stats.entries = entries.size();
    const size_t count = std::max<size_t>(entries.size(), 1);
    o << "\nstatic const snes_region_fn region_fn[" << std::max<size_t>(regions_.size(), 1) << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 8 ? " " : "\n    ") << "r_" << r << ",";
    if (regions_.empty()) o << "0";
    o << "\n};\nstatic const uint32_t entry_pc[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 12 ? " " : "\n    ") << hex24(entries[i]->addr) << "u,";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint64_t entry_ctx[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 4 ? " " : "\n    ") << hex64(entries[i]->ctx) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_region[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->region << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_local[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->local << ",";
    if (entries.empty()) o << "0";
    /* open addressing over (K:PC, context), twice as many slots as
     * entries: one multiply and usually one probe */
    unsigned bits = 4;
    while ((size_t(1) << bits) < entries.size() * 2) bits++;
    const size_t slots = size_t(1) << bits;
    auto slot_of = [&](uint32_t pc, uint64_t ctx) {
        const uint64_t h64 = uint64_t(pc) * 0x9E3779B97F4A7C15ull + ctx * 0xFF51AFD7ED558CCDull;
        return unsigned((uint32_t(h64) ^ uint32_t(h64 >> 32)) >> (32 - bits));
    };
    std::vector<uint32_t> slot(slots, 0xFFFFFFFFu);
    for (size_t i = 0; i < entries.size(); i++) {
        unsigned h = slot_of(entries[i]->addr, entries[i]->ctx);
        while (slot[h] != 0xFFFFFFFFu) h = (h + 1) & (slots - 1);
        slot[h] = uint32_t(i);
    }
    o << "\n};\nstatic const uint16_t entry_slot[" << slots << "] = {";
    for (size_t i = 0; i < slots; i++) o << (i % 12 ? " " : "\n    ") << hex(slot[i]) << ",";
    o << "\n};\n\n"
      << "int snes_gen_lookup(uint32_t pc24, uint64_t ctx) {\n"
      << "    const int hint = snes_gen_next;\n"
      << "    if (hint >= 0 && entry_pc[hint] == pc24 && entry_ctx[hint] == ctx) return hint;\n"
      << "    const uint64_t h64 = (uint64_t)pc24 * 0x9E3779B97F4A7C15ull + ctx * 0xFF51AFD7ED558CCDull;\n"
      << "    unsigned h = ((uint32_t)h64 ^ (uint32_t)(h64 >> 32)) >> " << (32 - bits) << ";\n"
      << "    for (;;) {\n"
      << "        const unsigned i = entry_slot[h];\n"
      << "        if (i == 0xFFFFu) return -1;\n"
      << "        if (entry_pc[i] == pc24 && entry_ctx[i] == ctx) return (int)i;\n"
      << "        h = (h + 1u) & " << (slots - 1) << "u;\n"
      << "    }\n}\n\n"
      << "int32_t snes_gen_enter(int entry, int32_t budget) {\n"
      << "    return region_fn[entry_region[entry]](budget, entry_local[entry]);\n}\n";
    return o.str();
}

} // namespace

std::string emit_code(const std::vector<uint8_t>& rom, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats) {
    Emitter e(rom, options);
    return e.run(coverage, stats);
}

} // namespace m0boy::snes
