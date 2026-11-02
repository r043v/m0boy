/* 6502 -> C for the NES runtime.
 *
 * Instructions reached from the vectors and the profile (played ones first
 * when the flash budget cuts) are grouped by address into regions, one C
 * function each, with gotos inside and a switch on the entry at the top.
 * Every exit returns to the dispatcher (Thumb-1 GCC never tail-calls, and
 * a chain of calls would grow the C stack without bound): a known target
 * in another region leaves its entry as a hint the lookup checks first;
 * RTS, RTI and JMP through RAM leave pc to be looked up; anything without
 * an entry (code in RAM, undocumented opcodes, BRK, what the budget left
 * out) is the dispatcher's interpreter's.
 *
 * The semantics are the interpreter's (6502.c), cycle for cycle: same
 * counts, same page-cross penalties, same bus accesses in the same order,
 * the PRG folded only where it is a constant (NROM).  Precise mode checks
 * the budget after every instruction, as the interpreter does, so the slice
 * ends on the same instruction; the fast mode checks it only at labels and
 * at most every `check_cycles` cycles, a slice overrunning by that much. */
#include "m0boy/nes_emitter.h"
#include "m0boy/sweep.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>

namespace m0boy::nes {
namespace {

enum class Mode { imp, acc, imm, zp, zpx, zpy, abs, absx, absy, ind, izx, izy, rel };

enum class Op {
    none, lda, ldx, ldy, sta, stx, sty, adc, sbc, ora, and_, eor, cmp, cpx, cpy, bit,
    asl, lsr, rol, ror, inc, dec, inx, iny, dex, dey, tax, tay, txa, tya, tsx, txs,
    pha, php, pla, plp, clc, sec, cli, sei, clv, cld, sed, nop,
    bpl, bmi, bvc, bvs, bcc, bcs, bne, beq, jmp, jsr, rts, rti,
};

struct OpInfo {
    Op op = Op::none;
    Mode mode = Mode::imp;
    uint8_t cycles = 0;
    bool cross = false; /* indexed read: +1 when the page changes */
};

/* the interpreter's table (6502.c), documented opcodes only: the others
 * and BRK stay interpreted */
const std::vector<OpInfo>& table() {
    static std::vector<OpInfo> t = [] {
        std::vector<OpInfo> v(256);
        auto set = [&](unsigned code, Op op, Mode m, unsigned c, bool x = false) {
            v[code] = { op, m, uint8_t(c), x };
        };
        /* the eight-mode ALU groups: imm zp zpx abs absx absy izx izy */
        auto group = [&](Op op, unsigned imm, unsigned zp, unsigned zpx, unsigned ab, unsigned abx,
                         unsigned aby, unsigned izx, unsigned izy) {
            set(imm, op, Mode::imm, 2); set(zp, op, Mode::zp, 3); set(zpx, op, Mode::zpx, 4);
            set(ab, op, Mode::abs, 4); set(abx, op, Mode::absx, 4, true); set(aby, op, Mode::absy, 4, true);
            set(izx, op, Mode::izx, 6); set(izy, op, Mode::izy, 5, true);
        };
        group(Op::lda, 0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1);
        group(Op::adc, 0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71);
        group(Op::sbc, 0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1);
        group(Op::ora, 0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11);
        group(Op::and_, 0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31);
        group(Op::eor, 0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51);
        group(Op::cmp, 0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1);
        set(0xA2, Op::ldx, Mode::imm, 2); set(0xA6, Op::ldx, Mode::zp, 3); set(0xB6, Op::ldx, Mode::zpy, 4);
        set(0xAE, Op::ldx, Mode::abs, 4); set(0xBE, Op::ldx, Mode::absy, 4, true);
        set(0xA0, Op::ldy, Mode::imm, 2); set(0xA4, Op::ldy, Mode::zp, 3); set(0xB4, Op::ldy, Mode::zpx, 4);
        set(0xAC, Op::ldy, Mode::abs, 4); set(0xBC, Op::ldy, Mode::absx, 4, true);
        set(0x85, Op::sta, Mode::zp, 3); set(0x95, Op::sta, Mode::zpx, 4); set(0x8D, Op::sta, Mode::abs, 4);
        set(0x9D, Op::sta, Mode::absx, 5); set(0x99, Op::sta, Mode::absy, 5);
        set(0x81, Op::sta, Mode::izx, 6); set(0x91, Op::sta, Mode::izy, 6);
        set(0x86, Op::stx, Mode::zp, 3); set(0x96, Op::stx, Mode::zpy, 4); set(0x8E, Op::stx, Mode::abs, 4);
        set(0x84, Op::sty, Mode::zp, 3); set(0x94, Op::sty, Mode::zpx, 4); set(0x8C, Op::sty, Mode::abs, 4);
        set(0xE0, Op::cpx, Mode::imm, 2); set(0xE4, Op::cpx, Mode::zp, 3); set(0xEC, Op::cpx, Mode::abs, 4);
        set(0xC0, Op::cpy, Mode::imm, 2); set(0xC4, Op::cpy, Mode::zp, 3); set(0xCC, Op::cpy, Mode::abs, 4);
        set(0x24, Op::bit, Mode::zp, 3); set(0x2C, Op::bit, Mode::abs, 4);
        auto shift = [&](Op op, unsigned acc, unsigned zp, unsigned zpx, unsigned ab, unsigned abx) {
            if (acc) set(acc, op, Mode::acc, 2);
            set(zp, op, Mode::zp, 5); set(zpx, op, Mode::zpx, 6); set(ab, op, Mode::abs, 6); set(abx, op, Mode::absx, 7);
        };
        shift(Op::asl, 0x0A, 0x06, 0x16, 0x0E, 0x1E);
        shift(Op::lsr, 0x4A, 0x46, 0x56, 0x4E, 0x5E);
        shift(Op::rol, 0x2A, 0x26, 0x36, 0x2E, 0x3E);
        shift(Op::ror, 0x6A, 0x66, 0x76, 0x6E, 0x7E);
        shift(Op::inc, 0, 0xE6, 0xF6, 0xEE, 0xFE);
        shift(Op::dec, 0, 0xC6, 0xD6, 0xCE, 0xDE);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0xE8, Op::inx}, {0xC8, Op::iny}, {0xCA, Op::dex}, {0x88, Op::dey}, {0xAA, Op::tax},
                 {0xA8, Op::tay}, {0x8A, Op::txa}, {0x98, Op::tya}, {0xBA, Op::tsx}, {0x9A, Op::txs},
                 {0x18, Op::clc}, {0x38, Op::sec}, {0x58, Op::cli}, {0x78, Op::sei}, {0xB8, Op::clv},
                 {0xD8, Op::cld}, {0xF8, Op::sed}, {0xEA, Op::nop}})
            set(code, op, Mode::imp, 2);
        set(0x48, Op::pha, Mode::imp, 3); set(0x08, Op::php, Mode::imp, 3);
        set(0x68, Op::pla, Mode::imp, 4); set(0x28, Op::plp, Mode::imp, 4);
        for (auto [code, op] : std::initializer_list<std::pair<unsigned, Op>>{
                 {0x10, Op::bpl}, {0x30, Op::bmi}, {0x50, Op::bvc}, {0x70, Op::bvs},
                 {0x90, Op::bcc}, {0xB0, Op::bcs}, {0xD0, Op::bne}, {0xF0, Op::beq}})
            set(code, op, Mode::rel, 2);
        set(0x4C, Op::jmp, Mode::abs, 3); set(0x6C, Op::jmp, Mode::ind, 5);
        set(0x20, Op::jsr, Mode::abs, 6); set(0x60, Op::rts, Mode::imp, 6); set(0x40, Op::rti, Mode::imp, 6);
        return v;
    }();
    return t;
}

unsigned length(Mode m) {
    switch (m) {
    case Mode::imp: case Mode::acc: return 1;
    case Mode::abs: case Mode::absx: case Mode::absy: case Mode::ind: return 3;
    default: return 2;
    }
}

bool is_branch(Op op) { return op >= Op::bpl && op <= Op::beq; }

/* flag bits of the liveness sets: N|Z|C|V */
enum : uint8_t { FNZ_N = 1, FNZ_Z = 2, FNZ_C = 4, FNZ_V = 8, FNZ_ALL = 0xF };

/* the flags an instruction writes (the I/D bits are kept out of the
 * model: SEI/CLI/CLD/SED and the IRQ checks always run) */
uint8_t flag_writes(Op op) {
    switch (op) {
    case Op::lda: case Op::ldx: case Op::ldy: case Op::ora: case Op::and_: case Op::eor:
    case Op::inc: case Op::dec: case Op::inx: case Op::iny: case Op::dex: case Op::dey:
    case Op::tax: case Op::tay: case Op::txa: case Op::tya: case Op::tsx: case Op::pla:
        return FNZ_N | FNZ_Z;
    case Op::adc: case Op::sbc:
        return FNZ_ALL;
    case Op::cmp: case Op::cpx: case Op::cpy:
    case Op::asl: case Op::lsr: case Op::rol: case Op::ror:
        return FNZ_N | FNZ_Z | FNZ_C;
    case Op::bit:
        return FNZ_N | FNZ_Z | FNZ_V;
    case Op::clc: case Op::sec:
        return FNZ_C;
    case Op::clv:
        return FNZ_V;
    case Op::plp: case Op::rti:
        return FNZ_ALL;                  /* NES_SETP joins P back */
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
    case Op::php: return FNZ_ALL;        /* NES_P() joins P together */
    default:
        return 0;
    }
}

std::string hex(unsigned v, int digits = 4) {
    char b[16];
    snprintf(b, sizeof b, "0x%0*X", digits, v);
    return b;
}

struct Ins {
    uint16_t addr = 0;
    uint8_t code = 0, o1 = 0, o2 = 0;
    uint8_t bank = 0xFF;     /* the switchable bank mapped while this runs
                              * (0xFF: the fixed window at $C000-$FFFF, all
                              * of NROM) */
    OpInfo info;
    unsigned len = 1;
    uint16_t next = 0;
    uint16_t target = 0;     /* branch, JMP abs, JSR, JMP (ROM) */
    bool has_target = false;
    bool falls = true;       /* control can reach next */
    uint32_t hits = 0;
    bool dynamic = false;
    bool label = false;      /* an entry: switch case, lookup key, budget check (fast) */
    int region = -1;
    unsigned local = 0;
    unsigned global = 0;     /* index in the entry table */
    uint8_t live_out = 0xF;  /* flags read after this instruction: N|Z|C|V */
};

/* the fixed window's context: the same bytes whatever the bank register */
constexpr uint8_t BANK_FIXED = 0xFF;

class Emitter {
public:
    Emitter(const std::vector<uint8_t>& prg, const EmitOptions& o) : prg_(prg), opt_(o) {}
    std::string run(const std::vector<CoverageEntry>& coverage, EmitStats& stats);

private:
    const std::vector<uint8_t>& prg_;
    EmitOptions opt_;
    std::map<uint32_t, Ins> ins_;      /* (bank << 16) | address */
    std::vector<std::vector<uint32_t>> regions_;

    bool banked() const { return opt_.prg_size > 0x8000u; }
    static uint32_t key(uint16_t at, uint8_t bank) { return (uint32_t(bank) << 16) | at; }
    /* the context control carries from `bank` into `to` */
    uint8_t ctx_to(uint16_t to, uint8_t bank) const { return banked() && to >= 0xC000u ? BANK_FIXED : bank; }
    /* and whether the walk can follow at all: on a banked cartridge, a
     * transfer from the fixed window into the $8000-$BFFF window lands on
     * whatever bank is mapped — the coverage's, not a static one */
    bool followable(uint16_t to, uint8_t bank) const {
        return !banked() || to >= 0xC000u || bank != BANK_FIXED;
    }
    /* a byte at `at` under `bank`: past $BFFF the fixed window holds it */
    uint8_t byte_ctx(uint16_t at, uint8_t bank) const { return banked() && at >= 0xC000u ? BANK_FIXED : bank; }
    uint8_t rom(uint16_t at, uint8_t bank) const {
        if (!banked()) return prg_[(at - 0x8000u) & (opt_.prg_size - 1)];
        if (at >= 0xC000u) return prg_[opt_.prg_size - 0x4000u + (at - 0xC000u)];
        return prg_[bank * 0x4000u + (at - 0x8000u)];
    }
    bool decode(uint16_t at, uint8_t bank, Ins& in) const;
    bool compiled(uint16_t at, uint8_t bank) const { return ins_.count(key(at, bank)) != 0; }
    void select(const std::vector<CoverageEntry>& coverage, EmitStats& stats);
    void plan();
    void emit_region(std::ostream& o, int r);
    void emit_ins(std::ostream& o, const Ins& in);
    std::string go(const Ins& from, uint16_t to) const;
    std::string read(const Ins& in, std::string& pre, std::string& cross) const;
    std::string address(const Ins& in, std::string& pre, bool& ram, bool& may_dma) const;
    static std::string label(uint32_t k) { return "L_" + hex(k, 6).substr(2); }
};

bool Emitter::decode(uint16_t at, uint8_t bank, Ins& in) const {
    if (at < 0x8000u) return false;
    in = Ins{};
    in.addr = at;
    in.bank = bank;
    in.code = rom(at, bank);
    in.info = table()[in.code];
    if (in.info.op == Op::none) return false;
    in.len = length(in.info.mode);
    if (unsigned(at) + in.len - 1 > 0xFFFFu) return false;
    in.o1 = in.len > 1 ? rom(uint16_t(at + 1), byte_ctx(uint16_t(at + 1), bank)) : 0;
    in.o2 = in.len > 2 ? rom(uint16_t(at + 2), byte_ctx(uint16_t(at + 2), bank)) : 0;
    in.next = uint16_t(at + in.len);
    const uint16_t w = uint16_t(in.o1 | (in.o2 << 8));
    const Op op = in.info.op;
    if (is_branch(op)) {
        in.target = uint16_t(in.next + int8_t(in.o1));
        in.has_target = true;
    } else if (op == Op::jmp && in.info.mode == Mode::abs) {
        in.target = w; in.has_target = true; in.falls = false;
    } else if (op == Op::jmp) {
        in.falls = false;
        if (w >= 0x8000u) { /* through the ROM: a constant, the page-wrap bug included */
            const uint16_t hi_at = uint16_t((w & 0xFF00u) | ((w + 1) & 0xFFu));
            if (followable(w, byte_ctx(w, bank)) && followable(hi_at, byte_ctx(hi_at, bank))) {
                in.target = uint16_t(rom(w, byte_ctx(w, bank)) | (rom(hi_at, byte_ctx(hi_at, bank)) << 8));
                in.has_target = true;
            }
        }
    } else if (op == Op::jsr) {
        in.target = w; in.has_target = true; /* falls: the return site */
    } else if (op == Op::rts || op == Op::rti) {
        in.falls = false;
    }
    return true;
}

void Emitter::select(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    /* every reachable instruction, in the order it is found: the played
     * ones by executions first, then the vectors and what they reach.
     * One instance per (address, bank) pair: the same address under two
     * banks holds two instructions (a UxROM cartridge switches its $8000
     * window; the fixed window at $C000-$FFFF and all of NROM share the
     * 0xFF context). */
    std::vector<CoverageEntry> hot(coverage);
    std::stable_sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.hits > b.hits; });
    std::map<uint32_t, Ins> found;
    std::vector<uint32_t> order;
    std::deque<uint32_t> work, swept_q;
    auto visit = [&](uint16_t at, uint8_t bank, std::deque<uint32_t>& sink, m0::Sweep* budget) {
        if (at < 0x8000u) return;
        if (banked() && at >= 0xC000u) bank = BANK_FIXED; /* the fixed window */
        if (!followable(at, bank)) return;
        const uint32_t k = key(at, bank);
        if (found.count(k)) return;
        Ins in;
        if (!decode(at, bank, in)) return;
        if (budget && !budget->take()) return;
        found[k] = in;
        order.push_back(k);
        sink.push_back(k);
    };
    auto drain = [&](std::deque<uint32_t>& queue, m0::Sweep* budget) {
        while (!queue.empty()) {
            const Ins in = found[queue.front()];
            queue.pop_front();
            if (in.falls) visit(in.next, in.bank, queue, budget);
            if (in.has_target) visit(in.target, ctx_to(in.target, in.bank), queue, budget);
        }
    };
    std::map<uint32_t, const CoverageEntry*> played; /* (bank << 16) | pc */
    for (const auto& e : hot) played.emplace(key(e.pc, e.bank), &e);
    for (const auto& e : hot) visit(e.pc, e.bank, work, nullptr);
    const size_t covered = order.size();
    for (uint16_t v : { uint16_t(0xFFFC), uint16_t(0xFFFA), uint16_t(0xFFFE) })
        visit(uint16_t(rom(v, BANK_FIXED) | (rom(uint16_t(v + 1), BANK_FIXED) << 8)), BANK_FIXED, work, nullptr);
    drain(work, nullptr);
    /* the sweep: the PRG's JSR ($20) and JMP abs ($4C) whose target the
     * walk did not reach — the jump-table handlers the reachability
     * cannot prove, compiled the same way.  Nothing lands there until
     * the game jumps (the dispatcher enters by PC, and by bank when the
     * window is switchable), but the generated file grows: the swept
     * instructions rank after the played ones and take what the code
     * budget has left (m0/sweep.h), the walk itself never being capped.
     * A banked cartridge sweeps its fixed window only: a target under
     * $C000 has no bank the static walk could name. */
    m0::Sweep swept(m0::sweep_room(covered, opt_.max_instructions));
    if (opt_.sweep)
        for (uint32_t at = banked() ? 0xC000u : 0x8000u; at < 0x10000; at++) {
            const uint8_t op = rom(uint16_t(at), BANK_FIXED);
            if (op != 0x20 && op != 0x4C) continue;
            const uint16_t target = uint16_t(rom(uint16_t(at + 1), BANK_FIXED) | rom(uint16_t(at + 2), BANK_FIXED) << 8);
            if (target < 0x8000) continue; /* the code lives in PRG */
            visit(target, ctx_to(target, BANK_FIXED), swept_q, &swept);
        }
    drain(swept_q, &swept);
    const size_t keep = opt_.max_instructions ? std::min(order.size(), opt_.max_instructions) : order.size();
    for (size_t i = 0; i < keep; i++) {
        Ins in = found[order[i]];
        if (auto p = played.find(key(in.addr, in.bank)); p != played.end()) {
            in.hits = p->second->hits;
            in.dynamic = p->second->dynamic;
        }
        ins_[order[i]] = in;
    }
    stats.covered = std::min(covered, keep);
    stats.compiled = ins_.size();
}

void Emitter::plan() {
    /* entries: what a jump from elsewhere, the dispatcher or a return can
     * land on.  Precise mode: every instruction (the slice may end after
     * any of them). */
    auto mark = [&](uint16_t at, uint8_t bank) {
        if (auto p = ins_.find(key(at, bank)); p != ins_.end()) p->second.label = true;
    };
    for (uint16_t v : { uint16_t(0xFFFC), uint16_t(0xFFFA), uint16_t(0xFFFE) })
        mark(uint16_t(rom(v, BANK_FIXED) | (rom(uint16_t(v + 1), BANK_FIXED) << 8)), BANK_FIXED);
    for (auto& [k, in] : ins_) {
        (void)k;
        if (opt_.precise || in.dynamic) in.label = true;
        if (in.has_target) mark(in.target, ctx_to(in.target, in.bank));
        const Op op = in.info.op;
        /* the instruction after an exit resumes there: a return site, the
         * end of a DMA, an IRQ window */
        if (op == Op::jsr || op == Op::cli || op == Op::plp ||
            ((op == Op::sta || op == Op::stx || op == Op::sty) && in.info.mode == Mode::abs &&
             (in.o1 | (in.o2 << 8)) == 0x4014))
            mark(in.next, in.bank);
    }
    /* regions: runs of neighbouring instructions, the start an entry */
    const size_t cap = 256;
    for (auto& [k, in] : ins_) {
        bool joins = !regions_.empty() && regions_.back().size() < cap;
        if (joins) {
            const Ins& prev = ins_.at(regions_.back().back());
            joins = key(prev.next, ctx_to(prev.next, prev.bank)) == k;
        }
        if (!joins) {
            regions_.emplace_back();
            in.label = true;
        }
        in.region = int(regions_.size() - 1);
        regions_.back().push_back(k);
    }
    /* the fast mode's checks: at labels, and no run longer than
     * check_cycles without one */
    if (!opt_.precise)
        for (const auto& r : regions_) {
            unsigned run = 0;
            for (uint32_t k : r) {
                Ins& in = ins_.at(k);
                if (in.label) run = 0;
                if (run > opt_.check_cycles) { in.label = true; run = 0; }
                run += in.info.cycles + 2;
            }
        }
    for (const auto& r : regions_) {
        unsigned local = 0;
        for (uint32_t k : r) {
            Ins& in = ins_.at(k);
            if (in.label) in.local = local++;
        }
    }
    unsigned global = 0;
    for (auto& [k, in] : ins_)
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

/* where control goes to reach `to` */
std::string Emitter::go(const Ins& from, uint16_t to) const {
    auto p = ins_.find(key(to, ctx_to(to, from.bank)));
    if (p == ins_.end() || !p->second.label) return "{ pc_ = " + hex(to) + "; goto out; }";
    if (p->second.region == from.region) return "goto " + label(key(to, p->second.bank)) + ";";
    return "{ pc_ = " + hex(to) + "; ni = " + std::to_string(p->second.global) + "; goto chain; }";
}

/* the operand's address as C, with `ram` when it is certainly RAM and
 * `may_dma` when it may be $4014 */
std::string Emitter::address(const Ins& in, std::string& pre, bool& ram, bool& may_dma) const {
    const unsigned w = in.o1 | (in.o2 << 8);
    ram = false;
    may_dma = false;
    switch (in.info.mode) {
    case Mode::zp: ram = true; return hex(in.o1, 2);
    case Mode::zpx: ram = true; return "(uint8_t)(" + hex(in.o1, 2) + " + x)";
    case Mode::zpy: ram = true; return "(uint8_t)(" + hex(in.o1, 2) + " + y)";
    case Mode::abs: ram = w < 0x2000u; may_dma = w == 0x4014u; return hex(w);
    case Mode::absx: case Mode::absy:
        ram = w + 0xFFu < 0x2000u;
        may_dma = w <= 0x4014u && w + 0xFFu >= 0x4014u;
        return "(uint16_t)(" + hex(w) + " + " + (in.info.mode == Mode::absx ? "x" : "y") + ")";
    case Mode::izx:
        may_dma = true;
        pre += "const uint8_t b_ = (uint8_t)(" + hex(in.o1, 2) + " + x); "
               "const uint16_t t_ = (uint16_t)(ram_[b_] | (ram_[(uint8_t)(b_ + 1u)] << 8)); ";
        return "t_";
    case Mode::izy:
        may_dma = true;
        pre += "const uint16_t p_ = (uint16_t)(ram_[" + hex(in.o1, 2) + "] | (ram_[" +
               hex((in.o1 + 1) & 0xFF, 2) + "] << 8)); const uint16_t t_ = (uint16_t)(p_ + y); ";
        return "t_";
    default: return "0";
    }
}

/* the operand's value as C: immediates and the fixed window are constants,
 * RAM is indexed directly, the rest goes through the bus; `cross` is the
 * page penalty's expression when the opcode pays it */
std::string Emitter::read(const Ins& in, std::string& pre, std::string& cross) const {
    const unsigned w = in.o1 | (in.o2 << 8);
    const std::string mask = hex(opt_.prg_size - 1);
    cross.clear();
    switch (in.info.mode) {
    case Mode::imm: return hex(in.o1, 2);
    case Mode::zp: return "ram_[" + hex(in.o1, 2) + "]";
    case Mode::zpx: return "ram_[(uint8_t)(" + hex(in.o1, 2) + " + x)]";
    case Mode::zpy: return "ram_[(uint8_t)(" + hex(in.o1, 2) + " + y)]";
    case Mode::abs:
        if (w < 0x2000u) return "ram_[" + hex(w & 0x7FFu, 3) + "]";
        if (w >= 0x8000u) {
            if (!banked()) return hex(rom(uint16_t(w), BANK_FIXED), 2);
            if (w >= 0xC000u) return hex(rom(uint16_t(w), BANK_FIXED), 2);
            /* the switchable window: the bank is the machine's at run time */
            return "nes_prg[NES_C.bank_base + " + hex(w - 0x8000u) + "u]";
        }
        return "nes_bus_read(" + hex(w) + ")";
    case Mode::absx: case Mode::absy: {
        const std::string r = in.info.mode == Mode::absx ? "x" : "y";
        if (in.info.cross && (w & 0xFFu)) cross = "((" + hex(w & 0xFFu, 2) + "u + " + r + ") >> 8)";
        if (w + 0xFFu < 0x2000u) return "ram_[(" + hex(w) + "u + " + r + ") & 0x7FFu]";
        if (w >= 0x8000u && w + 0xFFu <= 0xFFFFu) {
            if (!banked()) return "nes_prg[(" + hex(w - 0x8000u) + "u + " + r + ") & " + mask + "u]";
            if (w >= 0xC000u) /* the whole operand stays in the fixed window */
                return "nes_prg[NES_C.fixed_base + " + hex(w & 0x3FFFu) + "u + " + r + "]";
        }
        return "nes_gen_rd((uint16_t)(" + hex(w) + " + " + r + "))";
    }
    case Mode::izx: case Mode::izy: {
        bool ram, dma;
        const std::string at = address(in, pre, ram, dma);
        if (in.info.mode == Mode::izy && in.info.cross) cross = "(((p_ & 0xFFu) + y) >> 8)";
        return "nes_gen_rd(" + at + ")";
    }
    default: return "0";
    }
}

void Emitter::emit_ins(std::ostream& o, const Ins& in) {
    const Op op = in.info.op;
    const Mode m = in.info.mode;
    const std::string next = go(in, in.next);
    std::string pre, cross, body;
    unsigned cycles = in.info.cycles;
    std::string after;        /* after the budget: an exit the instruction asks for */
    auto store = [&](const std::string& value) {
        bool ram, dma;
        const std::string at = address(in, pre, ram, dma);
        const unsigned w = in.o1 | (in.o2 << 8);
        if (ram) {
            if (m == Mode::zp || m == Mode::zpx || m == Mode::zpy) return "ram_[" + at + "] = " + value + ";";
            if (m == Mode::abs) return "ram_[" + hex(w & 0x7FFu, 3) + "] = " + value + ";";
            return "ram_[" + at + " & 0x7FFu] = " + value + ";";
        }
        if (m == Mode::abs && w >= 0x8000u) {
            if (banked()) {
                /* the write selects the bank; reached from the switchable
                 * window, the region leaves (the dispatcher resumes under
                 * the bank the game mapped — nothing follows a bank switch
                 * but a jump into the fixed window) and from the fixed one
                 * the bytes it runs on do not move */
                if (in.bank != BANK_FIXED) after = "{ pc_ = " + hex(in.next) + "; goto out; }";
                return "nes_bus_write(" + hex(w) + ", " + value + ");";
            }
            return std::string("(void)0; /* NROM: no register */");
        }
        /* $4014: the DMA's stall is the dispatcher's, taken before the next
         * instruction (precise: whenever the address may be it) */
        if (m == Mode::abs && w == 0x4014u) after = "{ pc_ = " + hex(in.next) + "; goto out; }";
        else if (dma && opt_.precise)
            after = "if (NES_C.stall_pending) { pc_ = " + hex(in.next) + "; goto out; }";
        if (m == Mode::abs) return "nes_bus_write(" + at + ", " + value + ");";
        return "nes_gen_wr(" + at + ", " + value + ");";
    };
    /* the flags still read after this instruction (the liveness pass keeps
     * 0xF in precise mode and at every exit) */
    const uint8_t lv = in.live_out;
    const bool nz = lv & (FNZ_N | FNZ_Z), cl = lv & FNZ_C, vl = lv & FNZ_V;
    /* read-modify-write: the bus sees the read, then the write; an indexed
     * operand's address is computed once for both */
    bool rmw_ram = false, rmw_dma = false;
    std::string rmw_rd, rmw_wr;
    if (m == Mode::zp || m == Mode::zpx || m == Mode::zpy ||
        m == Mode::absx || m == Mode::absy) {
        const std::string at = address(in, pre, rmw_ram, rmw_dma);
        const std::string idx = (m == Mode::absx || m == Mode::absy) ? " & 0x7FFu" : "";
        if (rmw_ram) {
            rmw_rd = "ram_[" + at + idx + "]";
            rmw_wr = "ram_[" + at + idx + "] = r_;";
        } else {
            rmw_rd = "nes_gen_rd(" + at + ")";
            rmw_wr = "nes_gen_wr(" + at + ", r_);";
        }
        /* $4014 may hide behind an indexed address (precise mode stalls) */
        if (rmw_dma && opt_.precise)
            after = "if (NES_C.stall_pending) { pc_ = " + hex(in.next) + "; goto out; }";
    }
    /* the shift's value from m__, with the flags this instruction keeps */
    auto shift_core = [&](unsigned kind) {
        static const char* sh[4] = {"(uint8_t)(m__ << 1)", "(uint8_t)(m__ >> 1)",
                                    "(uint8_t)((m__ << 1) | c)", "(uint8_t)((m__ >> 1) | (c << 7))"};
        static const char* cw[4] = {"(uint8_t)(m__ >> 7)", "(uint8_t)(m__ & 1u)",
                                    "(uint8_t)(m__ >> 7)", "(uint8_t)(m__ & 1u)"};
        std::string s = "const uint8_t r_ = " + std::string(sh[kind]) + "; ";
        if (cl) s += std::string("c = ") + cw[kind] + "; ";
        if (nz) s += "NES_ZN(r_); ";
        return s;
    };
    auto modify = [&](unsigned kind) {
        if (m == Mode::acc) return "{ const uint8_t m__ = a; " + shift_core(kind) + "a = r_; }";
        if (m == Mode::abs) {
            /* a constant address: the ROM folds, $4014 keeps its bus path */
            const std::string value = read(in, pre, cross);
            cross.clear(); /* the read-modify-writes pay no page penalty: their count holds it */
            return "{ const uint8_t m__ = " + value + "; " + shift_core(kind) + store("r_") + " }";
        }
        cross.clear();
        return "{ const uint8_t m__ = " + rmw_rd + "; " + shift_core(kind) + rmw_wr + " }";
    };
    auto step = [&](const std::string& f) {
        if (m == Mode::abs) {
            const std::string value = read(in, pre, cross);
            cross.clear();
            return "{ const uint8_t r_ = (uint8_t)(" + value + " " + f + "); " +
                   (nz ? "NES_ZN(r_); " : "") + store("r_") + " }";
        }
        return "{ const uint8_t r_ = (uint8_t)(" + rmw_rd + " " + f + "); " +
               (nz ? "NES_ZN(r_); " : "") + rmw_wr + " }";
    };
    const std::string zn_a = nz ? " NES_ZN(a);" : "", zn_x = nz ? " NES_ZN(x);" : "",
                      zn_y = nz ? " NES_ZN(y);" : "";
    switch (op) {
    case Op::lda: body = "a = " + read(in, pre, cross) + ";" + zn_a; break;
    case Op::ldx: body = "x = " + read(in, pre, cross) + ";" + zn_x; break;
    case Op::ldy: body = "y = " + read(in, pre, cross) + ";" + zn_y; break;
    case Op::sta: body = store("a"); break;
    case Op::stx: body = store("x"); break;
    case Op::sty: body = store("y"); break;
    case Op::adc: case Op::sbc: {
        const std::string val = read(in, pre, cross);
        if (nz) body = (op == Op::adc ? "NES_ADC(" : "NES_SBC(") + val + ");";
        else if (cl || vl) body = (op == Op::adc ? "NES_ADC_NC(" : "NES_SBC_NC(") + val + ");";
        else body = op == Op::adc ? "a = (uint8_t)(a + " + val + " + c);"
                                  : "a = (uint8_t)(a + (uint8_t)~" + val + " + c);";
        break;
    }
    case Op::ora: body = "a |= " + read(in, pre, cross) + ";" + zn_a; break;
    case Op::and_: body = "a &= " + read(in, pre, cross) + ";" + zn_a; break;
    case Op::eor: body = "a ^= " + read(in, pre, cross) + ";" + zn_a; break;
    case Op::cmp: case Op::cpx: case Op::cpy: {
        const std::string r = op == Op::cmp ? "a" : op == Op::cpx ? "x" : "y";
        const std::string val = read(in, pre, cross);
        if (nz && cl) body = "NES_CMP(" + r + ", " + val + ");";
        else if (cl) body = "c = (uint8_t)((uint8_t)" + r + " >= " + val + ");";
        else if (nz) body = "NES_ZN((uint8_t)(" + r + " - " + val + "));";
        else body = "(void)(" + val + ");";
        break;
    }
    case Op::bit: {
        const std::string val = read(in, pre, cross);
        if (!(lv & (FNZ_N | FNZ_Z | FNZ_V))) body = "(void)(" + val + ");";
        else {
            body = "{ const uint8_t m__ = " + val + ";";
            if (lv & FNZ_Z) body += " z = (uint8_t)(a & m__);";
            if (lv & FNZ_V) body += " v = (uint8_t)(m__ & 0x40u);";
            if (lv & FNZ_N) body += " n = m__;";
            body += " }";
        }
        break;
    }
    case Op::asl: body = modify(0); break;
    case Op::lsr: body = modify(1); break;
    case Op::rol: body = modify(2); break;
    case Op::ror: body = modify(3); break;
    case Op::inc: body = step("+ 1u"); break;
    case Op::dec: body = step("- 1u"); break;
    case Op::inx: body = nz ? "x++; NES_ZN(x);" : "x++;"; break;
    case Op::iny: body = nz ? "y++; NES_ZN(y);" : "y++;"; break;
    case Op::dex: body = nz ? "x--; NES_ZN(x);" : "x--;"; break;
    case Op::dey: body = nz ? "y--; NES_ZN(y);" : "y--;"; break;
    case Op::tax: body = nz ? "x = a; NES_ZN(x);" : "x = a;"; break;
    case Op::tay: body = nz ? "y = a; NES_ZN(y);" : "y = a;"; break;
    case Op::txa: body = nz ? "a = x; NES_ZN(a);" : "a = x;"; break;
    case Op::tya: body = nz ? "a = y; NES_ZN(a);" : "a = y;"; break;
    case Op::tsx: body = nz ? "x = s; NES_ZN(x);" : "x = s;"; break;
    case Op::txs: body = "s = x;"; break;
    case Op::pha: body = "NES_PUSH(a);"; break;
    case Op::php: body = "NES_PUSH((uint8_t)(NES_P() | NES_P_B));"; break;
    case Op::pla: body = nz ? "a = NES_POP(); NES_ZN(a);" : "a = NES_POP();"; break;
    case Op::plp:
        body = "NES_SETP(NES_POP());";
        after = "if (NES_C.irq_line && !(id & NES_P_I)) { pc_ = " + hex(in.next) + "; goto out; }";
        break;
    case Op::clc: body = "c = 0;"; break;
    case Op::sec: body = "c = 1;"; break;
    case Op::cli:
        body = "id &= (uint8_t)~NES_P_I;";
        after = "if (NES_C.irq_line) { pc_ = " + hex(in.next) + "; goto out; }";
        break;
    case Op::sei: body = "id |= NES_P_I;"; break;
    case Op::clv: body = "v = 0;"; break;
    case Op::cld: body = "id &= (uint8_t)~NES_P_D;"; break;
    case Op::sed: body = "id |= NES_P_D;"; break;
    case Op::nop: body = ""; break;
    case Op::bpl: case Op::bmi: case Op::bvc: case Op::bvs:
    case Op::bcc: case Op::bcs: case Op::bne: case Op::beq: {
        static const char* cond[] = { "!(n & 0x80u)", "(n & 0x80u)", "!v", "v", "!c", "c", "z", "!z" };
        const unsigned taken = 3u + (((in.next ^ in.target) & 0xFF00u) ? 1u : 0u);
        o << "    if (" << cond[unsigned(op) - unsigned(Op::bpl)] << ") { budget -= " << taken << "; ";
        if (opt_.precise) o << "if (budget <= 0) { pc_ = " << hex(in.target) << "; goto out; } ";
        o << go(in, in.target) << " }\n";
        o << "    budget -= 2;";
        if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.next) << "; goto out; }";
        o << "\n";
        return;
    }
    case Op::jmp:
        if (in.has_target && in.target == in.addr && cycles == 3) {
            /* JMP to itself, a game waiting for its NMI: the rest of the
             * slice in jumps at once (nes_gen.h, NES_SPIN3), left with pc
             * on it as the interpreter would; entered with budget > 0 */
            o << "    budget = NES_SPIN3(budget); { pc_ = " << hex(in.addr) << "; goto out; }\n";
            return;
        }
        if (in.has_target) {
            o << "    budget -= " << cycles << ";";
            if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target) << "; goto out; }";
            o << " " << go(in, in.target) << "\n";
            return;
        } else {
            const unsigned w = in.o1 | (in.o2 << 8);
            const unsigned w2 = (w & 0xFF00u) | ((w + 1) & 0xFFu);
            auto byte = [&](unsigned at) {
                return at < 0x2000u ? "ram_[" + hex(at & 0x7FFu, 3) + "]" : "nes_bus_read(" + hex(at) + ")";
            };
            o << "    pc_ = (uint16_t)(" << byte(w) << " | (" << byte(w2) << " << 8)); budget -= " << cycles << ";";
            if (opt_.precise) o << " if (budget <= 0) goto out;";
            o << " goto dyn;\n";
            return;
        }
    case Op::jsr:
        o << "    NES_PUSH(" << hex(uint16_t(in.addr + 2) >> 8, 2) << "); NES_PUSH(" << hex(uint16_t(in.addr + 2) & 0xFF, 2)
          << "); budget -= 6;";
        if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.target) << "; goto out; }";
        o << " " << go(in, in.target) << "\n";
        return;
    case Op::rts:
        o << "    { const uint8_t lo_ = NES_POP(); const uint8_t hi_ = NES_POP(); pc_ = (uint16_t)((lo_ | (hi_ << 8)) + 1u); }"
          << " budget -= 6;";
        if (opt_.precise) o << " if (budget <= 0) goto out;";
        o << " goto dyn;\n";
        return;
    case Op::rti:
        o << "    NES_SETP(NES_POP()); { const uint8_t lo_ = NES_POP(); const uint8_t hi_ = NES_POP(); pc_ = (uint16_t)(lo_ | (hi_ << 8)); }"
          << " budget -= 6;";
        if (opt_.precise) o << " if (budget <= 0) goto out;";
        o << " goto dyn;\n";
        return;
    default:
        o << "    { pc_ = " << hex(in.addr) << "; goto out; }\n";
        return;
    }
    o << "    { " << pre << body;
    o << " budget -= " << cycles;
    if (!cross.empty()) o << " + " << cross;
    o << "; }";
    if (opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.next) << "; goto out; }";
    if (!after.empty()) o << " " << after;
    o << "\n";
}

void Emitter::emit_region(std::ostream& o, int r) {
    const auto& keys = regions_[size_t(r)];
    o << "\nstatic int32_t r_" << r << "(int32_t budget, unsigned entry) {\n"
      << "    NES_LOAD();\n    NES_RAM_BASE();\n    uint16_t pc_;\n    int ni;\n"
      << "    switch (entry) {\n";
    for (uint32_t k : keys) {
        const Ins& in = ins_.at(k);
        if (in.label) o << "    case " << in.local << ": goto " << label(k) << ";\n";
    }
    o << "    default: __builtin_unreachable();\n    }\n";
    for (size_t i = 0; i < keys.size(); i++) {
        const Ins& in = ins_.at(keys[i]);
        if (in.label) {
            o << label(keys[i]) << ":";
            if (!opt_.precise) o << " if (budget <= 0) { pc_ = " << hex(in.addr) << "; goto out; }";
            o << "\n";
        }
        emit_ins(o, in);
        /* the region's last instruction falls into the next one's place */
        if (i + 1 == keys.size() && in.falls && in.info.op != Op::jmp) o << "    " << go(in, in.next) << "\n";
    }
    o << "out:\n    NES_SAVE(); NES_C.pc = pc_; return budget;\n"
      << "chain:\n    NES_SAVE(); NES_C.pc = pc_; nes_gen_next = ni; return budget;\n"
      << "dyn:\n    NES_SAVE(); NES_C.pc = pc_; return budget;\n}\n";
}

std::string Emitter::run(const std::vector<CoverageEntry>& coverage, EmitStats& stats) {
    select(coverage, stats);
    plan();
    std::ostringstream o;
    o << "/* Generated by gbrecomp --system nes" << (opt_.precise ? " --meta-precise" : "") << ". Do not edit. */\n"
      << "#include \"nes_gen.h\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n"
      << "#if defined(__arm__) && !defined(NES_GEN_O2)\n"
      << "#pragma GCC optimize(\"Os\") /* a fifth smaller than -O2 on Thumb-1 */\n"
      << "#endif\n\n"
      << "/* the next entry a region left for the dispatcher (checked against pc) */\n"
      << "static int nes_gen_next = -1;\n";
    /* noinline: one function per region, called by the dispatcher only */
    for (size_t r = 0; r < regions_.size(); r++)
        o << "static int32_t r_" << r << "(int32_t budget, unsigned entry) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, int(r));

    std::vector<const Ins*> entries;
    for (const auto& [k, in] : ins_) if (in.label) entries.push_back(&in);
    stats.regions = regions_.size();
    stats.entries = entries.size();
    const size_t count = std::max<size_t>(entries.size(), 1);
    o << "\nstatic const nes_region_fn region_fn[" << std::max<size_t>(regions_.size(), 1) << "] = {";
    for (size_t r = 0; r < regions_.size(); r++) o << (r % 8 ? " " : "\n    ") << "r_" << r << ",";
    if (regions_.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_pc[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 12 ? " " : "\n    ") << hex(entries[i]->addr) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_region[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->region << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_local[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << entries[i]->local << ",";
    if (entries.empty()) o << "0";
    /* open addressing over the entry keys, twice as many slots as
     * entries: one multiply and usually one probe.  The key is the PC,
     * and the bank context with it when the window at $8000 is
     * switchable: the dispatcher enters an entry only under the bank it
     * was decoded from (the fixed window and NROM carry 0xFF). */
    const bool bank_lookup = banked();
    unsigned bits = 4;
    while ((size_t(1) << bits) < entries.size() * 2) bits++;
    const size_t slots = size_t(1) << bits;
    std::vector<uint32_t> slot(slots, 0xFFFFu);
    for (size_t i = 0; i < entries.size(); i++) {
        const uint32_t k = key(entries[i]->addr, entries[i]->bank);
        size_t h = uint32_t(k * 0x9E3779B1u) >> (32 - bits);
        while (slot[h] != 0xFFFFu) h = (h + 1) & (slots - 1);
        slot[h] = uint32_t(i);
    }
    o << "\n};\nstatic const uint8_t entry_bank[" << count << "] = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 16 ? " " : "\n    ") << hex(entries[i]->bank, 2) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const uint16_t entry_slot[" << slots << "] = {";
    for (size_t i = 0; i < slots; i++) o << (i % 12 ? " " : "\n    ") << hex(slot[i]) << ",";
    o << "\n};\n\n"
      << "int nes_gen_lookup(uint16_t pc) {\n"
      << "    unsigned bank = 0xFFu;\n";
    if (bank_lookup)
        o << "    if (pc < 0xC000u) bank = NES_C.bank;\n";
    o << "    const uint32_t k = ((uint32_t)bank << 16) | pc;\n"
      << "    const int hint = nes_gen_next;\n"
      << "    if (hint >= 0 && entry_pc[hint] == pc && entry_bank[hint] == bank) return hint;\n"
      << "    unsigned h = ((uint32_t)k * 0x9E3779B1u) >> " << (32 - bits) << ";\n"
      << "    for (;;) {\n"
      << "        const unsigned i = entry_slot[h];\n"
      << "        if (i == 0xFFFFu) return -1;\n"
      << "        if (entry_pc[i] == pc && entry_bank[i] == bank) return (int)i;\n"
      << "        h = (h + 1u) & " << (slots - 1) << "u;\n"
      << "    }\n}\n\n"
      << "int32_t nes_gen_enter(int entry, int32_t budget) {\n"
      << "    return region_fn[entry_region[entry]](budget, entry_local[entry]);\n}\n";
    return o.str();
}

} // namespace

std::string emit_code(const std::vector<uint8_t>& prg, const std::vector<CoverageEntry>& coverage,
                      const EmitOptions& options, EmitStats& stats) {
    Emitter e(prg, options);
    return e.run(coverage, stats);
}

} // namespace m0boy::nes
