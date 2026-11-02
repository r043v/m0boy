/* Z80 -> C for the SG-1000 runtime.
 *
 * Same structure as the Game Boy META backend: instructions (played ones
 * first, then those reachable from them) are grouped by address into
 * regions, one C function each, with gotos inside and a switch on pc at
 * entry.  CALL becomes a C call followed by a check that the guest returned
 * where expected (otherwise the C stack unwinds to a dispatch loop); every
 * other exit sets pc and returns or tail-calls the target region.  Flags are
 * computed only when a later instruction can read them; cycles and refresh
 * are added in batches.  Instructions without a hand-written lowering use
 * the interpreter's semantics (z80_ops.h) on constant operands. */
#include "m0boy/sg1000_emitter.h"
#include "m0boy/code_scan.h"
#include "z80.h"
#include "m0_msx_layout.h"
#include "m0_msx_bios.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <sstream>

namespace m0boy::sg1000 {
namespace {

constexpr uint8_t FC = 1, FN = 2, FPV = 4, FX = 8, FH = 16, FY = 32, FZ = 64, FS = 128, FALL = 0xff;

enum class Flow { Seq, Jump, JumpCond, Djnz, Call, CallCond, Ret, RetCond, Reti, JumpDyn, Halt, Repeat, Ei, Bad };

constexpr Loc NOLOC = 0xffffffffu;
/* Where control goes: a location, and a slot whose page must be checked at
 * run time (guard_slot < 0: none; loc NOLOC: dispatch). */
struct Dest {
    Loc loc = NOLOC;
    int guard_slot = -1;
    unsigned guard_page = 0;
};

struct Ins {
    uint16_t addr = 0;
    Loc loc = 0;
    z80_decoded_t d{};
    Flow flow = Flow::Seq;
    uint16_t target = 0;
    Dest next_dest, target_dest;
    bool remap = false;              ///< writes a mapper register: leave the region after it
    bool stores = false;             ///< sync_stores: writes memory (not the stack), a sync follows it
    unsigned cycles = 0, taken = 0; ///< not taken (or only) / taken
    uint8_t use = 0, def = 0;
    bool timed = false;              ///< observes machine.cyc or R: flush before
    bool covered = false;
    uint32_t hits = 0;               ///< executions the profile recorded (0: statically reached)
    int region = -1;
    size_t order = 0;
    bool label = false, sw_case = false, loop_head = false;
    bool sync_before = false;        ///< fast mode: a bounded sync (BSYNC) here
    bool sync_forced = false;        ///< ... where flags are live: they are kept
    unsigned sync_run = 0;           ///< T-states since a sync on the forward jumps here
    uint8_t live_in = 0, live_out = 0;
    uint16_t next() const { return static_cast<uint16_t>(addr + d.length); }
    bool falls() const {
        return flow == Flow::Seq || flow == Flow::JumpCond || flow == Flow::Djnz || flow == Flow::Call ||
               flow == Flow::CallCond || flow == Flow::RetCond || flow == Flow::Halt || flow == Flow::Repeat ||
               flow == Flow::Ei;
    }
    bool calls() const { return flow == Flow::Call || flow == Flow::CallCond; }
    bool jumps() const { return flow == Flow::Jump || flow == Flow::JumpCond || flow == Flow::Djnz; }
};

std::string hex(unsigned v, int w = 4) {
    std::ostringstream o;
    o << "0x" << std::hex << std::setw(w) << std::setfill('0') << v;
    return o.str();
}
std::string lab(Loc a) {
    std::ostringstream o;
    o << "L_" << std::hex << std::setw(a > 0xffff ? 6 : 4) << std::setfill('0') << a;
    return o.str();
}
std::string hexloc(Loc a) { return a > 0xffff ? hex(a, 6) : hex(a); }

/* The simulated BIOS routine a CALL to this address runs (msx.c: the stub is
 * OUT (id),A ; RET), when it can run without the stub: -1 if none. */
static int msx_bios_direct(unsigned address) {
#define X(name, a, id, direct) if (direct && address == (a)) return (id);
    M0_MSX_BIOS(X)
#undef X
    return -1;
}

/* ---- sandbox: the interpreter measures cycles and flow ------------------ */
struct Sandbox {
    std::span<const uint8_t> rom;
    const std::vector<uint8_t>* ram = nullptr; ///< C000-DFFF snapshot (--ram-image)
    bool banked = false, fixed_kib = true;
    bool msx = false;       ///< zero-based image of the ROM window only
    unsigned rom_start = 0;
    unsigned bank_shift = 14;
    unsigned page = 0;      ///< page of the decoded instruction's slot
    int slot = -1;          ///< its slot (-1: fixed area)
    int defaults[6] = { 0, 1, 2, 3, 4, 5 };
    bool wrote = false;     ///< the measured instruction wrote memory
    uint16_t pc = 0;        ///< the instruction being decoded or measured: its bytes are code
    uint8_t ram_byte(uint16_t a) const {
        return ram && a >= 0xc000 && a < 0xc000 + ram->size() ? (*ram)[a - 0xc000] : 0;
    }
    /* The snapshot's bytes feed the RAM code's own bus (fetch and data: the
     * interpreter runs it on live RAM, whose frame-N state the snapshot of
     * the same session holds).  A ROM instruction still reads RAM as 0, the
     * model its measured timing always used. */
    bool ram_bus(uint16_t a) const {
        return pc >= 0xc000;
    }
    /* Bus of the decoded instruction: its own slot at its page, the others
     * at their usual pages; RAM reads 0, or the snapshot with --ram-image. */
    uint8_t read(uint16_t a) const {
        const size_t n = rom.size();
        if (msx && !banked) return a >= 0xc000 ? 0 : a >= rom_start && a < n ? rom[a] : 0xff;
        if (msx && a < rom_start) return a >= 0xc000 ? 0 : 0xff; /* the BIOS */
        if (!banked) {
            if (a >= 0xc000) return ram_bus(a) ? ram_byte(a) : 0;
            if (n && !(n & (n - 1))) return rom[a & (n - 1)];
            return a < n ? rom[a] : 0xff;
        }
        if (a >= 0xc000) return ram_bus(a) ? ram_byte(a) : 0;
        if (a < 0x400 && fixed_kib) return a < n ? rom[a] : 0xff;
        const int s = a >> bank_shift;
        const unsigned page = s == slot ? this->page : static_cast<unsigned>(defaults[s] < 0 ? s : defaults[s]);
        const size_t off = (static_cast<size_t>(page) << bank_shift) + (a & ((1u << bank_shift) - 1));
        return n ? rom[off % n] : 0xff;
    }
};
uint8_t sb_read(void* u, uint16_t a) { return static_cast<Sandbox*>(u)->read(a); }
void sb_write(void* u, uint16_t, uint8_t) { static_cast<Sandbox*>(u)->wrote = true; }
uint8_t sb_in(void*, uint16_t) { return 0xff; }
void sb_out(void*, uint16_t, uint8_t) {}

/* ---- Z80 adapter of the shared speculative sweep (m0boy/code_scan.h) ---- */
struct Z80ScanCtx {
    Sandbox* sb;
    const EmitOptions* opt;
    size_t rom_size;
    unsigned page;      ///< page of the region (banked), 0 otherwise
    int slot;           ///< slot whose view the region decodes in (banked), -1 unbanked
    uint16_t addr_base; ///< CPU address of the region start
};

/* Known hardware: PSG 40-7F, VDP 80-BF, controller ports C0-FF on every
 * board; the Game Gear registers 00-06; the Master System I/O 3E/3F.  An
 * access to a known port is evidence of code; writing ROM is evidence of
 * data. */
static int z80_port_signal(unsigned port, const EmitOptions& opt) {
    if (opt.msx) return port == 0x98 || port == 0x99 || (port >= 0xa0 && port <= 0xa2) || (port >= 0xa8 && port <= 0xab);
    if (port >= 0x40) return 1;
    if (opt.mastersystem) return (port == 0x3e || port == 0x3f) ? 1 : 0;
    if (opt.banked && port <= 0x06) return 1;
    return 0;
}

static int z80_store_signal(uint16_t a, const EmitOptions& opt) {
    if (opt.msx) return a >= 0x10000u - opt.msx_ram ? 1 : 0;
    if (a < 0x8000) return -1;         /* writing ROM never happens */
    if (a >= 0xc000) return 1;         /* RAM */
    return opt.cart_ram ? 1 : 0;       /* 8000-BFFF: cartridge RAM when enabled */
}

static int z80_target_signal(uint16_t a, const EmitOptions& opt, size_t rom_size) {
    if (a >= 0xc000) return 0;         /* RAM code: the dispatcher handles it */
    if (opt.msx && a < opt.rom_start) return 0; /* the BIOS */
    if (!opt.banked && a >= rom_size) return -1;
    return 0;
}

static bool z80_scan_step(const void* ctxp, size_t pc, size_t end, m0boy::ScanStep* out) {
    const Z80ScanCtx* ctx = static_cast<const Z80ScanCtx*>(ctxp);
    const EmitOptions& opt = *ctx->opt;
    const uint16_t addr = static_cast<uint16_t>(ctx->addr_base + pc);
    auto slot_of = [&](uint16_t a) {
        return (a < 0x400 && opt.fixed_kib) || a >= 0xc000 ? -1 : a >> opt.bank_shift;
    };
    ctx->sb->slot = opt.banked ? slot_of(addr) : -1;
    ctx->sb->page = ctx->page;
    ctx->sb->pc = addr;
    z80_t c{};
    c.user = ctx->sb;
    c.read = sb_read;
    c.pc = addr;
    z80_decoded_t d{};
    if (!z80_decode(&c, &d)) return false;
    if (pc + d.length > end) return false;
    const uint16_t last = static_cast<uint16_t>(addr + d.length - 1);
    if (opt.banked && (slot_of(addr) != slot_of(last) || last < addr)) return false;

    out->length = d.length;
    out->terminator = false;
    out->access = 0;
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    const uint16_t next = static_cast<uint16_t>(addr + d.length);
    if (d.group == 0) {
        if (x == 0 && z == 0 && y >= 2) { /* DJNZ / JR (cc) */
            const uint16_t target = static_cast<uint16_t>(next + static_cast<int8_t>(d.immediate));
            out->access = z80_target_signal(target, opt, ctx->rom_size);
            if (y == 3) out->terminator = true;
        } else if (x == 3) switch (z) {
        case 1:
            if (q && p == 0) out->terminator = true; /* RET */
            if (q && p == 2) out->terminator = true; /* JP (HL)/(IX)/(IY) */
            break;
        case 2: /* JP cc,nn */
            out->access = z80_target_signal(d.immediate, opt, ctx->rom_size);
            break;
        case 3:
            if (y == 0) { /* JP nn */
                out->terminator = true;
                out->access = z80_target_signal(d.immediate, opt, ctx->rom_size);
            } else if (y == 2 || y == 3) { /* OUT (n),A / IN A,(n) */
                out->access = z80_port_signal(d.immediate, opt);
            }
            break;
        case 4: /* CALL cc,nn */
            out->access = z80_target_signal(d.immediate, opt, ctx->rom_size);
            break;
        case 5:
            if (q && p == 0) out->access = z80_target_signal(d.immediate, opt, ctx->rom_size);
            break;
        default:
            break;
        }
        if (x == 0 && z == 2 && !q && p >= 2) /* LD (nn),HL / LD (nn),A: (BC)/(DE) have no immediate */
            out->access = z80_store_signal(d.immediate, opt);
    } else if (d.group == 2) {
        if (x == 1 && z == 5) out->terminator = true; /* RETI/RETN */
        else if (x == 1 && z <= 1) out->access = 1;   /* IN/OUT (C) */
        else if (x == 1 && z == 3 && !q) out->access = z80_store_signal(d.immediate, opt); /* LD (nn),rr */
    }
    return true;
}

struct Run { unsigned cycles; uint16_t pc; };
Run measure(Sandbox& sb, const z80_decoded_t& d, uint16_t addr, uint8_t f, uint16_t bc) {
    z80_t c{};
    c.user = &sb; c.read = sb_read; c.write = sb_write; c.in = sb_in; c.out = sb_out;
    sb.pc = addr;
    c.pc = addr; c.f = f; c.bc = bc; c.a = 0x5a; c.hl = 0xc100; c.sp = 0xd000;
    unsigned cycles = z80_execute_decoded(&c, &d);
    return { cycles, c.pc };
}

uint8_t cond_flag(unsigned cc) {
    static const uint8_t f[4] = { FZ, FC, FPV, FS };
    return f[cc >> 1];
}
std::string cond_expr(unsigned cc) {
    static const char* names[4] = { "F_Z", "F_C", "F_PV", "F_S" };
    std::string t = std::string("(Z.f & ") + names[cc >> 1] + ")";
    return (cc & 1) ? t : "!" + t;
}

/* Flags read and written; unknown cases read everything and write nothing. */
void flag_info(const z80_decoded_t& d, uint8_t& use, uint8_t& def, bool& timed) {
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    use = def = 0;
    timed = false;
    const uint8_t all_but_c = FALL & ~FC;
    if (d.group == 1) {
        if (x == 0) { def = FALL; if (y == 2 || y == 3) use = FC; }
        else if (x == 1) def = all_but_c;
        return;
    }
    if (d.group == 2) {
        if (x == 1) switch (z) {
        case 0: def = all_but_c; timed = true; return;          /* IN r,(C) */
        case 1: timed = true; return;                            /* OUT (C),r */
        case 2: use = FC; def = FALL; return;                    /* SBC/ADC HL,rr */
        case 4: def = FALL; return;                              /* NEG */
        case 7:
            if (y == 1 || y == 3) timed = true;                  /* LD R,A / LD A,R */
            if (y == 2 || y == 3 || y == 4 || y == 5) def = all_but_c;
            return;
        default: return;
        }
        if (x == 2 && y >= 4 && z <= 3) {
            if (z == 0) def = FH | FPV | FN | FX | FY;
            else if (z == 1) def = all_but_c;
            else { def = FALL; timed = true; }
        }
        return;
    }
    if (x == 1) return;
    if (x == 2) { def = FALL; if (y == 1 || y == 3) use = FC; return; }
    if (x == 0) switch (z) {
    case 0:
        if (y == 1) { use = FALL; def = FALL; }                  /* EX AF,AF' */
        else if (y >= 4) use = cond_flag(y - 4);
        return;
    case 1: if (q) def = FH | FN | FC | FX | FY; return;        /* ADD HL,rr */
    case 4: case 5: def = all_but_c; return;
    case 7:
        switch (y) {
        case 0: case 1: def = FC | FN | FH | FX | FY; return;
        case 2: case 3: use = FC; def = FC | FN | FH | FX | FY; return;
        case 4: use = FC | FH | FN; def = FALL & ~FN; return;   /* DAA */
        case 5: def = FH | FN | FX | FY; return;                 /* CPL */
        case 6: def = FC | FH | FN | FX | FY; return;            /* SCF */
        default: use = FC; def = FC | FH | FN | FX | FY; return; /* CCF */
        }
    default: return;
    }
    switch (z) { /* x == 3 */
    case 0: case 2: case 4: use = cond_flag(y); return;
    case 1: if (!q && p == 3) def = FALL; return;                /* POP AF */
    case 3: if (y == 2 || y == 3) timed = true; return;          /* OUT/IN (n) */
    case 5: if (!q && p == 3) use = FALL; return;                /* PUSH AF */
    case 6: def = FALL; if (y == 1 || y == 3) use = FC; return;
    default: return;
    }
}

class Emitter {
public:
    Emitter(std::span<const uint8_t> rom, const EmitOptions& opt) : rom_(rom), opt_(opt) {
        /* a sync per instruction makes large functions very costly to compile */
        if (opt_.precise) opt_.max_region_instructions = std::min<size_t>(opt_.max_region_instructions, 32);
        sb_.rom = rom;
        sb_.ram = opt.ram.empty() ? nullptr : &opt.ram;
        sb_.banked = opt.banked;
        sb_.fixed_kib = opt.fixed_kib;
        sb_.bank_shift = opt.bank_shift;
        sb_.msx = opt.msx;
        sb_.rom_start = opt.rom_start;
    }
    std::string run(const std::vector<HotLoc>& hot, EmitStats& stats);

private:
    std::span<const uint8_t> rom_;
    EmitOptions opt_;
    Sandbox sb_;
    std::map<Loc, Ins> ins_;
    std::vector<std::vector<Loc>> regions_;
    int defaults_[6] = { -1, -1, -1, -1, -1, -1 }; ///< usual page of each slot (banked), -1: unknown
    uint32_t pend_cy_ = 0, pend_rf_ = 0;
    /* Sites of the instruction being emitted expand the out-of-line helpers
     * (sg_gen.h) when it ran this hot: PUSH/POP/RD/WR are the same bodies
     * without the call; flash pays a few bytes per site, and the budget
     * decides how many instructions fit. */
    uint32_t hot_threshold_ = 0xffffffffu;
    uint32_t cur_hits_ = 0;
    bool hot_site() const { return cur_hits_ >= hot_threshold_; }
    std::string rd_expr(const std::string& a) const { return std::string(hot_site() ? "RD(" : "sg_rd(") + a + ")"; }
    std::string wr_stmt(const std::string& a, const std::string& v) const {
        return std::string(hot_site() ? "WR(" : "sg_wr(") + a + ", " + v + ")";
    }
    std::string push_stmt(const std::string& v) const { return std::string(hot_site() ? "PUSH(" : "sg_push(") + v + ")"; }
    std::string pop_expr() const { return hot_site() ? "POP()" : "sg_pop()"; }
    EmitStats* stats_ = nullptr;

    static uint16_t addr_of(Loc l) { return static_cast<uint16_t>(l); }
    static unsigned page_of(Loc l) { return l >> 16; }
    int slot_of(uint16_t a) const {
        if (opt_.msx && a < opt_.rom_start) return -1;
        return (a < 0x400 && opt_.fixed_kib) || a >= 0xc000 ? -1 : a >> opt_.bank_shift;
    }
    /* ROM byte at a location (bus semantics of the runtime); the RAM
     * snapshot's bytes with --ram-image */
    uint8_t rom_at(Loc l) const {
        const uint16_t a = addr_of(l);
        if (!opt_.banked) return sb_.read(a);
        if (a >= 0xc000) return sb_.ram_byte(a);
        const size_t n = rom_.size();
        if (a < 0x400 && opt_.fixed_kib) return a < n ? rom_[a] : 0xff;
        const size_t off = (static_cast<size_t>(page_of(l)) << opt_.bank_shift) + (a & ((1u << opt_.bank_shift) - 1));
        return n ? rom_[off % n] : 0xff;
    }
    bool in_rom(Loc l) const {
        const uint16_t a = addr_of(l);
        if (a >= 0xc000) return sb_.ram && a < 0xc000 + sb_.ram->size(); /* compiled RAM with --ram-image */
        if (opt_.msx && a < opt_.rom_start) return false;
        if (!opt_.banked) return a < rom_.size();
        if (a < 0x400 && opt_.fixed_kib) return a < rom_.size();
        return (static_cast<size_t>(page_of(l)) << opt_.bank_shift) < rom_.size();
    }
    /* Constant reads that can be folded: unbanked ROM, or the fixed first KiB */
    bool rom_const(uint16_t a) const {
        if (opt_.msx && !opt_.banked) return a >= opt_.rom_start && a < rom_.size();
        if (opt_.banked) return opt_.fixed_kib && a < 0x400 && a < rom_.size();
        return a < (opt_.cart_ram ? 0x8000u : 0xc000u);
    }
    Dest resolve(const Ins& from, uint16_t a) const;
    bool decode(Loc loc, Ins& in);
    bool remaps(uint16_t pc, uint16_t a, unsigned n) const;
    Ins* find(Loc a) { auto it = ins_.find(a); return it == ins_.end() ? nullptr : &it->second; }
    void plan();
    void flush(std::ostringstream& o) {
        if (pend_cy_ || pend_rf_) {
            if (pend_rf_) o << "    ADV(" << pend_cy_ << ", " << pend_rf_ << ");\n";
            else o << "    CY(" << pend_cy_ << ");\n";
        }
        pend_cy_ = pend_rf_ = 0;
    }
    std::string region_name(int r) const { return "r_" + std::to_string(r); }
    std::string guard(const Dest& d) const {
        return "machine.page[" + std::to_string(d.guard_slot) + "] == " + std::to_string(d.guard_page);
    }
    /* still in the caller's page after a call or an event (banked slots) */
    std::string page_check(const Ins& in) const {
        const int s = opt_.banked ? slot_of(in.addr) : -1;
        if (s < 0) return "";
        return " || machine.page[" + std::to_string(s) + "] != " + std::to_string(page_of(in.loc));
    }
    void transfer(std::ostringstream& o, const Ins& from, uint16_t target, const Dest& dest);
    void emit_region(std::ostringstream& o, int r);
    void emit_ins(std::ostringstream& o, Ins& in, const Ins* following);
    bool emit_native(std::ostringstream& o, const Ins& in, bool flags);
    void emit_generic(std::ostringstream& o, const Ins& in);
    std::string reg8(unsigned r, const z80_decoded_t& d) const;
    std::string addr_hl(const z80_decoded_t& d) const;
    std::string pair(unsigned p, const z80_decoded_t& d) const;
    std::string mem_read(uint16_t a) const;
    std::string mem_write(uint16_t a, const std::string& v) const;
    static std::string decoded(const z80_decoded_t& d);
};

/* Destination of a transfer to address a from instruction from.  Banked: the
 * fixed area is page 0; the caller's own slot keeps its page; another slot
 * takes its usual page, checked at run time; RAM and unknown pages go
 * through the dispatcher. */
Dest Emitter::resolve(const Ins& from, uint16_t a) const {
    Dest d;
    if (!opt_.banked) { if (a < 0xc000 && a >= opt_.rom_start) d.loc = a; return d; }
    if (a >= 0xc000) return d;
    if (opt_.msx && a < opt_.rom_start) return d; /* the BIOS: the interpreter */
    if (a < 0x400 && opt_.fixed_kib) { d.loc = a; return d; }
    const int s = a >> opt_.bank_shift;
    if (slot_of(from.addr) == s) { d.loc = (Loc(page_of(from.loc)) << 16) | a; return d; }
    if (defaults_[s] < 0) return d;
    d.loc = (Loc(defaults_[s]) << 16) | a;
    d.guard_slot = s;
    d.guard_page = static_cast<unsigned>(defaults_[s]);
    return d;
}

/* A constant store of n bytes at a that pages the slot of the code at pc:
 * FFFD slot 0, FFFE slot 1, FFFF and FFFC (cartridge RAM) slot 2;
 * Codemasters: 0000, 4000, 8000. */
bool Emitter::remaps(uint16_t pc, uint16_t a, unsigned n) const {
    if (opt_.msx && opt_.banked) { /* a bank register, from code in any window */
        for (unsigned k = 0; k < n; k++) if (m0_msx_is_register(opt_.msx_mapper, static_cast<uint16_t>(a + k))) return true;
        return false;
    }
    const int s = opt_.banked ? slot_of(pc) : -1;
    if (s < 0) return false;
    for (unsigned k = 0; k < n; k++) {
        const uint16_t b = static_cast<uint16_t>(a + k);
        if (opt_.bank_shift == 13) { if (b == 0xfff3 || b == 0xfffc) return true; continue; }
        if (!opt_.fixed_kib) { if (b < 0xc000 && !(b & 0x3fff) && (b >> 14) == s) return true; continue; }
        if (b < 0xfffc) continue;
        if ((b == 0xfffd ? 0 : b == 0xfffe ? 1 : 2) == s) return true;
    }
    return false;
}

bool Emitter::decode(Loc loc, Ins& in) {
    const uint16_t addr = addr_of(loc);
    if (!in_rom(loc)) return false;
    sb_.slot = opt_.banked ? slot_of(addr) : -1;
    sb_.page = page_of(loc);
    for (int s = 0; s < (0xc000 >> opt_.bank_shift); s++) sb_.defaults[s] = defaults_[s];
    z80_t c{};
    c.user = &sb_; c.read = sb_read;
    sb_.pc = addr;
    c.pc = addr;
    in.addr = addr;
    in.loc = loc;
    if (!z80_decode(&c, &in.d)) return false;
    if (!opt_.banked && addr + in.d.length > rom_.size()) return false;
    /* banked: an instruction must not straddle two slots (or leave the fixed KiB) */
    const uint16_t last = static_cast<uint16_t>(addr + in.d.length - 1);
    if (opt_.banked && (slot_of(addr) != slot_of(last) || last < addr)) return false;
    const z80_decoded_t& d = in.d;
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    flag_info(d, in.use, in.def, in.timed);
    const uint16_t next = in.next();
    if (d.group == 0) {
        if (x == 0 && z == 0 && y >= 2) {
            in.flow = y == 2 ? Flow::Djnz : y == 3 ? Flow::Jump : Flow::JumpCond;
            in.target = static_cast<uint16_t>(next + static_cast<int8_t>(d.immediate));
        } else if (op == 0x76) in.flow = Flow::Halt;
        else if (x == 3) switch (z) {
        case 0: in.flow = Flow::RetCond; break;
        case 1: if (q && p == 0) in.flow = Flow::Ret; else if (q && p == 2) in.flow = Flow::JumpDyn; break;
        case 2: in.flow = Flow::JumpCond; in.target = d.immediate; break;
        case 3: if (y == 0) { in.flow = Flow::Jump; in.target = d.immediate; } else if (y == 7) in.flow = Flow::Ei; break;
        case 4: in.flow = Flow::CallCond; in.target = d.immediate; break;
        case 5: if (q && p == 0) { in.flow = Flow::Call; in.target = d.immediate; } break;
        case 7: in.flow = Flow::Call; in.target = static_cast<uint16_t>(y * 8); break;
        default: break;
        }
        /* LD (nn),A / LD (nn),HL onto the mapper register of this code's slot */
        if (x == 0 && z == 2 && !q && p >= 2) in.remap = remaps(addr, d.immediate, p == 2 ? 2 : 1);
    } else if (d.group == 2) {
        if (x == 1 && z == 5) in.flow = Flow::Reti;
        else if (x == 2 && y >= 6 && z <= 3) in.flow = Flow::Repeat;
        if (x == 1 && z == 3 && !q) in.remap = remaps(addr, d.immediate, 2); /* LD (nn),rr */
    }
    in.next_dest = resolve(in, next);
    if (in.jumps() || in.calls()) in.target_dest = resolve(in, in.target);
    /* Cycles from the interpreter: both outcomes of a condition. */
    sb_.wrote = false;
    Run a = measure(sb_, d, addr, 0x00, 0x0101), b = measure(sb_, d, addr, 0xff, 0x0201);
    /* PUSH, CALL, RST and EX (SP),HL write the stack, which does not alias the mapper registers */
    const bool stack_write = d.group == 0 && ((x == 3 && (z == 5 || z == 7)) || op == 0xe3);
    in.stores = opt_.sync_stores && sb_.wrote && !stack_write;
    switch (in.flow) {
    case Flow::JumpCond: case Flow::CallCond: case Flow::RetCond: case Flow::Djnz: {
        const bool a_taken = a.pc != next, b_taken = b.pc != next;
        if (a_taken == b_taken) return false; /* not expected: leave to the interpreter */
        in.taken = a_taken ? a.cycles : b.cycles;
        in.cycles = a_taken ? b.cycles : a.cycles;
        break;
    }
    case Flow::Repeat: in.cycles = 16; in.taken = 21; break;
    default: in.cycles = in.taken = a.cycles; break;
    }
    return true;
}

void Emitter::plan() {
    /* Regions: runs of nearby instructions. */
    const Ins* prev = nullptr;
    for (auto& [a, in] : ins_) {
        /* banked: one slot (and page) per region */
        bool split = !prev || in.loc > prev->loc + prev->d.length + 32 ||
                     (opt_.banked && slot_of(in.addr) != slot_of(prev->addr));
        if (!split && regions_.back().size() >= opt_.max_region_instructions &&
            !(prev->falls() && prev->next_dest.loc == in.loc))
            split = true;
        if (split) regions_.emplace_back();
        in.region = static_cast<int>(regions_.size() - 1);
        in.order = regions_.back().size();
        regions_.back().push_back(a);
        prev = &in;
    }
    for (auto& reg : regions_) { Ins& f = ins_[reg.front()]; f.label = f.sw_case = true; }
    for (Loc v : { 0x0000u, 0x0038u, 0x0066u })
        if (Ins* t = find(v)) t->label = t->sw_case = true;
    for (Loc v : opt_.entries) /* pushed return addresses, jump tables */
        if (Ins* t = find(v)) t->label = t->sw_case = true;
    for (auto& [a, in] : ins_) {
        if (in.falls()) {
            if (Ins* n = find(in.next_dest.loc)) {
                const bool adjacent = n->region == in.region && n->order == in.order + 1;
                if (!adjacent) { n->label = true; if (n->region != in.region) n->sw_case = true; }
                /* resumption points: after a call returns, after HALT, after
                 * the instruction that follows EI, after a mapper write */
                if (in.calls() || in.flow == Flow::Halt || in.remap) n->label = n->sw_case = true;
                if (in.flow == Flow::Ei) if (Ins* m = find(n->next_dest.loc)) m->label = m->sw_case = true;
            }
        }
        if (in.flow == Flow::Repeat) in.label = in.sw_case = in.loop_head = true;
        if (in.jumps() || in.calls()) {
            if (Ins* t = find(in.target_dest.loc)) {
                t->label = true;
                if (in.calls() || t->region != in.region) t->sw_case = true;
                if (!in.calls() && t->region == in.region && t->order <= in.order) t->loop_head = t->sw_case = true;
            }
        }
    }
    /* Flag liveness, global least fixed point over every region: a direct
     * transfer between compiled regions (a goto, a jump or the run into the
     * next region) carries exactly what the target still reads there — the
     * same reasoning as inside one region.  Everything that leaves for the
     * interpreter, the callee side of a call and the return sites keep all
     * flags live. */
    auto liveness = [&]() {
        for (auto& [a, in] : ins_) in.live_in = in.live_out = 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& reg : regions_) {
                for (size_t k = reg.size(); k-- > 0;) {
                    Ins& in = ins_[reg[k]];
                    uint8_t out = 0;
                    if (opt_.precise || in.calls() || in.remap || in.stores || in.flow == Flow::Ei || in.flow == Flow::Bad) out = FALL;
                    if (in.falls()) {
                        Ins* n = find(in.next_dest.loc);
                        out |= n ? n->live_in : FALL;
                    }
                    if (in.jumps()) {
                        Ins* t = find(in.target_dest.loc);
                        out |= t ? t->live_in : FALL;
                    }
                    if (in.flow == Flow::Ret || in.flow == Flow::RetCond || in.flow == Flow::Reti || in.flow == Flow::JumpDyn)
                        out = FALL;
                    /* a forced sync may take an interrupt that pushes AF */
                    const uint8_t live_in = in.sync_forced ? FALL : static_cast<uint8_t>(in.use | (out & ~in.def));
                    if (out != in.live_out || live_in != in.live_in) {
                        in.live_out = out;
                        in.live_in = live_in;
                        changed = true;
                    }
                }
            }
        }
    };
    liveness();
    /* Fast mode: an interrupt waits for the next sync, at loop heads (Ristar's
     * water: a line interrupt taken 185 T-states late, after a loop body,
     * reprogrammed the line counter after its reload; the next frame's
     * display-off came 21 lines early, 12 black lines).  Bounded syncs
     * (BSYNC, SG_BOUNDED_SYNC) keep that delay near sync_cycles: once that
     * many T-states have run since the last sync on the straight path, the
     * next instruction that needs no flag gets one, so that no dead flag has
     * to be computed for it (the interrupt pushes whatever Z.f holds there,
     * a value the code overwrites anyway); past twice the budget the sync is
     * forced and the flags are kept.  Counted along each region in order: a
     * label takes the longest of the path falling into it and the forward
     * jumps to it (backward jumps go to loop heads, which sync). */
    if (!opt_.precise && opt_.sync_cycles) {
        bool forced = false;
        for (auto& reg : regions_) {
            unsigned run = 0;
            for (Loc a : reg) {
                Ins& in = ins_[a];
                if (in.loop_head) run = 0;
                else run = std::max(run, in.sync_run);
                const unsigned cost = std::max(in.cycles, in.taken);
                if (run && run + cost > opt_.sync_cycles &&
                    (in.live_in == 0 || run + cost > 2 * opt_.sync_cycles)) {
                    in.sync_before = true;
                    if (in.live_in) in.sync_forced = forced = true;
                    run = 0;
                }
                run += cost;
                if (in.jumps())
                    if (Ins* t = find(in.target_dest.loc))
                        if (t->region == in.region && t->order > in.order) t->sync_run = std::max(t->sync_run, run);
                if (!in.falls() || in.calls() || in.flow == Flow::Ei || in.flow == Flow::Halt) run = 0;
            }
        }
        if (forced) liveness();
    }
}

std::string Emitter::reg8(unsigned r, const z80_decoded_t& d) const {
    static const char* n[8] = { "Z.b", "Z.c", "Z.d", "Z.e", "Z.h", "Z.l", nullptr, "Z.a" };
    if (r == 6) return rd_expr(addr_hl(d));
    return n[r];
}
std::string Emitter::addr_hl(const z80_decoded_t& d) const {
    if (!d.index) return "Z.hl";
    std::ostringstream o;
    o << "(uint16_t)(" << (d.index == 1 ? "Z.ix" : "Z.iy") << " + " << int(static_cast<int8_t>(d.displacement)) << ")";
    return o.str();
}
std::string Emitter::pair(unsigned p, const z80_decoded_t& d) const {
    if (p == 0) return "Z.bc";
    if (p == 1) return "Z.de";
    if (p == 2) return d.index == 1 ? "Z.ix" : d.index == 2 ? "Z.iy" : "Z.hl";
    return "Z.sp";
}

/* Constant addresses: ROM folded, RAM indexed directly.  Banked: only the
 * fixed KiB folds; paged ROM is read through the map, and the mapper
 * registers (FFFC-FFFF) and the cartridge window are written by the bus. */
std::string Emitter::mem_read(uint16_t a) const {
    if (rom_const(a)) return hex(rom_at(a), 2);
    if (opt_.msx) /* RAM at the top of the address space; the slots decide the rest */
        return a >= 0xc000 && a >= 0x10000u - opt_.msx_ram ? "machine.ram[" + hex(a - (0x10000u - opt_.msx_ram)) + "]" : "RD(" + hex(a) + ")";
    if (opt_.banked) {
        if (a >= 0xc000) return "machine.ram[" + hex(a & 8191u) + "]";
        return "RD(" + hex(a) + ")";
    }
    if (a >= 0xc000) return "machine.ram[" + hex(a & 1023u, 3) + "]";
    return "machine.cart_ram[" + hex(a & 8191u) + "]";
}
std::string Emitter::mem_write(uint16_t a, const std::string& v) const {
    if (opt_.msx)
        return a >= 0xc000 && a >= 0x10000u - opt_.msx_ram ? "machine.ram[" + hex(a - (0x10000u - opt_.msx_ram)) + "] = " + v + ";"
                                                           : "sg_wr(" + hex(a) + ", " + v + ");";
    if (opt_.banked) {
        if (a >= 0xc000 && a < 0xfffc && !(opt_.bank_shift == 13 && a == 0xfff3)) return "machine.ram[" + hex(a & 8191u) + "] = " + v + ";";
        return "sg_wr(" + hex(a) + ", " + v + ");";
    }
    if (rom_const(a)) return "/* ROM */";
    if (a >= 0xc000) return "machine.ram[" + hex(a & 1023u, 3) + "] = " + v + ";";
    return "machine.cart_ram[" + hex(a & 8191u) + "] = " + v + ";";
}
std::string Emitter::decoded(const z80_decoded_t& d) {
    std::ostringstream o;
    o << "{" << d.immediate << ", " << d.length << ", " << unsigned(d.opcode) << ", " << unsigned(d.index) << ", "
      << unsigned(d.group) << ", " << unsigned(d.displacement) << ", " << unsigned(d.refresh) << ", " << d.extra_cycles << "}";
    return o.str();
}

void Emitter::transfer(std::ostringstream& o, const Ins& from, uint16_t target, const Dest& dest) {
    const Ins* t = find(dest.loc);
    if (t && t->region == from.region && dest.guard_slot < 0) { o << "    goto " << lab(dest.loc) << ";\n"; return; }
    o << "    Z.pc = " << hex(target) << ";";
    if (t) {
        if (dest.guard_slot >= 0) o << " if (" << guard(dest) << ")";
        o << " " << region_name(t->region) << "();";
    }
    o << " return;\n";
}

void Emitter::emit_generic(std::ostringstream& o, const Ins& in) {
    const z80_decoded_t& d = in.d;
    o << "    { static const z80_decoded_t d_ = " << decoded(d) << "; (void)sg_op(&d_); }\n";
    stats_->generic++;
}

/* Hand-written lowering of sequential instructions; false: use generic. */
bool Emitter::emit_native(std::ostringstream& o, const Ins& in, bool flags) {
    const z80_decoded_t& d = in.d;
    const unsigned op = d.opcode, x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    const bool ixhl = d.index != 0; /* H/L name IXH/IXL unless memory is involved */
    const uint8_t need = in.def & in.live_out; /* flags read later (all in precise mode) */
    const uint8_t SZXY = FS | FZ | FX | FY;
    auto ok = [&](unsigned r, bool plain) { return !(ixhl && !plain && (r == 4 || r == 5)); };
    std::string s;
    if (d.group == 1) {
        if (d.index && z != 6) return false; /* undocumented register copy */
        const unsigned r = d.index ? 6 : z;
        const std::string v = reg8(r, d);
        if (x == 1) { /* BIT: X/Y from the address high byte when indexed */
            if (!flags) { o << "    /* BIT: flags unused */\n"; return true; }
            const std::string mask = hex(1u << y, 2);
            if (d.index) {
                /* the indexed address feeds both the read and F5/F3: once */
                o << "    { uint16_t a_ = " << addr_hl(d) << "; uint8_t v_ = " << rd_expr("a_")
                  << "; Z.f = (uint8_t)((Z.f & 1) | 0x10 | ((a_ >> 8) & 0x28) | ((v_ & "
                  << mask << ") ? " << (y == 7 ? "0x80" : "0") << " : 0x44)); }\n";
            } else {
                o << "    { uint8_t v_ = " << v << "; Z.f = (uint8_t)((Z.f & 1) | 0x10 | (v_ & 0x28) | ((v_ & "
                  << mask << ") ? " << (y == 7 ? "0x80" : "0") << " : 0x44)); }\n";
            }
            return true;
        }
        if (x == 0) {
            std::string expr;
            if (flags) expr = "sg_rot" + std::to_string(y) + "(V_)";
            else {
                static const char* plain[8] = {
                    "(uint8_t)((V_ << 1) | (V_ >> 7))", "(uint8_t)((V_ >> 1) | (V_ << 7))",
                    "(uint8_t)((V_ << 1) | (Z.f & 1))", "(uint8_t)((V_ >> 1) | ((Z.f & 1) << 7))",
                    "(uint8_t)(V_ << 1)", "(uint8_t)((V_ >> 1) | (V_ & 0x80))",
                    "(uint8_t)((V_ << 1) | 1)", "(uint8_t)(V_ >> 1)" };
                expr = plain[y];
            }
            auto sub = [&](const std::string& val) { std::string e = expr; size_t pos;
                while ((pos = e.find("V_")) != std::string::npos) e.replace(pos, 2, val); return e; };
            if (r == 6) o << "    { uint16_t a_ = " << addr_hl(d) << "; uint8_t m_ = " << rd_expr("a_") << "; " << wr_stmt("a_", sub("m_")) << "; }\n";
            else o << "    { uint8_t m_ = " << v << "; " << v << " = " << sub("m_") << "; }\n";
            return true;
        }
        const std::string mask = hex(1u << y, 2);
        if (r == 6) {
            o << "    { uint16_t a_ = " << addr_hl(d) << "; " << wr_stmt("a_", "(uint8_t)(" + rd_expr("a_") + " " + (x == 2 ? std::string("& ~") : std::string("| ")) + mask + ")") << "; }\n";
        } else {
            o << "    " << reg8(r, d) << (x == 2 ? " &= (uint8_t)~" : " |= ") << mask << ";\n";
        }
        return true;
    }
    if (d.group == 2) {
        if (x == 1 && z == 4) { o << (flags ? "    sg_neg();\n" : "    Z.a = (uint8_t)-Z.a;\n"); return true; }
        if (x == 1 && z == 2) { o << "    sg_hl16(" << q << ", " << pair(p, d) << ");\n"; return true; }
        if (x == 1 && z == 1) { /* OUT (C),r: port C (OUT (C),0 for y 6) */
            o << "    " << opt_.io_out << "(Z.c, " << (y == 6 ? std::string("0") : reg8(y, d)) << ");\n";
            return true;
        }
        if (x == 1 && z == 0) { /* IN r,(C) (IN F,(C) for y 6: flags only) */
            o << "    { uint8_t v_ = " << opt_.io_in << "(Z.c);" << (y == 6 ? "" : " " + reg8(y, d) + " = v_;")
              << (flags ? " Z.f = (uint8_t)((Z.f & 1) | z80_szp(v_));" : "") << " }\n";
            return true;
        }
        if (x == 1 && z == 3) { /* LD (nn),rr / LD rr,(nn) */
            const uint16_t nn = d.immediate, n1 = static_cast<uint16_t>(nn + 1);
            const std::string rr = pair(p, d);
            if (!q) o << "    " << mem_write(nn, "(uint8_t)" + rr) << " " << mem_write(n1, "(uint8_t)(" + rr + " >> 8)") << "\n";
            else o << "    " << rr << " = (uint16_t)(" << mem_read(nn) << " | (" << mem_read(n1) << " << 8));\n";
            return true;
        }
        if (x == 2 && (y == 4 || y == 5) && z == 3) { /* OUTI / OUTD: unrolled VRAM uploads */
            o << (y == 4 ? "    sg_outi();\n" : "    sg_outd();\n");
            return true;
        }
        if (x == 2 && (y == 4 || y == 5) && z == 0 && !flags) { /* LDI / LDD */
            const char* step = y == 4 ? "++" : "--";
            o << "    { uint8_t v_ = " << rd_expr("Z.hl") << "; " << wr_stmt("Z.de", "v_") << "; Z.hl" << step << "; Z.de" << step << "; Z.bc--; }\n";
            return true;
        }
        return false;
    }
    if (x == 1) {
        const bool plain = y == 6 || z == 6;
        if (!ok(y, plain) || !ok(z, plain)) return false;
        if (y == z) return true;
        if (y == 6) o << "    " << wr_stmt(addr_hl(d), reg8(z, d)) << ";\n";
        else o << "    " << reg8(y, d) << " = " << reg8(z, d) << ";\n";
        return true;
    }
    if (x == 2 || (x == 3 && z == 6)) {
        std::string v;
        if (x == 2) { if (!ok(z, z == 6)) return false; v = reg8(z, d); }
        else v = hex(d.immediate & 255, 2);
        static const char* alu[8] = { "sg_add", "sg_adc", "sg_sub", "sg_sbc", "sg_and", "sg_xor", "sg_or", "sg_cp" };
        if (flags && !(need & ~(SZXY | FC))) { /* only S/Z/X/Y and carry read: inline */
            switch (y) {
            case 0: case 2: case 7: /* ADD, SUB, CP */
                if (need & ~(FZ | FC)) break;
                o << "    { uint32_t v_ = (uint32_t)Z.a " << (y ? "-" : "+") << " " << v << "; Z.f = (uint8_t)(((v_ >> 8) & 1) | ((uint8_t)v_ ? 0 : 0x40));"
                  << (y == 7 ? "" : " Z.a = (uint8_t)v_;") << " }\n";
                return true;
            case 4: case 5: case 6: /* AND, XOR, OR: carry cleared */
                o << "    Z.a " << (y == 4 ? "&" : y == 5 ? "^" : "|") << "= " << v << "; Z.f = z80_sz(Z.a);\n";
                return true;
            default: break;
            }
        }
        if (flags) switch (y) { /* all flags, inline (ADC/SBC keep the helper) */
        case 0: case 2: case 7: { /* ADD, SUB, CP (CP: X/Y from the operand) */
            if (!hot_site()) { o << "    " << alu[y] << "(" << v << ");\n"; return true; } /* a call, not 60 bytes, for what is not hot */
            const char* op = y ? "-" : "+";
            o << "    { uint32_t a_ = Z.a, b_ = " << v << ", r_ = a_ " << op << " b_; Z.f = (uint8_t)((r_ & 0x80) | ((uint8_t)r_ ? 0 : 0x40) | ("
              << (y == 7 ? "b_" : "r_") << " & 0x28) | ((a_ ^ b_ ^ r_) & 0x10) | (((" << (y ? "(a_ ^ b_)" : "~(a_ ^ b_)")
              << " & (a_ ^ r_)) >> 5) & 4) | " << (y ? "2 | " : "") << "((r_ >> 8) & 1));" << (y == 7 ? "" : " Z.a = (uint8_t)r_;") << " }\n";
            return true;
        }
        case 4: o << "    Z.a &= " << v << "; Z.f = (uint8_t)(z80_szp(Z.a) | 0x10);\n"; return true;
        case 5: o << "    Z.a ^= " << v << "; Z.f = z80_szp(Z.a);\n"; return true;
        case 6: o << "    Z.a |= " << v << "; Z.f = z80_szp(Z.a);\n"; return true;
        default: o << "    " << alu[y] << "(" << v << ");\n"; return true;
        }
        switch (y) {
        case 0: o << "    Z.a = (uint8_t)(Z.a + " << v << ");\n"; break;
        case 1: o << "    Z.a = (uint8_t)(Z.a + " << v << " + (Z.f & 1));\n"; break;
        case 2: o << "    Z.a = (uint8_t)(Z.a - " << v << ");\n"; break;
        case 3: o << "    Z.a = (uint8_t)(Z.a - " << v << " - (Z.f & 1));\n"; break;
        case 4: o << "    Z.a &= " << v << ";\n"; break;
        case 5: o << "    Z.a ^= " << v << ";\n"; break;
        case 6: o << "    Z.a |= " << v << ";\n"; break;
        default: o << "    /* CP: flags unused */\n"; break;
        }
        return true;
    }
    if (x == 0) switch (z) {
    case 0:
        if (y == 0) return true;
        if (y == 1) {
            o << "    { uint16_t t_ = (uint16_t)((Z.a << 8) | Z.f); Z.a = (uint8_t)(Z.af2 >> 8); Z.f = (uint8_t)Z.af2; Z.af2 = t_; }\n";
            return true;
        }
        return false;
    case 1:
        if (!q) { o << "    " << pair(p, d) << " = " << hex(d.immediate) << ";\n"; return true; }
        {
            const std::string hl = pair(2, d), rr = pair(p, d);
            const uint8_t need = in.def & in.live_out;
            if (!need) o << "    " << hl << " = (uint16_t)(" << hl << " + " << rr << ");\n";
            else if (!(need & ~FC)) /* only the carry is read */
                o << "    { uint32_t v_ = (uint32_t)" << hl << " + " << rr << "; Z.f = (uint8_t)((Z.f & ~1) | (v_ >> 16)); " << hl << " = (uint16_t)v_; }\n";
            else
                o << "    { uint32_t a_ = " << hl << ", b_ = " << rr << ", v_ = a_ + b_; Z.f = (uint8_t)((Z.f & 0xc4) | ((v_ >> 8) & 0x28)"
                     " | (((a_ ^ b_ ^ v_) >> 8) & 0x10) | (v_ >> 16)); " << hl << " = (uint16_t)v_; }\n";
        }
        return true;
    case 2: {
        const uint16_t nn = d.immediate;
        if (p < 2) {
            const std::string a = p ? "Z.de" : "Z.bc";
            if (q) o << "    Z.a = sg_rd(" << a << ");\n"; else o << "    sg_wr(" << a << ", Z.a);\n";
        } else if (p == 2) {
            const uint16_t n1 = static_cast<uint16_t>(nn + 1);
            if (!q) o << "    " << mem_write(nn, "(uint8_t)" + pair(2, d)) << " " << mem_write(n1, "(uint8_t)(" + pair(2, d) + " >> 8)") << "\n";
            else o << "    " << pair(2, d) << " = (uint16_t)(" << mem_read(nn) << " | (" << mem_read(n1) << " << 8));\n";
        } else {
            if (!q) o << "    " << mem_write(nn, "Z.a") << "\n";
            else o << "    Z.a = " << mem_read(nn) << ";\n";
        }
        return true;
    }
    case 3: o << "    " << pair(p, d) << (q ? "--" : "++") << ";\n"; return true;
    case 4: case 5: {
        if (!ok(y, false)) return false;
        const bool dec = z == 5;
        const bool sz = flags && !(need & ~SZXY); /* only S/Z/X/Y read: inline */
        if (y == 6) {
            o << "    { uint16_t a_ = " << addr_hl(d) << "; ";
            if (sz) o << "uint8_t v_ = (uint8_t)(sg_rd(a_) " << (dec ? "- 1" : "+ 1") << "); sg_wr(a_, v_); Z.f = (uint8_t)((Z.f & ~0xe8) | z80_sz(v_)); }\n";
            else if (flags) o << "sg_wr(a_, " << (dec ? "sg_dec" : "sg_inc") << "(sg_rd(a_))); }\n";
            else o << "sg_wr(a_, (uint8_t)(sg_rd(a_) " << (dec ? "- 1" : "+ 1") << ")); }\n";
        } else {
            const std::string r = reg8(y, d);
            if (sz) o << "    " << r << (dec ? "--" : "++") << "; Z.f = (uint8_t)((Z.f & ~0xe8) | z80_sz(" << r << "));\n";
            else if (flags) o << "    " << r << " = " << (dec ? "sg_dec(" : "sg_inc(") << r << ");\n";
            else o << "    " << r << (dec ? "--" : "++") << ";\n";
        }
        return true;
    }
    case 7:
        switch (y) {
        case 0: o << "    { uint8_t r_ = (uint8_t)((Z.a << 1) | (Z.a >> 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x29));" : "") << " }\n"; break;
        case 1: o << "    { uint8_t c_ = Z.a & 1, r_ = (uint8_t)((Z.a >> 1) | (c_ << 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 2: o << "    { " << (flags ? "uint8_t c_ = Z.a >> 7; " : "") << "uint8_t r_ = (uint8_t)((Z.a << 1) | (Z.f & 1)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 3: o << "    { " << (flags ? "uint8_t c_ = Z.a & 1; " : "") << "uint8_t r_ = (uint8_t)((Z.a >> 1) | ((Z.f & 1) << 7)); Z.a = r_;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc4) | (r_ & 0x28) | c_);" : "") << " }\n"; break;
        case 4: o << "    sg_daa();\n"; break;
        case 5: o << "    Z.a = (uint8_t)~Z.a;" << (flags ? " Z.f = (uint8_t)((Z.f & 0xc5) | 0x12 | (Z.a & 0x28));" : "") << "\n"; break;
        case 6: if (flags) o << "    Z.f = (uint8_t)((Z.f & 0xc4) | 0x01 | (Z.a & 0x28));\n"; break;
        default: if (flags) o << "    Z.f = (uint8_t)((Z.f & 0xc4) | ((Z.f & 1) ? 0x10 : 0x01) | (Z.a & 0x28));\n"; break;
        }
        return true;
    case 6:
        if (!ok(y, false)) return false;
        if (y == 6) o << "    " << wr_stmt(addr_hl(d), hex(d.immediate & 255, 2)) << ";\n";
        else o << "    " << reg8(y, d) << " = " << hex(d.immediate & 255, 2) << ";\n";
        return true;
    default: return false;
    }
    /* x == 3, sequential forms */
    switch (z) {
    case 1:
        if (!q) {
            if (p == 3) o << "    { uint16_t v_ = " << pop_expr() << "; Z.a = (uint8_t)(v_ >> 8); Z.f = (uint8_t)v_; }\n";
            else o << "    " << pair(p, d) << " = " << pop_expr() << ";\n";
            return true;
        }
        if (p == 1) {
            o << "    { uint16_t t_ = Z.bc; Z.bc = Z.bc2; Z.bc2 = t_; t_ = Z.de; Z.de = Z.de2; Z.de2 = t_;"
                 " t_ = Z.hl; Z.hl = Z.hl2; Z.hl2 = t_; }\n";
            return true;
        }
        if (p == 3) { o << "    Z.sp = " << pair(2, d) << ";\n"; return true; }
        return false;
    case 3:
        if (y == 2) { o << "    " << opt_.io_out << "(" << hex(d.immediate & 255, 2) << ", Z.a);\n"; return true; }
        if (y == 3) { o << "    Z.a = " << opt_.io_in << "(" << hex(d.immediate & 255, 2) << ");\n"; return true; }
        if (y == 5) { o << "    { uint16_t t_ = Z.de; Z.de = Z.hl; Z.hl = t_; }\n"; return true; }
        if (y == 4) {
            const std::string hl = pair(2, d);
            o << "    { uint16_t t_ = sg_rd16(Z.sp); sg_wr16(Z.sp, " << hl << "); " << hl << " = t_; }\n";
            return true;
        }
        if (y == 6) { o << "    Z.iff1 = Z.iff2 = 0; Z.ei_delay = 0;\n"; return true; }
        return false;
    case 5:
        if (q) return false;
        if (p == 3) o << "    " << push_stmt("(uint16_t)((Z.a << 8) | Z.f)") << ";\n";
        else o << "    " << push_stmt(pair(p, d)) << ";\n";
        return true;
    default: return false;
    }
}

void Emitter::emit_ins(std::ostringstream& o, Ins& in, const Ins* following) {
    const z80_decoded_t& d = in.d;
    const uint16_t next = in.next();
    cur_hits_ = in.hits; /* helper-inlining decision for this instruction */
    if (in.label) {
        flush(o);
        o << lab(in.loc) << ":\n";
        if (in.loop_head && !opt_.precise) o << "    SYNC(" << hex(in.addr) << ");\n";
    }
    if (in.sync_before && !(in.label && in.loop_head)) { flush(o); o << "    BSYNC(" << hex(in.addr) << ");\n"; }
    if (opt_.precise) { flush(o); o << "    SYNC(" << hex(in.addr) << ");\n"; }
    const bool flags = (in.def & in.live_out) != 0;
    if (in.timed) flush(o);
    auto native_or_generic = [&]() {
        if (emit_native(o, in, flags)) { pend_rf_ += d.refresh; stats_->native++; }
        else emit_generic(o, in); /* adds its own refresh */
        pend_cy_ += in.cycles;
    };
    switch (in.flow) {
    case Flow::Seq:
        native_or_generic();
        break;
    case Flow::Bad:
        flush(o);
        o << "    Z.pc = " << hex(in.addr) << "; return;\n";
        return;
    case Flow::Jump:
        if (in.target == in.addr && in.loop_head) {
            /* JR $ / JP $, a game waiting for its interrupt: the loops
             * that run before the next event, all but the last at once
             * (SG_IDLE, sg_gen.h); the last and the label's SYNC as when
             * spinning */
            flush(o);
            o << "    SG_IDLE(" << unsigned(in.cycles) << ", " << unsigned(d.refresh) << ");\n";
            stats_->idle++;
        }
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        transfer(o, in, in.target, in.target_dest);
        return;
    case Flow::JumpCond: case Flow::Djnz: {
        pend_rf_ += d.refresh;
        flush(o);
        if (in.flow == Flow::Djnz) o << "    if (--Z.b) {\n";
        else o << "    if (" << cond_expr(in.d.group == 0 && (d.opcode >> 6) == 0 ? ((d.opcode >> 3) & 7) - 4 : (d.opcode >> 3) & 7) << ") {\n";
        o << "    CY(" << in.taken << ");\n";
        transfer(o, in, in.target, in.target_dest);
        o << "    }\n";
        pend_cy_ += in.cycles;
        break;
    }
    case Flow::Call: case Flow::CallCond: {
        pend_rf_ += d.refresh;
        flush(o);
        const Ins* t = find(in.target_dest.loc);
        if (opt_.msx && !opt_.precise && in.flow == Flow::Call && msx_bios_direct(in.target) >= 0) {
            /* a BIOS routine: what the interpreter would do through the stub (CALL, OUT (id),A, RET)
             * without the stack and the dispatch; the interrupt window between those three
             * instructions goes, so not in precise builds */
            o << "    CY(" << in.taken << "); " << opt_.io_out << "(" << msx_bios_direct(in.target) << ", Z.a); ADV(21, 2);\n";
            break;
        }
        if (in.flow == Flow::CallCond) o << "    if (" << cond_expr((d.opcode >> 3) & 7) << ") {\n";
        o << "    CY(" << in.taken << "); " << push_stmt(hex(next)) << "; Z.pc = " << hex(in.target) << ";\n";
        if (t && in.target_dest.guard_slot >= 0)
            o << "    if (" << guard(in.target_dest) << ") " << region_name(t->region) << "(); else sg_exec();\n";
        else o << "    " << (t ? region_name(t->region) + "()" : std::string("sg_exec()")) << ";\n";
        o << "    if (Z.pc != " << hex(next) << page_check(in) << " || SG_STOPPED()) return;\n";
        if (in.flow == Flow::CallCond) o << "    } else CY(" << in.cycles << ");\n";
        break;
    }
    case Flow::Ret:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = " << pop_expr() << "; return;\n";
        return;
    case Flow::Reti:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = " << pop_expr() << "; Z.iff1 = Z.iff2; return;\n";
        return;
    case Flow::RetCond:
        pend_rf_ += d.refresh;
        flush(o);
        o << "    if (" << cond_expr((d.opcode >> 3) & 7) << ") { CY(" << in.taken << "); Z.pc = " << pop_expr() << "; return; }\n";
        pend_cy_ += in.cycles;
        break;
    case Flow::JumpDyn:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.pc = " << pair(2, d) << "; return;\n";
        return;
    case Flow::Halt:
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.halted = 1; Z.pc = " << hex(next) << "; sg_halt(); if (sg_event(" << hex(next) << ")) return;\n";
        break;
    case Flow::Repeat: {
        /* label + SYNC above.  Fast builds run LDIR/LDDR/OTIR/OTDR as C loops
         * (their flags unread), still interruptible between iterations. */
        flush(o);
        const unsigned op = d.opcode, y = (op >> 3) & 7, z = op & 7;
        const bool block_flags = (in.def & in.live_out) != 0;
        if (!opt_.precise && (z == 0 || z == 3)) {
            const char* step = (y & 1) ? "--" : "++";
            o << "    for (;;) {\n";
            if (z == 0) { /* flags (if read) as LDI's with BC 0: P/V clear, X/Y from A + value */
                o << "        uint8_t v_ = " << rd_expr("Z.hl") << "; " << wr_stmt("Z.de", "v_") << "; Z.hl" << step << "; Z.de" << step << "; Z.bc--;\n"
                  << "        if (!Z.bc) { ADV(16, " << unsigned(d.refresh) << ");"
                  << (block_flags ? " { uint8_t n_ = (uint8_t)(Z.a + v_); Z.f = (uint8_t)((Z.f & 0xc1) | (n_ & 0x08) | ((n_ & 0x02) << 4)); }" : "")
                  << " break; }\n";
            } else {
                /* flags (if read) as OUTI's with B 0 */
                o << "        uint8_t v_ = " << rd_expr("Z.hl") << "; Z.b--; Z.hl" << step << "; " << opt_.io_out << "(Z.c, v_);\n"
                  << "        if (!Z.b) { ADV(16, " << unsigned(d.refresh) << ");"
                  << (block_flags ? " { unsigned s_ = v_ + Z.l; Z.f = (uint8_t)(0x40 | (v_ & 0x80 ? 2 : 0) | (s_ > 255 ? 0x11 : 0) | z80_parity(s_ & 7)); }" : "")
                  << " break; }\n";
            }
            o << "        ADV(21, " << unsigned(d.refresh) << ");\n"
              << "        if (EVT() && sg_event(" << hex(in.addr) << ")) return;\n    }\n";
            stats_->native++;
            break;
        }
        o << "    Z.pc = " << hex(in.addr) << ";\n";
        o << "    { static const z80_decoded_t d_ = " << decoded(d) << "; CY(sg_op(&d_)); }\n";
        o << "    if (Z.pc == " << hex(in.addr) << ") goto " << lab(in.loc) << ";\n";
        stats_->generic++;
        break;
    }
    case Flow::Ei: {
        /* The next instruction runs in the interpreter, which ends the EI
         * delay and forces a sync; the generated code resumes after it. */
        pend_cy_ += in.cycles; pend_rf_ += d.refresh;
        flush(o);
        o << "    Z.iff1 = Z.iff2 = 1; Z.ei_delay = 1;\n";
        const Ins* n = in.next_dest.guard_slot < 0 ? find(in.next_dest.loc) : nullptr;
        if (!n) { o << "    Z.pc = " << hex(next) << "; return;\n"; return; }
        const uint16_t after = n->next();
        if (n->flow == Flow::Jump || n->flow == Flow::Ret || n->flow == Flow::Reti) {
            /* EI; JP/JR/RET/RETI: the delay ends with the transfer (the idle
             * loop EI / JP $-1 and the handler end EI / RETI) */
            if (opt_.precise) o << "    SYNC(" << hex(next) << ");\n";
            pend_cy_ += n->cycles; pend_rf_ += n->d.refresh;
            flush(o);
            o << "    EI_DONE();\n";
            if (n->flow == Flow::Jump) {
                if (!opt_.precise) o << "    SYNC(" << hex(n->target) << ");\n";
                transfer(o, in, n->target, n->target_dest);
            } else {
                o << "    Z.pc = " << pop_expr() << ";" << (n->flow == Flow::Reti ? " Z.iff1 = Z.iff2;" : "") << " return;\n";
            }
            stats_->native++;
            return;
        }
        if (n->flow == Flow::Seq) {
            /* EI; N; ...: N inline, then the delay ends and a sync takes a
             * pending interrupt before the instruction after N (the usual
             * EI / NOP / DI window costs no interpreter step). */
            if (opt_.precise) o << "    SYNC(" << hex(next) << ");\n";
            if (n->timed) flush(o);
            if (emit_native(o, *n, (n->def & n->live_out) != 0)) { pend_rf_ += n->d.refresh; stats_->native++; }
            else emit_generic(o, *n);
            pend_cy_ += n->cycles;
            flush(o);
            o << "    EI_DONE();\n";
            if (!opt_.precise) o << "    SYNC(" << hex(after) << ");\n";
            transfer(o, in, after, n->next_dest);
            return;
        }
        o << "    Z.pc = " << hex(next) << "; if (!sg_step(" << hex(after) << ")) return;\n";
        o << "    SYNC(" << hex(after) << ");\n";
        transfer(o, in, after, n->next_dest);
        return;
    }
    }
    /* A store that may have moved the windows: the sync takes the dispatcher back if so. */
    if (in.stores && !opt_.precise && in.flow == Flow::Seq && !in.remap) { flush(o); o << "    SYNC(" << hex(next) << ");\n"; }
    /* Fall through to the next instruction, inline or by transfer. */
    if (in.remap) { /* the mapper may have moved this code: back to the dispatcher */
        flush(o);
        o << "    Z.pc = " << hex(next) << "; return;\n";
        return;
    }
    if (!following || following->loc != in.next_dest.loc || in.next_dest.guard_slot >= 0) {
        flush(o);
        transfer(o, in, next, in.next_dest);
    }
}

void Emitter::emit_region(std::ostringstream& o, int r) {
    const auto& reg = regions_[static_cast<size_t>(r)];
    pend_cy_ = pend_rf_ = 0;
    o << "\nvoid " << region_name(r) << "(void) {\n    ENTER();\n";
    if (!opt_.precise) o << "    SYNC(Z.pc);\n";
    o << "    switch (Z.pc) {\n";
    for (Loc a : reg) if (ins_[a].sw_case) o << "    case " << hex(addr_of(a)) << ": goto " << lab(a) << ";\n";
    o << "    default: return;\n    }\n";
    /* an instruction with no label after an exit is text GCC would throw
     * away: do not emit it (the next label always restarts a live path) */
    bool falls_in = true;
    for (size_t k = 0; k < reg.size(); k++) {
        Ins& in = ins_[reg[k]];
        const Ins* following = k + 1 < reg.size() ? &ins_[reg[k + 1]] : nullptr;
        if (!falls_in && !in.label) continue;
        /* an Ei/Ret/Jump... ends the straight line: the next one needs a label */
        emit_ins(o, in, following);
        /* Flow::Ei inlines the instruction after it (EI;RET...): the run
         * never falls into it */
        falls_in = in.flow != Flow::Ei && in.falls();
    }
    flush(o);
    o << "}\n";
}

std::string Emitter::run(const std::vector<HotLoc>& hot, EmitStats& stats) {
    stats_ = &stats;
    if (opt_.banked) { /* usual page of each slot: the most played one */
        std::map<unsigned, uint64_t> count[6];
        for (const HotLoc& h : hot) { const int s = slot_of(addr_of(h.loc)); if (s >= 0) count[s][page_of(h.loc)] += h.hits; }
        for (int s = 0; s < (0xc000 >> opt_.bank_shift); s++) {
            uint64_t best = 0;
            defaults_[s] = opt_.msx ? (s >= 2 ? static_cast<int>(m0_msx_initial_unit(opt_.msx_mapper, static_cast<unsigned>(s))) : -1) : s; /* power-on mapping */
            for (auto& [page, n] : count[s]) if (n > best) { best = n; defaults_[s] = static_cast<int>(page); }
        }
    }
    /* Played instructions first (hottest first), then what they reach. */
    std::deque<Loc> work;
    auto add = [&](Loc a, bool covered, uint32_t hits) {
        if (ins_.size() >= opt_.max_instructions || a == NOLOC || ins_.count(a)) return;
        Ins in;
        if (!decode(a, in)) return;
        in.covered = covered;
        in.hits = hits;
        ins_[a] = in;
        work.push_back(a);
    };
    for (const HotLoc& h : hot) add(h.loc, true, h.hits);
    /* Sites this hot expand the memory and stack helpers inline (sg_gen.h):
     * the body is the helper's, the call itself is the cost on a Cortex-M0+;
     * within a factor of the hottest execution count, flash permitting. */
    hot_threshold_ = hot.empty() ? 0xffffffffu :
        std::max_element(hot.begin(), hot.end(), [](const HotLoc& a, const HotLoc& b) { return a.hits < b.hits; })->hits / 64 + 1;
    stats.covered = ins_.size();
    /* Static reach stops at padding: four equal bytes 00 (NOP) or FF (RST 38). */
    auto padding = [&](Loc a) {
        const uint8_t v = rom_at(a);
        if (v != 0x00 && v != 0xff) return false;
        for (unsigned k = 1; k < 4; k++) if (rom_at(a + k) != v) return false;
        return true;
    };
    auto reach = [&](Loc a) { if (a != NOLOC && !ins_.count(a) && in_rom(a) && !padding(a)) add(a, false, 0); };
    for (Loc v : { 0x0000u, 0x0038u, 0x0066u }) reach(v);
    if (opt_.msx && opt_.msx_init)
        reach(opt_.banked ? (Loc(m0_msx_initial_unit(opt_.msx_mapper, opt_.msx_init >> 13)) << 16) | opt_.msx_init : Loc(opt_.msx_init));
    auto drain = [&]() {
        while (!work.empty()) {
            Ins in = ins_[work.front()];
            work.pop_front();
            if (in.falls()) reach(in.next_dest.loc);
            if (in.jumps() || in.calls()) reach(in.target_dest.loc);
        }
    };
    drain();
    /* Speculative sweep (the GB aggressive scan): the shared code/data driver
     * classifies the ROM; courses it validates but profiling never played and
     * the walk never reached still compile while the budget lasts, and what
     * it rejects is reported as data zones.  Nothing blocks the reachability
     * walk: the interpreter stays the safety net for anything missed. */
    if (opt_.aggressive_scan || opt_.infer_data_zones) {
        struct SweptRegion {
            m0boy::ScanResult scan;
            unsigned page = 0;
            int slot = -1;
            uint16_t addr_base = 0;
        };
        std::vector<SweptRegion> swept;
        std::vector<m0boy::ScanZone> zones; /* (page << 16 | page offset) keys */
        m0boy::ScanOptions scan_options;
        scan_options.min_instructions = 2;
        scan_options.pointer_lo = opt_.msx ? opt_.rom_start : 0x0000;
        scan_options.pointer_hi = 0xc000;
        const size_t page_bytes = size_t(1) << opt_.bank_shift;
        const size_t page_count = opt_.banked ? (rom_.size() + page_bytes - 1) / page_bytes : 1;
        for (size_t pi = 0; pi < page_count; pi++) {
            const size_t base = opt_.banked ? pi * page_bytes : opt_.msx ? opt_.rom_start : 0;
            const size_t size = opt_.banked ? std::min<size_t>(page_bytes, rom_.size() - base) : rom_.size() - base;
            SweptRegion region;
            region.page = opt_.banked ? static_cast<unsigned>(pi) : 0;
            region.slot = opt_.banked ? (pi == 0 ? 0 : 1) : -1;
            region.addr_base = opt_.banked && pi > 0 ? static_cast<uint16_t>(page_bytes) : opt_.msx ? static_cast<uint16_t>(opt_.rom_start) : 0x0000;
            Z80ScanCtx ctx{&sb_, &opt_, rom_.size(), region.page, region.slot, region.addr_base};
            m0boy::ScanAdapter adapter{&ctx, z80_scan_step};
            m0boy::scan_region(rom_.data() + base, size, adapter, scan_options, region.scan);
            const uint32_t zone_base = opt_.banked ? static_cast<uint32_t>(pi) << 16 : opt_.msx ? opt_.rom_start : 0;
            for (const m0boy::ScanZone& z : m0boy::scan_data_zones(region.scan, 512)) {
                zones.push_back({zone_base + z.start, zone_base + z.end, z.dominant});
                stats.data_bytes += z.end - z.start;
            }
            swept.push_back(std::move(region));
        }
        stats.data_zones = zones.size();
        if (opt_.aggressive_scan) {
            size_t seeded = 0;
            for (const SweptRegion& region : swept) {
                const m0boy::ScanResult& scan = region.scan;
                for (size_t off = 0; off < scan.klass.size(); ) {
                    if (scan.klass[off] != m0boy::ScanClass::Code) { off++; continue; }
                    const size_t run_start = off;
                    while (off < scan.klass.size() && scan.klass[off] == m0boy::ScanClass::Code) off++;
                    /* seed the run instruction by instruction; the budget and
                     * the decoder decide how much of it compiles */
                    size_t cur = run_start;
                    while (cur < off) {
                        const Loc a = region.page == 0
                            ? static_cast<Loc>(region.addr_base + cur)
                            : (static_cast<Loc>(region.page) << 16) |
                                  static_cast<uint16_t>(region.addr_base + cur);
                        if (ins_.count(a)) break;
                        if (ins_.size() >= opt_.max_instructions) break;
                        Ins in;
                        if (!decode(a, in)) break;
                        in.covered = false;
                        in.hits = 0;
                        ins_[a] = in;
                        work.push_back(a);
                        seeded++;
                        if (in.flow == Flow::Ret || in.flow == Flow::Reti ||
                            in.flow == Flow::Jump || in.flow == Flow::JumpDyn)
                            break;
                        cur += in.d.length;
                    }
                }
            }
            stats.swept = seeded;
            drain();
        }
    }
    stats.compiled = ins_.size();
    stats.speculative = stats.compiled - stats.covered;
    stats.locations.clear();
    for (const auto& [a, in] : ins_) stats.locations.push_back(static_cast<uint32_t>(a));
    plan();
    std::ostringstream o;
    o << "/* Generated by m0recomp --system " << (opt_.banked ? "gg" : opt_.msx ? "msx" : "sg1000") << ". Do not edit. */\n#include \"sg_gen.h\"\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n";
    /* noinline: regions call each other; inlining chains of them explodes
     * compile time and flash. */
    for (size_t r = 0; r < regions_.size(); r++)
        o << "void " << region_name(static_cast<int>(r)) << "(void) __attribute__((noinline));\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, static_cast<int>(r));
    /* Entry table: sorted location -> region, direct-mapped cache in front. */
    std::vector<const Ins*> entries;
    for (auto& [a, in] : ins_) if (in.sw_case) entries.push_back(&in);
    stats.regions = regions_.size();
    stats.entries = entries.size();
    const char* key = opt_.banked ? "uint32_t" : "uint16_t";
    /* SG_ENTRY_ATTR (sg_gen.h): flash on the ESP8266, which reads it by
     * 32-bit words only, so not the SG-1000's 16-bit keys. */
    o << "\nstatic const " << key << " entry_pc[" << std::max<size_t>(entries.size(), 1) << "]"
      << (opt_.banked ? " SG_ENTRY_ATTR" : "") << " = {";
    for (size_t i = 0; i < entries.size(); i++)
        o << (i % 12 ? " " : "\n    ") << (opt_.banked ? hex(entries[i]->loc, 6) : hex(entries[i]->addr)) << ",";
    if (entries.empty()) o << "0";
    o << "\n};\nstatic const sg_fn entry_fn[" << std::max<size_t>(entries.size(), 1) << "] SG_ENTRY_ATTR = {";
    for (size_t i = 0; i < entries.size(); i++) o << (i % 8 ? " " : "\n    ") << region_name(entries[i]->region) << ",";
    if (entries.empty()) o << "0";
    /* Open-addressing table over the entry keys: one multiply, usually one probe
     * and no RAM (the lookup used to keep a direct-mapped cache in front of a
     * binary search).  Twice as many slots as entries, a power of two. */
    unsigned bits = 4;
    while ((size_t(1) << bits) < entries.size() * 2) bits++;
    const size_t slots = size_t(1) << bits;
    std::vector<uint32_t> slot(slots, 0xffffffffu);
    for (size_t i = 0; i < entries.size(); i++) {
        const uint32_t key = opt_.banked ? entries[i]->loc : entries[i]->addr;
        size_t h = (static_cast<uint32_t>(key * 0x9e3779b1u)) >> (32 - bits);
        while (slot[h] != 0xffffffffu) h = (h + 1) & (slots - 1);
        slot[h] = static_cast<uint32_t>(i);
    }
    o << "\n};\n#if defined(ESP8266)\ntypedef uint32_t sg_slot_t; /* flash is read by words */\n#else\ntypedef uint16_t sg_slot_t;\n#endif\n"
      << "static const sg_slot_t entry_slot[" << slots << "] SG_ENTRY_ATTR = {";
    for (size_t i = 0; i < slots; i++) o << (i % 12 ? " " : "\n    ") << (slot[i] == 0xffffffffu ? std::string("(sg_slot_t)~0u") : std::to_string(slot[i])) << ",";
    o << "\n};\n"
      << "sg_fn sg_gen_lookup(uint16_t pc) {\n";
    if (opt_.banked && opt_.msx) /* the unit each window shows, while the slots show the cartridge there */
        o << "    if (pc >= 0xc000 || pc < 0x4000 || !(machine.cart_vis >> (pc >> 13) & 1)) return 0;\n"
          << "    const uint32_t loc = (uint32_t)machine.page[pc >> 13] << 16 | pc;\n";
    else if (opt_.banked) { /* the location of pc under the current mapping; RAM compiles with --ram-image */
        o << (opt_.ram.empty() ? std::string("    if (pc >= 0xc000) return 0;\n")
                               : std::string("    if (pc >= 0xc000) { /* the RAM snapshot's code: the plain pc keys it */\n"
                                             "        const uint32_t rloc = pc;\n"
                                             "        unsigned rh = ((uint32_t)rloc * 0x9e3779b1u) >> ") + std::to_string(32 - bits) + ";\n"
                                             "        for (;;) {\n"
                                             "            const sg_slot_t i = entry_slot[rh];\n"
                                             "            if (i == (sg_slot_t)~0u) return 0;\n"
                                             "            if (entry_pc[i] == rloc) return entry_fn[i];\n"
                                             "            rh = (rh + 1) & " + std::to_string(slots - 1) + ";\n"
                                             "        }\n"
                                             "    }\n");
        o << (opt_.fixed_kib ? "    const uint32_t loc = pc < 0x400 ? pc : (uint32_t)machine.page[pc >> " + std::to_string(opt_.bank_shift) + "] << 16 | pc;\n"
                             : "    const uint32_t loc = (uint32_t)machine.page[pc >> " + std::to_string(opt_.bank_shift) + "] << 16 | pc;\n");
    }
    else if (opt_.msx) /* the cartridge, and only while the slots show it there */
        o << "    if (pc >= 0xc000 || pc < " << hex(opt_.rom_start) << ") return 0;\n"
          << "    if (machine.rd_win[pc >> 13] != machine.rom + ((pc & 0xe000u) - " << hex(opt_.rom_start) << ")) return 0;\n"
          << "    const uint16_t loc = pc;\n";
    else
        o << "    const uint16_t loc = pc;\n";
    o << "    unsigned h = ((uint32_t)loc * 0x9e3779b1u) >> " << (32 - bits) << ";\n"
      << "    for (;;) {\n"
      << "        const sg_slot_t i = entry_slot[h];\n"
      << "        if (i == (sg_slot_t)~0u) return 0;\n"
      << "        if (entry_pc[i] == loc) return entry_fn[i];\n"
      << "        h = (h + 1) & " << (slots - 1) << ";\n"
      << "    }\n}\n";
    return o.str();
}

} // namespace

std::string emit_code(std::span<const uint8_t> rom, const std::vector<HotLoc>& hot_first,
                      const EmitOptions& options, EmitStats& stats) {
    Emitter e(rom, options);
    return e.run(hot_first, stats);
}

} // namespace m0boy::sg1000
