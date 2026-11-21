/* TLCS-900H -> C for the NGP runtime (targets/ngp/runtime/ngp_cpu.c).
 *
 * The NGP cartridge windows map the image flat (0x200000, and 0x800000 for
 * a 4 MiB cart's upper half), so a translated instruction knows its bytes:
 * one function per instruction, the fetch, the (mem) addressing and the
 * primary dispatch folded to constants, the instruction's effects left to
 * the interpreter's own handlers (ngp_gen_exec / ngp_gen_exec1 in
 * ngp_cpu.c) — the same expressions, cycle for cycle, which is what the
 * run_hash contract needs.  The walk starts at the header's entry and
 * follows what it can prove (jumps, calls, branches, DJNZ); WRAM-copied
 * code, jump tables and undefined opcodes stay with the interpreter (the
 * dispatcher's lookup misses and ngp_step runs them).
 *
 * The dispatcher (ngp_step under NGP_NATIVE) enters one translated
 * instruction per call and ticks its cycles exactly where the interpreter
 * would, so the timers, the interrupt acceptance points and the frame
 * boundary land on the same ticks. */
#include "m0boy/ngp_emitter.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <sstream>
#include <vector>

namespace m0boy::ngp {
namespace {

std::string hex32(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08Xu", v);
    return b;
}
std::string hex8(uint32_t v) {
    char b[8];
    snprintf(b, sizeof b, "0x%02Xu", v);
    return b;
}

/* the groups of ngp_cpu.c: a single-byte form runs through ngp_gen_exec1,
 * a two-byte form through ngp_gen_exec with the group named by first */
enum Group { single, src_g, dst_g, reg_g };

struct Insn {
    uint32_t addr = 0, next = 0;
    uint8_t first = 0, second = 0;
    Group group = single;
    int size = 0;            /* 0 byte, 1 word, 2 long (group forms) */
    uint8_t rcode = 0, brc = 0;
    uint32_t tail_pc = 0;    /* where the handler's tail fetches start (pc preset) */
    std::string pre;         /* statements before the (mem) address (the r32 inc/dec) */
    std::string mem_expr;    /* the (mem) address as C ("0" when none) */
    int extra = 0;           /* the addressing's cycles_extra */
    bool fall = false;       /* the walk follows the next instruction */
    uint32_t fall_to = 0;
    bool has_target = false; /* a target the walk can prove */
    uint32_t target = 0;
};

class Emitter {
public:
    explicit Emitter(const std::vector<uint8_t>& rom) : rom_(rom) {}
    std::map<uint32_t, Insn> run(const EmitOptions& opt, size_t& reached);

private:
    const std::vector<uint8_t>& rom_;

    /* the image offset an address maps to (flash_offset in ngp.c) */
    long off_of(uint32_t addr) const {
        if (addr >= 0x200000u && addr < 0x400000u) return long(addr - 0x200000u);
        if (addr >= 0x800000u && addr < 0xA00000u) return long(addr - 0x800000u + 0x200000u);
        return -1;
    }
    bool byte_at(uint32_t addr, uint8_t& v) const {
        const long off = off_of(addr);
        if (off < 0 || size_t(off) >= rom_.size()) return false;
        v = rom_[size_t(off)];
        return true;
    }
    /* the (reg)/(reg+d8) register of the 0x80-0xBF modes names the current
     * bank's registers (reg_l in ngp_cpu.c); the long register-code that
     * names the same cell through ngp_code_l */
    static unsigned bank_code(unsigned r) { return r < 4 ? 0xE0u + 4u * r : 0xF0u + 4u * (r - 4u); }
    bool decode(uint32_t addr, Insn& in) const;
};

/* the seconds the handlers run (the others fault, the interpreter keeps
 * them: the walk stops) */
bool src_second(uint8_t s) {
    return (s >= 0x04 && s <= 0x07) || (s >= 0x10 && s <= 0x17) || s == 0x19 ||
           (s >= 0x20 && s <= 0x37) || (s >= 0x38 && s <= 0x4F) || (s >= 0x50 && s <= 0x6F) ||
           (s >= 0x78 && s <= 0xFF);
}
bool dst_second(uint8_t s) {
    return s == 0x00 || s == 0x02 || s == 0x04 || s == 0x06 || s == 0x14 || s == 0x16 ||
           (s >= 0x20 && s <= 0x2C) || (s >= 0x30 && s <= 0x37) || (s >= 0x40 && s <= 0x67) ||
           s == 0x80 || s == 0x88 || s == 0x90 || s == 0x98 || s == 0xA0 ||
           (s >= 0xA8 && s <= 0xFF);
}
bool reg_second(uint8_t s) {
    return (s >= 0x03 && s <= 0x10) || s == 0x12 || s == 0x13 || s == 0x14 || s == 0x16 ||
           s == 0x19 || s == 0x1C || (s >= 0x20 && s <= 0x24) || (s >= 0x28 && s <= 0x2C) ||
           s == 0x2E || s == 0x2F || (s >= 0x30 && s <= 0x34) || (s >= 0x38 && s <= 0x3A) ||
           (s >= 0x3C && s <= 0x3E) || (s >= 0x60 && s <= 0xFF);
}

/* the operand bytes a handler fetches after the second byte (the walk's
 * length; the generated code leaves pc at the instruction's end and the
 * handler fetches them itself, the bytes being the same) */
unsigned tail_bytes(Group g, uint8_t s, int size) {
    if (g == src_g) {
        if (s == 0x19) return 2;
        if (s >= 0x38 && s <= 0x3F) return size == 0 ? 1u : 2u;
        return 0;
    }
    if (g == dst_g) {
        if (s == 0x00 || s == 0x14 || s == 0x16) return 1;
        if (s == 0x02) return 2;
        return 0;
    }
    /* reg_g */
    switch (s) {
        case 0x03: return size == 0 ? 1u : size == 1 ? 2u : 4u;
        case 0x08: case 0x09: case 0x0A: case 0x0B: return size == 0 ? 1u : 2u;
        case 0x0C: return 2;
        case 0x1C: return 1;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
        case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
            return 1;
        case 0x38: case 0x39: case 0x3A: case 0x3C: case 0x3D: case 0x3E: return 2;
        default: return 0;
    }
}

bool Emitter::decode(uint32_t addr, Insn& in) const {
    uint8_t f;
    if (!byte_at(addr, f)) return false;
    in = Insn{};
    in.addr = addr;
    in.first = f;
    in.mem_expr = "0";
    in.fall = true;
    in.fall_to = 0;
    uint32_t p = addr + 1; /* the walk's cursor: the second byte's position */

    /* ---- single-byte forms ------------------------------------------------ */
    if (f < 0x80 || f >= 0xF6) {
        in.group = single;
        unsigned len = 1;
        switch (f) {
            case 0x06: case 0x09: case 0x17:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
            case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
            case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
                len = 2; break;
            case 0x08: case 0x0A: case 0x0F: case 0x1A: case 0x1C: case 0x1E:
            case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
            case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
                len = 3; break;
            case 0x0B: case 0x1B: case 0x1D:
                len = 4; break;
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
                len = 5; break;
            case 0xF7:
                len = 6; break;
            default:
                if (f >= 0x50 && f <= 0x57) return false; /* undefined: the interpreter keeps them */
                len = 1; break;
        }
        /* the operand bytes as constants */
        uint8_t o[4] = {0, 0, 0, 0};
        for (unsigned i = 1; i < len; i++)
            if (!byte_at(addr + i, o[i - 1])) return false;
        in.next = addr + len;
        in.fall_to = in.next; /* the early returns below keep it (run checks in.fall) */
        const auto d8 = [](uint8_t v) { return int8_t(v); };
        const auto d16 = [](uint8_t lo, uint8_t hi) { return int16_t(uint16_t(lo | (hi << 8))); };
        switch (f) {
            case 0x07: case 0x0E: case 0x0F: /* RETI, RET, RETD: the stack names the pc */
                in.fall = false;
                return true;
            case 0x05: case 0x1F: /* HALT (the resume stays), the BIOS hook */
                return true;
            case 0x1A: /* JP nn16 */
                in.fall = false;
                in.has_target = true;
                in.target = uint32_t(o[0] | (o[1] << 8));
                return true;
            case 0x1B: /* JP nn24 */
                in.fall = false;
                in.has_target = true;
                in.target = uint32_t(o[0] | (o[1] << 8) | (o[2] << 16));
                return true;
            case 0x1C: /* CALL nn16 */
                in.has_target = true;
                in.target = uint32_t(o[0] | (o[1] << 8));
                return true;
            case 0x1D: /* CALL nn24 */
                in.has_target = true;
                in.target = uint32_t(o[0] | (o[1] << 8) | (o[2] << 16));
                return true;
            case 0x1E: /* CALR d16 */
                in.has_target = true;
                in.target = uint32_t(uint32_t(in.next) + uint32_t(d16(o[0], o[1])));
                return true;
            case 0xF8: case 0xF9: case 0xFA: case 0xFB:
            case 0xFC: case 0xFD: case 0xFE: case 0xFF: /* SWI: the vector names the pc */
                in.fall = false;
                return true;
            default:
                if (f >= 0x60 && f <= 0x6F) { /* JR cc,d8 */
                    in.has_target = true;
                    in.target = uint32_t(uint32_t(in.next) + uint32_t(d8(o[0])));
                    return true;
                }
                if (f >= 0x70 && f <= 0x7F) { /* JRL cc,d16 */
                    in.has_target = true;
                    in.target = uint32_t(uint32_t(in.next) + uint32_t(d16(o[0], o[1])));
                    return true;
                }
                return true; /* everything else falls, no target */
        }
    }

    /* ---- two-byte forms ---------------------------------------------------- */
    if (f >= 0xC6 && f <= 0xE6 && (f & 7) == 6) return false; /* C6/D6/E6: undefined */
    if (f < 0xB0) {
        /* src, the (reg)/(reg+d8) modes: the (mem) bytes precede the op byte */
        in.group = src_g;
        in.size = (f >> 4) & 3;
    } else if (f < 0xC0) {
        in.group = dst_g; /* dst, the same two modes */
    } else if (f == 0xC7 || f == 0xD7 || f == 0xE7) {
        in.group = reg_g; /* the register-code operand byte */
    } else if (f < 0xF0) {
        in.group = reg_g; /* C8-CF/D8-DF/E8-EF: no (mem) bytes */
    } else {
        in.group = dst_g; /* F0-F5, the explicit modes */
    }
    if (in.group == src_g) in.size = (f >> 4) & 3;

    /* the (mem) address: mode-dependent bytes before the second */
    const unsigned mode = f & 7;
    uint8_t o[4] = {0, 0, 0, 0};
    if (f < 0xC0) { /* (reg) and (reg+d8) */
        const unsigned r = f & 7;
        char rc[8];
        snprintf(rc, sizeof rc, "0x%02Xu", bank_code(r));
        if (f & 8) {
            if (!byte_at(addr + 1, o[0])) return false;
            in.mem_expr = std::string("(ngp_code_l(") + rc + ") + (uint32_t)(int8_t)" + hex8(o[0]) + ")";
            in.extra = 2;
            p = addr + 2;
        } else {
            in.mem_expr = std::string("ngp_code_l(") + rc + ")";
            in.extra = 0;
            p = addr + 1;
        }
    } else if (f == 0xC7 || f == 0xD7 || f == 0xE7) {
        if (!byte_at(addr + 1, o[0])) return false;
        in.brc = 1;
        in.rcode = o[0];
        in.extra = 1;
        p = addr + 2;
    } else if (in.group == reg_g) { /* C8-CF/D8-DF/E8-EF: the second follows */
        p = addr + 1;
    } else {
        switch (mode) {
            case 0:
                if (!byte_at(addr + 1, o[0])) return false;
                in.mem_expr = hex8(o[0]);
                in.extra = 2;
                p = addr + 2;
                break;
            case 1:
                if (!byte_at(addr + 1, o[0]) || !byte_at(addr + 2, o[1])) return false;
                in.mem_expr = hex32(o[0] | (o[1] << 8));
                in.extra = 2;
                p = addr + 3;
                break;
            case 2:
                if (!byte_at(addr + 1, o[0]) || !byte_at(addr + 2, o[1]) || !byte_at(addr + 3, o[2]))
                    return false;
                in.mem_expr = hex32(o[0] | (o[1] << 8) | (o[2] << 16));
                in.extra = 3;
                p = addr + 4;
                break;
            case 3: { /* the indexed modes */
                if (!byte_at(addr + 1, o[0])) return false;
                const uint8_t data = o[0];
                char rc[8], ic[8];
                if (data == 0x03 || data == 0x07) {
                    if (!byte_at(addr + 2, o[1]) || !byte_at(addr + 3, o[2])) return false;
                    snprintf(rc, sizeof rc, "0x%02Xu", o[1] & 0xFCu);
                    snprintf(ic, sizeof ic, "0x%02Xu", o[2]);
                    in.mem_expr = data == 0x03
                        ? std::string("(ngp_code_l(") + rc + ") + (uint32_t)(int8_t)ngp_code_b(" + ic + "))"
                        : std::string("(ngp_code_l(") + rc + ") + (uint32_t)(int16_t)ngp_code_w(" + ic + "))";
                    in.extra = 8;
                    p = addr + 4;
                } else if (data == 0x13) { /* undocumented: PC relative */
                    if (!byte_at(addr + 2, o[1]) || !byte_at(addr + 3, o[2])) return false;
                    in.mem_expr = hex32(uint32_t(addr + 4 + uint32_t(int16_t(uint16_t(o[1] | (o[2] << 8))))));
                    in.extra = 8;
                    p = addr + 4;
                } else {
                    snprintf(rc, sizeof rc, "0x%02Xu", data);
                    if (data & 3) {
                        if (!byte_at(addr + 2, o[1]) || !byte_at(addr + 3, o[2])) return false;
                        in.mem_expr = std::string("(ngp_code_l(") + rc + ") + (uint32_t)(int16_t)" +
                                      hex32(o[1] | (o[2] << 8)) + ")";
                        p = addr + 4;
                    } else {
                        in.mem_expr = std::string("ngp_code_l(") + rc + ")";
                        p = addr + 2;
                    }
                    in.extra = 5;
                }
                break;
            }
            case 4: { /* (r32-dec) */
                if (!byte_at(addr + 1, o[0])) return false;
                const unsigned w = o[0] & 3;
                char rc[8];
                snprintf(rc, sizeof rc, "0x%02Xu", o[0] & 0xFCu);
                if (w < 3)
                    in.pre = std::string("ngp_code_l_set(") + rc + ", ngp_code_l(" + rc + ") - " +
                             (w == 0 ? "1u" : w == 1 ? "2u" : "4u") + "); ";
                in.mem_expr = std::string("ngp_code_l(") + rc + ")";
                in.extra = 3;
                p = addr + 2;
                break;
            }
            case 5: { /* (r32-inc): the address is the value before the step */
                if (!byte_at(addr + 1, o[0])) return false;
                const unsigned w = o[0] & 3;
                char rc[8];
                snprintf(rc, sizeof rc, "0x%02Xu", o[0] & 0xFCu);
                if (w < 3)
                    in.pre = std::string("const uint32_t m0_ = ngp_code_l(") + rc +
                             "); ngp_code_l_set(" + rc + ", ngp_code_l(" + rc + ") + " +
                             (w == 0 ? "1u" : w == 1 ? "2u" : "4u") + "); ";
                in.mem_expr = w < 3 ? "m0_" : std::string("ngp_code_l(") + rc + ")";
                in.extra = 3;
                p = addr + 2;
                break;
            }
            case 6:
                return false; /* unreachable (rejected above) */
            case 7:
                return false; /* unreachable (C7/D7/E7 handled above) */
        }
    }

    if (!byte_at(p, in.second)) return false;
    const uint8_t s = in.second;
    bool legal;
    if (in.group == src_g) legal = src_second(s);
    else if (in.group == dst_g) legal = dst_second(s);
    else legal = reg_second(s);
    if (!legal) return false;
    in.next = p + 1 + tail_bytes(in.group, s, in.size);
    in.tail_pc = p + 1; /* the handler fetches the tail bytes from pc onward */
    if (in.next <= addr || in.next > addr + 8) return false;
    { /* the tail bytes must exist (the handler fetches them at pc) */
        for (uint32_t q = p + 1; q < in.next; q++) {
            uint8_t t;
            if (!byte_at(q, t)) return false;
        }
    }

    /* the walk's flow */
    if (in.group == reg_g && s == 0x1C) { /* DJNZ r,d */
        uint8_t d;
        if (!byte_at(in.next - 1, d)) return false;
        in.has_target = true;
        in.target = uint32_t(uint32_t(in.next) + uint32_t(int8_t(d)));
    } else if (in.group == dst_g && s >= 0xD0 && s <= 0xDF) { /* JP cc,(mem) */
        if (in.mem_expr.find('(') == std::string::npos && in.mem_expr != "0") {
            in.has_target = true;
            in.target = uint32_t(strtoul(in.mem_expr.c_str(), nullptr, 0));
        }
    } else if (in.group == dst_g && s >= 0xE0 && s <= 0xEF) { /* CALL cc,(mem) */
        if (in.mem_expr.find('(') == std::string::npos && in.mem_expr != "0") {
            in.has_target = true;
            in.target = uint32_t(strtoul(in.mem_expr.c_str(), nullptr, 0));
        }
    }
    in.fall_to = in.next;
    return true;
}

std::map<uint32_t, Insn> Emitter::run(const EmitOptions& opt, size_t& reached) {
    std::map<uint32_t, Insn> ins;
    std::deque<uint32_t> queue;
    /* the header's entry (LE24 at 0x1C of the image) seeds the walk */
    if (rom_.size() >= 0x20) {
        const uint32_t entry = uint32_t(rom_[0x1C]) | (uint32_t(rom_[0x1D]) << 8) |
                               (uint32_t(rom_[0x1E]) << 16);
        if (off_of(entry) >= 0) queue.push_back(entry);
    }
    reached = 0;
    while (!queue.empty()) {
        const uint32_t at = queue.front();
        queue.pop_front();
        if (ins.count(at)) continue;
        if (opt.max_instructions && ins.size() >= opt.max_instructions) break;
        Insn in;
        if (!decode(at, in)) continue;
        ins[at] = in;
        reached++;
        if (in.fall) queue.push_back(in.fall_to);
        if (in.has_target) queue.push_back(in.target);
    }
    return ins;
}

} // namespace

std::string emit_code(const std::vector<uint8_t>& rom, const EmitOptions& options, EmitStats& stats) {
    Emitter e(rom);
    size_t reached = 0;
    const std::map<uint32_t, Insn> ins = e.run(options, reached);
    stats.reached = reached;
    stats.compiled = ins.size();
    stats.regions = ins.size();
    stats.entries = ins.size();

    std::ostringstream o;
    o << "/* Generated by m0recomp --system ngp. Do not edit.\n"
      << " *\n"
      << " * One function per translated TLCS-900H instruction: the fetch, the\n"
      << " * (mem) addressing and the primary dispatch are folded (the bytes are\n"
      << " * these constants) and the instruction's effects are the interpreter's\n"
      << " * own handler (ngp_cpu.c's ngp_gen_exec / ngp_gen_exec1), cycle for\n"
      << " * cycle — the dispatcher (ngp_step under NGP_NATIVE) enters one per\n"
      << " * call and ticks exactly where the interpreter would.  Code the walk\n"
      << " * cannot prove (WRAM copies, jump tables, undefined opcodes) stays\n"
      << " * with the interpreter: the lookup misses and ngp_step runs it. */\n"
      << "#include \"ngp.h\"\n\n"
      << "int ngp_gen_hint = -1; /* the next entry the flow names, -1: none */\n\n";

    /* the entry index of each provable successor, for the hint the
     * dispatcher follows (one hash search per basic block, not per
     * instruction) */
    std::map<uint32_t, int> index_of;
    {
        int i = 0;
        for (const auto& [addr, in] : ins) index_of[addr] = i++;
    }
    const auto hint_of = [&](const Insn& in) {
        const auto it = in.fall ? index_of.find(in.fall_to) : index_of.find(in.target);
        if (it == index_of.end()) return -1;
        return it->second;
    };

    size_t ri = 0;
    for (const auto& [addr, in] : ins) {
        const int hint = hint_of(in);
        o << "static int32_t r_" << ri++ << "(int32_t budget) { /* " << hex32(addr) << " */\n"
          << "    (void)budget;\n";
        if (in.group == single) {
            o << "    machine.cpu.pc = " << hex32(addr + 1) << ";\n"
              << "    ngp_gen_hint = " << hint << ";\n"
              << "    return ngp_gen_exec1(" << hex8(in.first) << ");\n";
        } else {
            o << "    machine.cpu.pc = " << hex32(in.tail_pc) << ";\n"
              << "    ngp_gen_hint = " << hint << ";\n"
              << "    { " << in.pre << "const uint32_t mem_ = " << in.mem_expr << ";\n"
              << "      return ngp_gen_exec(" << hex8(in.first) << ", " << hex8(in.second) << ", "
              << in.size << ", " << hex8(in.rcode) << ", " << unsigned(in.brc) << ", mem_, "
              << in.extra << ");\n"
              << "    }\n";
        }
        o << "}\n";
    }

    std::vector<const std::pair<const uint32_t, Insn>*> order;
    order.reserve(ins.size());
    for (const auto& it : ins) order.push_back(&it);
    const size_t count = ins.size();
    o << "\ntypedef int32_t (*ngp_region_fn)(int32_t budget);\n"
      << "static const ngp_region_fn region_fn[" << (count ? count : 1) << "] = {";
    for (size_t i = 0; i < count; i++) o << (i % 8 ? " " : "\n    ") << "r_" << i << ",";
    if (!count) o << "0";
    o << "\n};\nstatic const uint32_t entry_pc[" << (count ? count : 1) << "] = {";
    for (size_t i = 0; i < count; i++) o << (i % 8 ? " " : "\n    ") << hex32(order[i]->first) << ",";
    if (!count) o << "0";
    /* open addressing over pc, twice as many slots as entries */
    unsigned bits = 4;
    while ((size_t(1) << bits) < count * 2) bits++;
    const size_t slots = size_t(1) << bits;
    const uint32_t empty = 0xFFFFFFFFu;
    std::vector<uint32_t> slot(slots, empty);
    auto slot_of = [&](uint32_t pc) {
        const uint64_t h = uint64_t(pc) * 0x9E3779B97F4A7C15ull;
        return unsigned((uint32_t(h) ^ uint32_t(h >> 32)) >> (32 - bits));
    };
    for (size_t i = 0; i < count; i++) {
        unsigned h = slot_of(order[i]->first);
        while (slot[h] != empty) h = (h + 1) & (slots - 1);
        slot[h] = uint32_t(i);
    }
    o << "\n};\nstatic const uint32_t entry_slot[" << slots << "] = {";
    for (size_t i = 0; i < slots; i++) o << (i % 8 ? " " : "\n    ") << hex32(slot[i]) << ",";
    o << "\n};\n\n"
      << "int ngp_gen_lookup(uint32_t pc, int hint) {\n"
      << "    if (hint >= 0 && entry_pc[hint] == pc) return hint;\n"
      << "    unsigned h = ((uint32_t)((uint64_t)pc * 0x9E3779B97F4A7C15ull) ^\n"
      << "                  (uint32_t)((uint64_t)pc * 0x9E3779B97F4A7C15ull >> 32)) >> " << (32 - bits) << ";\n"
      << "    for (;;) {\n"
      << "        const uint32_t i = entry_slot[h];\n"
      << "        if (i == 0xFFFFFFFFu) return -1;\n"
      << "        if (entry_pc[i] == pc) return (int)i;\n"
      << "        h = (h + 1u) & " << (slots - 1) << "u;\n"
      << "    }\n}\n\n"
      << "int32_t ngp_gen_enter(int entry) {\n"
      << "    return region_fn[entry](0);\n}\n";
    return o.str();
}

} // namespace m0boy::ngp
