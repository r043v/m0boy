/**
 * @file meta_emitter.cpp
 * @brief Coverage-driven compact C generation for the gbl runtime.
 *
 * Execution model (see targets/meta/runtime/gbl.h):
 *  - Covered instructions are grouped by address into regions.  Each region
 *    is one C function whose entry switch accepts every address that can be
 *    reached from outside it; control inside a region uses goto.
 *  - CALL is a C call followed by a return-address check, RET is a C return.
 *    Any mismatch unwinds to the nearest gbl_exec() loop, which continues
 *    from gbr.pc through compiled regions or the interpreter.
 *  - Cycles are accumulated per straight-line run and flushed before control
 *    transfers and IO accesses.  Interrupts are polled at region entry, loop
 *    heads, HALT, and after writes that can raise or unmask one.  The
 *    precise mode polls before every instruction instead, reproducing the
 *    interpreter's timing exactly for differential testing.
 *  - Flags are computed only when a later instruction can observe them.
 */

#include "recompiler/codegen/meta_emitter.h"
#include "m0boy/rom_storage.h"
#include "m0boy/sweep.h"
#include "recompiler/decoder.h"
#include "recompiler/codegen/register_ir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

/* Set by recompiler/CMakeLists.txt from targets/meta/VERSION and
 * targets/consoles/pokitto/VERSION; the target console's is emitted into
 * gbl_config.h so a firmware records which gbrecomp produced it. */
#ifndef GBRECOMP_META_VERSION
#define GBRECOMP_META_VERSION "unknown"
#endif
#ifndef GBRECOMP_ESPBOY_VERSION
#define GBRECOMP_ESPBOY_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_POKITTO_VERSION
#define GBRECOMP_POKITTO_VERSION "unknown"
#endif
#ifndef GBRECOMP_PC_VERSION
#define GBRECOMP_PC_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_ODROID_VERSION
#define GBRECOMP_ODROID_VERSION "0.1.0"
#endif
#ifndef GBRECOMP_AKA_VERSION
#define GBRECOMP_AKA_VERSION "0.1.0"
#endif

namespace gbrecomp {

namespace fs = std::filesystem;

namespace {

constexpr uint8_t FZ = 1, FN = 2, FH = 4, FC = 8, FALL = 15;

/* Base T-cycles: the runtime's table (gbl_cycles.h), one source for the
 * interpreter and this emitter. */
#include "gbl_cycles.h"
static const uint8_t* const kCycles = gbl_cyc_main;

bool is_undefined(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB:
    case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD:
        return true;
    default:
        return false;
    }
}

uint8_t op_length(uint8_t op) {
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x08:
    case 0xC2: case 0xC3: case 0xCA: case 0xD2: case 0xDA:
    case 0xC4: case 0xCC: case 0xCD: case 0xD4: case 0xDC:
    case 0xEA: case 0xFA:
        return 3;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
    case 0xE0: case 0xF0: case 0xE8: case 0xF8: case 0xCB: case 0x10:
        return 2;
    default:
        return 1;
    }
}

enum class Flow { Seq, Jump, JumpCond, JumpDyn, Call, CallCond, Ret, RetCond, Reti, Halt, Bad };

struct Ins {
    uint16_t bank = 0;
    uint16_t addr = 0;
    uint8_t len = 1;
    uint8_t op = 0;
    uint8_t n8 = 0;
    uint16_t n16 = 0;
    uint8_t cycles = 4;
    uint8_t extra = 0;
    Flow flow = Flow::Seq;
    uint16_t target = 0;
    bool entry = false;
    uint8_t use = 0;
    uint8_t def = 0;
    uint8_t live_in = FALL;
    uint8_t live_out = FALL;
    int region = -1;
    size_t order = 0;
    bool label = false;
    bool sw_case = false;
    bool loop_head = false;
    /* Wait-loop fast-forward (see detect_spin_loops).  On the back edge:
     * spin 1 = side-effect-free polling loop, 2 = DEC r delay loop, 3 = DEC rr
     * delay loop (spin_reg is the counter).  spin_head marks the loop head,
     * spin_read the one read whose timing bounds the skip. */
    uint8_t spin = 0;
    uint8_t spin_reg = 0;
    uint16_t spin_period = 0;   /* cycles per iteration (back edge taken) */
    uint16_t spin_read_ofs = 0; /* cycles from the head to the timed read */
    bool spin_head = false;
    bool spin_read = false;
    std::string spin_kind;
    std::vector<size_t> spin_body; /* delay loop: body without the back edge */
    /* Cycles still pending (not yet added to gbr.cyc) when execution falls
     * through into this label; other entries (gotos, the region switch)
     * subtract it so every path agrees.  0 where gbr.cyc must be exact. */
    int carry = 0;
    /* EI whose next instruction is emitted a second time on the EI path,
     * followed by the delayed interrupt poll and a goto to the one after
     * (see emit_ins): the poll then stays out of loops that the next
     * instruction may head. */
    bool ei_dup = false;
    std::string text;

    uint16_t next() const { return static_cast<uint16_t>(addr + len); }
    bool falls() const {
        return flow == Flow::Seq || flow == Flow::JumpCond || flow == Flow::Call ||
               flow == Flow::CallCond || flow == Flow::RetCond || flow == Flow::Halt;
    }
};

uint32_t key(uint16_t bank, uint16_t addr) {
    return (static_cast<uint32_t>(bank) << 16) | addr;
}

std::string hex(unsigned v, int width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%0*x", width, v);
    return buf;
}

std::string hex4(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", v & 0xFFFF);
    return buf;
}

/* 8.3 name of a streamed ROM on the SD card: up to 8 upper-case letters
 * and digits of the project name, extension GB. */
/* "NAME    EXT" as a FAT directory entry holds it. */
std::string name83(std::string base, const std::string& ext) {
    base.resize(8, ' ');
    std::string e = ext;
    e.resize(3, ' ');
    return base + e;
}

std::string stream_name(const std::string& project) {
    std::string n;
    for (char c : project) {
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (n.size() == 8) break;
    }
    return n.empty() ? "ROM" : n;
}

uint8_t cond_use(uint8_t op) {
    return ((op >> 3) & 3) < 2 ? FZ : FC;
}

std::string cond_expr(uint8_t op) {
    switch ((op >> 3) & 3) {
    case 0: return "gbr.zr != 0";
    case 1: return "gbr.zr == 0";
    case 2: return "!gbr.cf";
    default: return "gbr.cf";
    }
}

bool decode(const ROM& rom, uint16_t bank, uint16_t addr, Ins& in) {
    const uint32_t limit = addr < 0x4000 ? 0x4000u : 0x8000u;
    in.bank = bank;
    in.addr = addr;
    in.op = rom.read_banked(bank, addr);
    in.len = op_length(in.op);
    if (static_cast<uint32_t>(addr) + in.len > limit) return false;
    in.n8 = in.len > 1 ? rom.read_banked(bank, static_cast<uint16_t>(addr + 1)) : 0;
    in.n16 = static_cast<uint16_t>(
        in.n8 | (in.len > 2 ? rom.read_banked(bank, static_cast<uint16_t>(addr + 2)) << 8 : 0));
    in.cycles = kCycles[in.op];
    const uint8_t op = in.op;

    if (is_undefined(op)) {
        in.flow = Flow::Bad;
        return true;
    }
    if (op == 0xCB) {
        const uint8_t cb = in.n8;
        const bool mem = (cb & 7) == 6;
        in.cycles = mem ? ((cb >> 6) == 1 ? 12 : 16) : 8;
        if ((cb >> 6) == 0) {
            in.def = FALL;
            if (cb >= 0x10 && cb < 0x20) in.use = FC;
        } else if ((cb >> 6) == 1) {
            in.def = FZ | FN | FH;
        }
        return true;
    }

    switch (op) {
    case 0x18:
        in.flow = Flow::Jump;
        in.target = static_cast<uint16_t>(addr + 2 + static_cast<int8_t>(in.n8));
        break;
    case 0x20: case 0x28: case 0x30: case 0x38:
        in.flow = Flow::JumpCond;
        in.extra = 4;
        in.target = static_cast<uint16_t>(addr + 2 + static_cast<int8_t>(in.n8));
        in.use = cond_use(op);
        break;
    case 0xC3:
        in.flow = Flow::Jump;
        in.target = in.n16;
        break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        in.flow = Flow::JumpCond;
        in.extra = 4;
        in.target = in.n16;
        in.use = cond_use(op);
        break;
    case 0xE9:
        in.flow = Flow::JumpDyn;
        break;
    case 0xCD:
        in.flow = Flow::Call;
        in.target = in.n16;
        in.use = FALL;
        break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        in.flow = Flow::CallCond;
        in.extra = 12;
        in.target = in.n16;
        in.use = FALL;
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        in.flow = Flow::Call;
        in.target = op & 0x38;
        in.use = FALL;
        break;
    case 0xC9:
        in.flow = Flow::Ret;
        break;
    case 0xD9:
        in.flow = Flow::Reti;
        break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        in.flow = Flow::RetCond;
        in.extra = 12;
        in.use = cond_use(op);
        break;
    case 0x76:
        in.flow = Flow::Halt;
        break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        in.def = FZ | FN | FH;
        break;
    case 0x09: case 0x19: case 0x29: case 0x39:
        in.def = FN | FH | FC;
        break;
    case 0xE8: case 0xF8: case 0x07: case 0x0F: case 0xF1:
        in.def = FALL;
        break;
    case 0x17: case 0x1F:
        in.def = FALL;
        in.use = FC;
        break;
    case 0x27:
        in.use = FN | FH | FC;
        in.def = FZ | FH | FC;
        break;
    case 0x2F:
        in.def = FN | FH;
        break;
    case 0x37:
        in.def = FN | FH | FC;
        break;
    case 0x3F:
        in.use = FC;
        in.def = FN | FH | FC;
        break;
    case 0xF5:
        in.use = FALL;
        break;
    default:
        if ((op >= 0x80 && op < 0xC0) || (op >= 0xC6 && (op & 7) == 6)) {
            const unsigned grp = (op >> 3) & 7;
            in.def = FALL;
            if (grp == 1 || grp == 3) in.use = FC;
        }
        break;
    }
    return true;
}

const char* kR8[8] = {"gbr.b", "gbr.c", "gbr.d", "gbr.e", "gbr.h", "gbr.l", nullptr, "gbr.a"};
const char* kR16[4] = {"gbr.bc", "gbr.de", "gbr.hl", "gbr.sp"};

/* ESPboy flash map (4 MiB), as targets/meta/runtime/gbl_espboy.h. */
constexpr size_t kEspApp = 0x100000;       // application: all the CPU maps
constexpr size_t kEspRomBytes = 0x2C0000;  // rom.bin area, from kEspApp (read with spi_flash_read)
constexpr size_t kEspEmbed = 0x40000;      // ROMs up to this size are part of the application
constexpr size_t kEspSaveBlocks = 0x03B000 / 4096;
constexpr size_t kEspRuntime = 0x60000;    // Arduino core + runtime, before generated code

class Generator {
public:
    Generator(const ROM& rom, const MetaTargetOptions& opt) : rom_(rom), opt_(opt) {
        stream_ = !opt.espboy && !opt.pc && !opt.odroid && !opt.aka &&
                  (opt.stream == 1 || (opt.stream < 0 && rom.size() >= 0x40000) || opt.pack);
        pack_ = opt.espboy ? rom.size() > kEspEmbed : opt.pack;
        if (opt.espboy) stream_ = pack_;
    }

    /* ROM streaming (see MetaTargetOptions::stream): plan_stream reads the
     * data-read profiles and splits the flash between compiled code and
     * resident ROM chunks; load_coverage then adds the fetches of code left
     * to the interpreter, and choose_resident picks the hottest chunks. */
    bool streaming() const { return stream_; }
    bool plan_stream(MetaTargetStats& stats, std::string& error);
    void choose_resident(MetaTargetStats& stats);
    std::string emit_rom_full() const;

    bool load_coverage(MetaTargetStats& stats, std::string& error);
    void build();
    std::string emit_code();
    std::string emit_rom() const;
    std::vector<uint8_t> espboy_image() const;
    std::string emit_config(std::string& error) const;
    std::string espboy_state_config(size_t eram, bool battery) const;

    size_t compiled() const { return ins_.size(); }
    size_t dropped() const { return dropped_; }
    size_t regions() const { return regions_.size(); }
    size_t entries() const { return entry_count_; }
    size_t register_blocks() const { return register_blocks_; }
    size_t thumb_blocks() const { return thumb_blocks_; }

private:
    Ins* find(uint16_t bank, uint16_t addr) {
        auto it = index_.find(key(bank, addr));
        return it == index_.end() ? nullptr : &ins_[it->second];
    }
    /* Resolve a static target.  bank_check is set when the target lives in the
     * switchable window and the ROM bank must be verified at run time. */
    Ins* resolve(const Ins& from, uint16_t target, int& bank_check);
    std::string region_name(int r) const;

    void emit_region(std::ostringstream& o, int r);
    void emit_ins(std::ostringstream& o, Ins& in, const Ins* following,
                  const std::string* replacement = nullptr);
    void emit_transfer(std::ostringstream& o, const Ins& from, uint16_t target, bool set_pc,
                       int extra = 0);
    void emit_call(std::ostringstream& o, const Ins& in);
    void emit_alu(std::ostringstream& o, unsigned grp, const std::string& src, bool mem,
                  bool src_is_a, uint8_t live);
    void emit_cb(std::ostringstream& o, const Ins& in);
    std::string const_read(std::ostringstream& o, uint16_t a);
    void const_write(std::ostringstream& o, uint16_t a, const std::string& v, bool& sync_after);
    void flush(std::ostringstream& o) {
        if (pend_) {
            o << "    CY(" << pend_ << ");\n";
            pend_ = 0;
        }
    }
    void detect_spin_loops();
    void emit_spin(std::ostringstream& o, const Ins& in);

    const ROM& rom_;
    const MetaTargetOptions& opt_;
    std::vector<Ins> ins_;
    std::unordered_map<uint32_t, size_t> index_;
    std::vector<std::vector<size_t>> regions_;
    size_t dropped_ = 0;
    size_t entry_count_ = 0;
    size_t register_blocks_ = 0, thumb_blocks_ = 0;
    int pend_ = 0;
    int cur_region_ = -1;
    bool ei_pending_ = false;
    size_t spin_loops_ = 0, counted_loops_ = 0;
    bool replaying_ = false;
    bool stream_ = false, pack_ = false;
    bool partial_pack_ = false;
    size_t flash_data_budget_ = 0;
    std::vector<bool> packed_keep_;
    size_t stream_code_budget_ = 0, resident_budget_ = 0;
    /* Flash for the application: META 240 KiB above its 16 KiB loader,
     * Pokitto 228 KiB below its loader at 0x39000; the PC embeds everything
     * in an executable and knows no budget. */
    size_t app_flash() const {
        return opt_.pc ? SIZE_MAX : opt_.espboy ? kEspApp
             : opt_.aka ? 0x2EE000 /* the AKA's official 3000K app slot */
             : opt_.odroid ? 0x370000
             : opt_.pokitto ? 0x39000 : 245760;
    }
    std::vector<std::vector<uint8_t>> packed_sector_; /* per ROM sector (banks 1+) */
    std::vector<uint64_t> chunk_weight_; /* per 4 KiB ROM chunk */
    std::vector<int> resident_index_;    /* per chunk: slot in flash, -1 on the card */

public:
    size_t spin_loops() const { return spin_loops_; }
    size_t counted_loops() const { return counted_loops_; }
};

bool Generator::load_coverage(MetaTargetStats& stats, std::string& error) {
    /* flags: 1 executed, 2 entered non-sequentially, 4 speculative (from
     * static analysis rather than a play session), 8 proven data (memscan:
     * verbatim copy to VRAM or pointer table — the explorer executed it as
     * code on purpose, m0boy must not compile it). */
    std::map<uint32_t, uint8_t> cov;
    std::set<uint32_t> played;
    std::set<uint32_t> data;
    std::unordered_map<uint32_t, uint64_t> hits; /* optional 4th column */
    for (const fs::path& path : opt_.coverage_files) {
        std::ifstream in(path);
        if (!in) {
            error = "cannot read coverage file " + path.string();
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            unsigned bank = 0, addr = 0, flags = 0, count = 1;
            if (std::sscanf(line.c_str(), "%x %x %x %x", &bank, &addr, &flags, &count) < 3) continue;
            if (addr < 0x4000) bank = 0;
            if (addr >= 0x8000 || bank >= rom_.bank_count()) continue;
            const uint32_t k = key(static_cast<uint16_t>(bank), static_cast<uint16_t>(addr));
            cov[k] |= static_cast<uint8_t>(flags & 3);
            if (flags & 8) data.insert(k);
            /* a proven-data line is not played code: no hit, no streaming
             * weight, no flash-ranking preference */
            if (!(flags & 4) && !(flags & 8)) {
                played.insert(k);
                hits[k] += count ? count : 1;
            }
        }
    }
    stats.covered = cov.size();

    /* --meta-sweep: every bank's CALL ($CD) and JP abs ($C3) offers its
     * target as an entry, the callers behind a bank table included.  A
     * target below $4000 keys under bank 0 (fixed), one in the switchable
     * window under the bank it was read from — the dispatcher checks the
     * mapped bank at run time, so a wrongly assumed bank never runs
     * anything.  The candidates carry the entry flag only (2): no
     * execution count, the budget ranks them with the static analysis's
     * speculative code. */
    if (opt_.sweep) {
        size_t swept = 0;
        for (uint16_t bank = 0; bank < rom_.bank_count() && swept < m0::kSweepDefault; bank++) {
            for (unsigned at = bank ? 0x4000u : 0u; at < 0x8000u && swept < m0::kSweepDefault; at++) {
                if (rom_.read_banked(bank, static_cast<uint16_t>(at)) != 0xCD &&
                    rom_.read_banked(bank, static_cast<uint16_t>(at)) != 0xC3)
                    continue;
                if (at + 2 >= 0x8000u) break;
                const uint16_t to = static_cast<uint16_t>(
                    rom_.read_banked(bank, static_cast<uint16_t>(at + 1)) |
                    rom_.read_banked(bank, static_cast<uint16_t>(at + 2)) << 8);
                const uint32_t k = key(to < 0x4000u ? static_cast<uint16_t>(0) : bank, to);
                if (!cov.count(k)) {
                    cov[k] = 2;
                    swept++;
                }
            }
        }
    }

    std::vector<Ins> all;
    for (const auto& [k, flags] : cov) {
        /* proven data never executed: leave it to the ROM reads entirely */
        if (data.count(k) && !(flags & 1)) {
            stats.data_skipped++;
            continue;
        }
        Ins in;
        if (!decode(rom_, static_cast<uint16_t>(k >> 16), static_cast<uint16_t>(k), in)) {
            dropped_++;
            continue;
        }
        in.entry = (flags & 2) != 0;
        all.push_back(std::move(in));
    }

    /* Flash budget: played code first, hottest first (execution counts from
     * the profile), then the most promising speculative code: loops (spans
     * closed by a backward branch) are likely hot, bank 0 holds the shared
     * routines. */
    size_t budget = opt_.code_budget_bytes;
    if (budget && stream_) budget = std::min(budget, stream_code_budget_);
    if (!budget && stream_) {
        budget = stream_code_budget_;
    } else if (!budget) {
        const size_t app = app_flash(), runtime = opt_.espboy ? kEspRuntime : 40960;
        const size_t rom_flash = rom_.size();
        budget = app > runtime + rom_flash ? app - runtime - rom_flash : 0;
    }
    const size_t max_ins = budget / (opt_.bytes_per_instruction ? opt_.bytes_per_instruction : 20);
    std::vector<uint8_t> keep(all.size(), 1);
    size_t spec = 0;
    for (const Ins& in : all)
        if (!played.count(key(in.bank, in.addr))) spec++;
    stats.speculative = spec;
    if (all.size() > max_ins) {
        std::vector<uint8_t> score(all.size(), 0);
        std::unordered_map<uint32_t, size_t> pos;
        for (size_t i = 0; i < all.size(); i++) pos[key(all[i].bank, all[i].addr)] = i;
        for (size_t i = 0; i < all.size(); i++) {
            const Ins& in = all[i];
            if ((in.flow == Flow::Jump || in.flow == Flow::JumpCond) && in.target <= in.addr &&
                in.addr - in.target < 512 && (in.target < 0x4000) == (in.addr < 0x4000)) {
                auto it = pos.find(key(in.bank, in.target));
                if (it == pos.end()) continue;
                for (size_t j = it->second; j <= i; j++) score[j] |= 2;
            }
            if (in.bank == 0) score[i] |= 1;
        }
        std::vector<size_t> order(all.size());
        for (size_t i = 0; i < all.size(); i++) order[i] = i;
        auto heat = [&](size_t i) -> uint64_t {
            auto it = hits.find(key(all[i].bank, all[i].addr));
            return it == hits.end() ? 0 : it->second;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const bool pa = played.count(key(all[a].bank, all[a].addr)) != 0;
            const bool pb = played.count(key(all[b].bank, all[b].addr)) != 0;
            if (pa != pb) return pa;
            if (pa) return heat(a) > heat(b);
            return score[a] > score[b];
        });
        for (size_t n = 0; n < order.size(); n++)
            if (n >= max_ins) keep[order[n]] = 0;
    }
    stats.budget_instructions = max_ins;
    if (stream_) {
        /* Played code left to the interpreter fetches its opcodes from the
         * ROM: that counts as reads of its chunk (about len bytes each). */
        for (size_t i = 0; i < all.size(); i++) {
            const Ins& in = all[i];
            if (keep[i] || in.addr < 0x4000) continue;
            auto it = hits.find(key(in.bank, in.addr));
            if (it == hits.end()) continue;
            const size_t c = (static_cast<size_t>(in.bank) * 0x4000u + (in.addr - 0x4000u)) >> 12;
            if (c < chunk_weight_.size()) chunk_weight_[c] += it->second * in.len;
        }
    }
    Decoder dec(rom_);
    for (size_t i = 0; i < all.size(); i++) {
        if (!keep[i]) continue;
        Ins& in = all[i];
        if (!played.count(key(in.bank, in.addr))) stats.speculative_kept++;
        if (opt_.emit_comments) in.text = dec.decode(in.addr, in.bank).disassemble();
        index_[key(in.bank, in.addr)] = ins_.size();
        ins_.push_back(std::move(in));
    }
    return true;
}

Ins* Generator::resolve(const Ins& from, uint16_t target, int& bank_check) {
    bank_check = -1;
    if (target < 0x4000) return find(0, target);
    if (target >= 0x8000) return nullptr;
    if (from.bank != 0) {
        Ins* t = find(from.bank, target);
        if (t) bank_check = from.bank;
        return t;
    }
    Ins* found = nullptr;
    for (uint16_t b = 1; b < rom_.bank_count(); b++) {
        if (Ins* t = find(b, target)) {
            if (found) return nullptr; /* reached through several banks */
            found = t;
            bank_check = b;
        }
    }
    return found;
}

bool Generator::plan_stream(MetaTargetStats& stats, std::string& error) {
    if (opt_.espboy) {
        // Raw first: bank 0 stays mapped and every other sector is stored
        // as is, read with spi_flash_read.  Only when that exceeds the ROM
        // area are sectors LZSS-packed, largest savings first (the fewest
        // sectors to decode; no cost claim).
        const size_t sectors = rom_.size() / 512;
        packed_sector_.resize(sectors);
        std::vector<std::pair<size_t, size_t>> savings;
        for (size_t k = 32; k < sectors; ++k) {
            auto block = m0boy::rom::encode(rom_.data() + k * 512, 512);
            savings.emplace_back(512 - block.bytes.size(), k);
            packed_sector_[k].assign(rom_.data() + k * 512, rom_.data() + (k + 1) * 512);
        }
        std::stable_sort(savings.begin(), savings.end(), [](auto a, auto b) { return a.first > b.first; });
        size_t size = rom_.size() - 16384 + 2; // rom.bin holds sectors 32 on; bank 0 is in the application
        for (auto [saved, k] : savings) {
            if (size <= kEspRomBytes) break;
            if (!saved) continue;
            packed_sector_[k] = m0boy::rom::encode(rom_.data() + k * 512, 512).bytes;
            size -= saved;
        }
        if (size > kEspRomBytes) {
            error = "ESPboy ROM does not fit in the 2.75 MiB ROM area of the 4 MiB flash, even compressed (bank 0 stays in the application)";
            return false;
        }
        chunk_weight_.assign((rom_.size() + 4095) / 4096, 0);
        resident_index_.assign(chunk_weight_.size(), -1);
        stream_code_budget_ = kEspApp - kEspRuntime - 16384 - (sectors + 1) * 4;
        stats.stream = stats.packed = true;
        stats.sd = false;
        stats.packed_raw_bytes = rom_.size() - 16384;
        stats.packed_bytes = size - 2;
        return true;
    }
    /* No need to introduce a sector cache when the entire ROM and played
     * native code fit raw. Explicit streaming/residency requests still win
     * (in particular the packed/cache regression tests). */
    if (opt_.stream != 1 && !opt_.rom_resident_bytes) {
        std::set<uint32_t> locations;
        for (const auto& path : opt_.coverage_files) {
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line)) {
                unsigned bank, address, flags;
                if (std::sscanf(line.c_str(), "%x %x %x", &bank, &address, &flags) == 3 && address < 0x8000)
                    locations.insert((address < 0x4000 ? 0 : bank) << 16 | address);
            }
        }
        const size_t code = opt_.code_budget_bytes ? opt_.code_budget_bytes :
                            locations.size() * (opt_.bytes_per_instruction ? opt_.bytes_per_instruction : 20) + 16384;
        if (m0boy::rom::fits_raw(app_flash(), 40960, rom_.size(), code)) {
            stream_ = pack_ = false;
            return true;
        }
    }
    const size_t chunks = (rom_.size() + 4095) / 4096;
    chunk_weight_.assign(chunks, 0);
    resident_index_.assign(chunks, -1);
    for (size_t c = 0; c < 4 && c < chunks; c++) resident_index_[c] = 0; /* bank 0: gbl_rom */
    for (const fs::path& path : opt_.rom_profile_files) {
        std::ifstream in(path);
        if (!in) {
            error = "cannot read ROM read profile " + path.string();
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            unsigned page = 0;
            unsigned long long count = 0;
            if (std::sscanf(line.c_str(), "%x %llu", &page, &count) != 2) continue;
            if (page / 16 < chunks) chunk_weight_[page / 16] += count;
        }
    }
    /* --rom-plan: what each block saves when kept, from the card cache
     * simulated on the sessions' traces (m0romsim, as for the Game Gear),
     * instead of read counts: a chunk read often but still cached costs
     * nothing.  Weights in millionths of an SD read per frame. */
    if (!opt_.rom_plan.empty()) {
        std::ifstream in(opt_.rom_plan);
        if (!in) {
            error = "cannot read ROM plan " + opt_.rom_plan.string();
            return false;
        }
        std::fill(chunk_weight_.begin(), chunk_weight_.end(), 0);
        for (std::string line; std::getline(in, line);) {
            if (line.empty() || line[0] == '#') continue;
            unsigned offset = 0, size = 0;
            char codec[16];
            unsigned long bytes = 0;
            double saved = 0;
            if (std::sscanf(line.c_str(), "%x %u %15s %lu %lf", &offset, &size, codec, &bytes, &saved) != 5 || !size) continue;
            const size_t first = offset / 4096, last = (offset + size - 1) / 4096;
            for (size_t c = first; c <= last && c < chunks; c++)
                chunk_weight_[c] += static_cast<uint64_t>(saved * 1e6 / static_cast<double>(last - first + 1) + 0.5);
        }
    }
    size_t touched = 0;
    for (size_t c = 4; c < chunks; c++)
        if (chunk_weight_[c]) touched += 4096;
    const size_t app = app_flash();
    if (pack_) {
        /* Compressed flash: every sector of banks 1+ is packed now; the
         * resident chunks (chosen later) stay raw instead.  Flash holds the
         * runtime with the decoder, bank 0, the chunk map, the sector
         * offsets, the resident chunks, the packed rest and the code. */
        const size_t sectors = (rom_.size() + 511) / 512;
        packed_sector_.assign(sectors, {});
        size_t packed_total = 0, banked = 0;
        for (size_t k = 32; k < sectors; k++) {
            const size_t len = std::min<size_t>(512, rom_.size() - k * 512);
            auto block = m0boy::rom::encode(rom_.data() + k * 512, len);
            std::vector<uint8_t> c = std::move(block.bytes);
            if (len < 512) c.assign(rom_.data() + k * 512, rom_.data() + k * 512 + len);
            packed_total += c.size();
            banked += len;
            packed_sector_[k] = std::move(c);
        }
        const size_t fixed = 40960 + 1024 + 0x4000 + chunks + 4 * (sectors + 1) + 4;
        const size_t available = app > fixed ? app - fixed : 0;
        /* Resident chunks cost their raw size minus what their compressed
         * copy would have taken; favour code over raw data (a third). */
        size_t resident = opt_.rom_resident_bytes ? opt_.rom_resident_bytes
                                                  : std::min(touched, available / 3);
        resident = resident / 4096 * 4096;
        auto code_left = [&](size_t r) -> long long {
            const double packed_rest = banked ? packed_total * double(banked - std::min(r, banked)) / banked : 0;
            return static_cast<long long>(available) - static_cast<long long>(r) -
                   static_cast<long long>(packed_rest);
        };
        const long long min_code = 16384;
        if (code_left(resident) < min_code && code_left(0) >= min_code) resident = 0;
        if (code_left(resident) < min_code) {
            /* All packed data does not fit. Keep a useful raw/packed hot
             * subset, using the same density/cost policy as GG/SMS; the
             * remaining sectors still come from the uncompressed card. */
            const size_t hybrid_fixed = fixed + 4096; /* SD runtime allowance */
            if (touched && app > hybrid_fixed + min_code + 4096) {
                partial_pack_ = true;
                const size_t free = app - hybrid_fixed;
                stream_code_budget_ = std::max<size_t>(min_code, free / 2);
                flash_data_budget_ = free - stream_code_budget_;
                resident_budget_ = opt_.rom_resident_bytes ? opt_.rom_resident_bytes : flash_data_budget_;
                packed_keep_.assign(sectors, false);
                stats.stream = stats.packed = stats.sd = true;
                return true;
            }
            char note[160];
            std::snprintf(note, sizeof(note),
                          "compressed ROM (%zu KiB) leaves no room for code",
                          (packed_total + 1023) / 1024);
            stats.pack_note = note;
            pack_ = false;
            packed_sector_.clear();
            if (rom_.size() < 0x40000 && opt_.stream != 1) {
                stream_ = false; /* plain build: the ROM fits raw */
                return true;
            }
        } else {
            resident_budget_ = resident;
            stream_code_budget_ = static_cast<size_t>(code_left(resident));
            stats.stream = true;
            stats.packed = true;
            return true;
        }
    }
    /* Flash: the application area minus the runtime with the SD reader,
     * bank 0 and the chunk map; the rest is shared between resident chunks
     * and code. */
    const size_t runtime = 45056;
    const size_t fixed = runtime + 0x4000 + chunks;
    const size_t available = app > fixed ? app - fixed : 0;
    resident_budget_ = opt_.rom_resident_bytes ? opt_.rom_resident_bytes
                                               : std::min(touched, available / 2);
    resident_budget_ = std::min(resident_budget_ / 4096 * 4096, available / 4096 * 4096);
    stream_code_budget_ = available - resident_budget_;
    stats.stream = true;
    stats.sd = true;
    return true;
}

void Generator::choose_resident(MetaTargetStats& stats) {
    if (opt_.espboy) return;
    if (partial_pack_) {
        std::vector<size_t> sizes;
        for (const auto& sector : packed_sector_) sizes.push_back(sector.size());
        std::vector<double> weights(chunk_weight_.begin(), chunk_weight_.end());
        auto selected = m0boy::rom::select_units(sizes, weights, 8, 4, flash_data_budget_, resident_budget_, m0boy::rom::sd_cycles);
        int slot = 0;
        for (size_t c : selected.raw) resident_index_[c] = slot++;
        for (size_t c : selected.packed)
            for (size_t s = c * 8; s < (c + 1) * 8; ++s) {
                packed_keep_[s] = true;
                stats.packed_bytes += packed_sector_[s].size();
                stats.packed_raw_bytes += 512;
            }
        stats.resident_chunks = selected.raw.size();
        stats.streamed_chunks = chunk_weight_.size() - 4 - selected.raw.size() - selected.packed.size();
        if (selected.packed.empty()) {
            pack_ = partial_pack_ = false;
            stats.packed = false; /* no decoder/index overhead for a raw-only subset */
        }
        return;
    }
    std::vector<size_t> order;
    for (size_t c = 4; c < chunk_weight_.size(); c++)
        if (chunk_weight_[c]) order.push_back(c);
    std::stable_sort(order.begin(), order.end(),
                      [&](size_t a, size_t b) { return chunk_weight_[a] > chunk_weight_[b]; });
    if (pack_) {
        size_t packed = 0;
        std::vector<size_t> sizes;
        for (const auto& sector : packed_sector_) { sizes.push_back(sector.size()); packed += sector.size(); }
        const size_t fixed = 40960 + 1024 + 0x4000 + chunk_weight_.size() + 4 * (packed_sector_.size() + 1) + 4;
        const size_t used = fixed + stream_code_budget_ + packed;
        std::vector<double> weights(chunk_weight_.begin(), chunk_weight_.end());
        order = m0boy::rom::raw_units(sizes, weights, 8, 4, resident_budget_, app_flash() > used ? app_flash() - used : 0);
    }
    int slot = 0;
    for (size_t k = 0; k < order.size() && static_cast<size_t>(slot) < resident_budget_ / 4096; k++)
        resident_index_[order[k]] = slot++;
    stats.resident_chunks = static_cast<size_t>(slot);
    stats.streamed_chunks = chunk_weight_.size() > 4 ? chunk_weight_.size() - 4 - slot : 0;
    if (pack_) {
        stats.packed_bytes = stats.packed_raw_bytes = 0;
        for (size_t k = 32; k < packed_sector_.size(); k++) {
            if (resident_index_[k / 8] >= 0) continue;
            stats.packed_bytes += packed_sector_[k].size();
            stats.packed_raw_bytes += std::min<size_t>(512, rom_.size() - k * 512);
        }
    }
}

std::string Generator::region_name(int r) const {
    const Ins& first = ins_[regions_[r].front()];
    char buf[32];
    std::snprintf(buf, sizeof(buf), "r%u_%04x", first.bank, first.addr);
    return buf;
}

void Generator::build() {
    /* ins_ is sorted by key because coverage came from an ordered map. */
    std::vector<size_t>* cur = nullptr;
    const Ins* prev = nullptr;
    for (size_t i = 0; i < ins_.size(); i++) {
        Ins& in = ins_[i];
        bool split = cur == nullptr || prev->bank != in.bank ||
                     (prev->addr < 0x4000) != (in.addr < 0x4000) ||
                     in.addr > prev->next() + 32;
        if (!split && cur->size() >= opt_.max_region_instructions &&
            !(prev->falls() && prev->next() == in.addr))
            split = true;
        if (split) {
            regions_.emplace_back();
            cur = &regions_.back();
        }
        in.region = static_cast<int>(regions_.size() - 1);
        in.order = cur->size();
        cur->push_back(i);
        prev = &in;
    }

    /* Labels and region entry points. */
    for (Ins& in : ins_) {
        if (in.entry) in.sw_case = in.label = true;
        if (in.falls()) {
            if (Ins* n = find(in.bank, in.next())) {
                const bool adjacent = n->region == in.region && n->order == in.order + 1;
                if (!adjacent) {
                    n->label = true;
                    if (n->region != in.region) n->sw_case = true;
                }
            }
        }
        if (in.flow == Flow::Call || in.flow == Flow::CallCond) {
            if (Ins* ret = find(in.bank, in.next())) ret->sw_case = ret->label = true;
        }
        if (in.flow == Flow::Jump || in.flow == Flow::JumpCond || in.flow == Flow::Call ||
            in.flow == Flow::CallCond) {
            int bank_check;
            if (Ins* t = resolve(in, in.target, bank_check)) {
                t->label = true;
                const bool is_call = in.flow == Flow::Call || in.flow == Flow::CallCond;
                if (is_call || t->region != in.region) t->sw_case = true;
                if (!is_call && t->region == in.region && t->order <= in.order) {
                    t->loop_head = true;
                    t->sw_case = true;
                }
            }
        }
    }
    /* EI followed by a plain instruction N in the same region: N is
     * duplicated on the EI path, which then jumps to the instruction after
     * N (labelled here). */
    if (!opt_.precise) {
        for (Ins& in : ins_) {
            if (in.op != 0xFB) continue;
            Ins* n = find(in.bank, in.next());
            if (!n || n->region != in.region || n->flow != Flow::Seq || n->op == 0xFB) continue;
            Ins* m = find(n->bank, n->next());
            if (!m || m->region != in.region) continue;
            in.ei_dup = true;
            m->label = true;
        }
    }
    for (auto& reg : regions_) {
        Ins& first = ins_[reg.front()];
        first.label = true;
    }

    /* Flag liveness, per region: least fixed point from empty sets.  Exits to
     * other regions or the interpreter keep every flag live. */
    for (Ins& in : ins_) in.live_in = in.live_out = 0;
    for (size_t r = 0; r < regions_.size(); r++) {
        const std::vector<size_t>& reg = regions_[r];
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t k = reg.size(); k-- > 0;) {
                Ins& in = ins_[reg[k]];
                uint8_t out = 0;
                if (in.falls()) {
                    Ins* n = find(in.bank, in.next());
                    out |= (n && n->region == in.region) ? n->live_in : FALL;
                }
                if (in.flow == Flow::Jump || in.flow == Flow::JumpCond) {
                    int bank_check;
                    Ins* t = resolve(in, in.target, bank_check);
                    out |= (t && t->region == in.region) ? t->live_in : FALL;
                }
                if (in.flow == Flow::JumpDyn || in.flow == Flow::Ret || in.flow == Flow::Reti ||
                    in.flow == Flow::RetCond || in.flow == Flow::Bad)
                    out |= FALL;
                const uint8_t live_in = static_cast<uint8_t>(in.use | (out & ~in.def));
                if (out != in.live_out || live_in != in.live_in) {
                    in.live_out = out;
                    in.live_in = live_in;
                    changed = true;
                }
            }
        }
    }

    if (!opt_.precise) detect_spin_loops();
    for (const Ins& in : ins_)
        if (in.sw_case) entry_count_++;
}

/* ---- wait loops ------------------------------------------------------------
 *
 * A loop whose body has no side effect but register and flag writes, and
 * whose registers carry nothing from one iteration to the next, repeats the
 * same iteration until something it reads changes.  What it reads can only
 * change when code runs (an interrupt, i.e. at a device event), at a device
 * event, or for LY/STAT at the next PPU mode change.  The back edge then
 * adds whole periods to the cycle counter up to the first iteration where
 * that can happen (gbl_spin); the iterations skipped would have produced the
 * same state, so the result is identical to running them.  Delay loops
 * counting a register down to zero (DEC r / DEC rr; LD A,hi; OR lo; JR NZ)
 * skip whole iterations the same way, up to the next device event. */

namespace {

constexpr uint8_t RB = 1, RC = 2, RD = 4, RE = 8, RH = 16, RL = 32, RA = 128;

uint8_t rbit(unsigned r) { return r == 6 ? 0 : static_cast<uint8_t>(1u << r); }
uint8_t pair_bits(unsigned p) { return p == 0 ? (RB | RC) : p == 1 ? (RD | RE) : (RH | RL); }

struct SpinFx {
    uint8_t rd = 0, wr = 0;
    int mem = 0;       /* 0 none, 1 constant address, 2 through register pair */
    uint16_t addr = 0;
    unsigned pair = 0; /* 0 BC, 1 DE, 2 HL */
};

/* Register effects of an instruction allowed in a wait loop; false for
 * anything with a side effect (memory or IO writes, stack, IME, calls). */
bool spin_effects(const Ins& in, SpinFx& fx) {
    const uint8_t op = in.op;
    auto src = [&](unsigned r) {
        if (r == 6) {
            fx.mem = 2;
            fx.pair = 2;
            fx.rd |= RH | RL;
        } else {
            fx.rd |= rbit(r);
        }
    };
    if (op >= 0x40 && op < 0x80) {
        const unsigned d = (op >> 3) & 7;
        if (op == 0x76 || d == 6) return false;
        src(op & 7);
        fx.wr |= rbit(d);
        return true;
    }
    if (op >= 0x80 && op < 0xC0) {
        src(op & 7);
        fx.rd |= RA;
        if (((op >> 3) & 7) != 7) fx.wr |= RA;
        return true;
    }
    switch (op) {
    case 0x00: case 0x37: case 0x3F:
    case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        return true;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E:
        fx.wr |= rbit((op >> 3) & 7);
        return true;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C:
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D:
        fx.rd |= rbit((op >> 3) & 7);
        fx.wr |= rbit((op >> 3) & 7);
        return true;
    case 0x03: case 0x13: case 0x23: case 0x0B: case 0x1B: case 0x2B:
        fx.rd |= pair_bits(op >> 4);
        fx.wr |= pair_bits(op >> 4);
        return true;
    case 0x0A: case 0x1A:
        fx.mem = 2;
        fx.pair = op >> 4;
        fx.rd |= pair_bits(op >> 4);
        fx.wr |= RA;
        return true;
    case 0xF0: case 0xFA:
        fx.mem = 1;
        fx.addr = op == 0xF0 ? static_cast<uint16_t>(0xFF00 | in.n8) : in.n16;
        fx.wr |= RA;
        return true;
    case 0x07: case 0x0F: case 0x17: case 0x1F: case 0x2F:
        fx.rd |= RA;
        fx.wr |= RA;
        return true;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        fx.rd |= RA;
        if (op != 0xFE) fx.wr |= RA;
        return true;
    case 0xCB: {
        const unsigned r = in.n8 & 7, grp = in.n8 >> 6;
        if (r == 6) {
            if (grp != 1) return false;
            src(6);
            return true;
        }
        fx.rd |= rbit(r);
        if (grp != 1) fx.wr |= rbit(r);
        return true;
    }
    default:
        return false;
    }
}

/* How a read of constant address a can change while nothing runs:
 * 0 only when code runs, 1 at a device event, 2 at the next PPU mode or line
 * change (LY, STAT), 3 at any time (no fast-forward).  Must match
 * gbl_spin_kind() in the runtime. */
int spin_const_kind(uint16_t a) {
    if (a < 0xFF00 || a >= 0xFF80) return 0;
    switch (a & 0xFF) {
    case 0x00: case 0x0F: return 1;
    case 0x41: case 0x44: return 2;
    case 0x40: case 0x42: case 0x43: case 0x45: case 0x47: case 0x48:
    case 0x49: case 0x4A: case 0x4B: return 0;
    default: return 3;
    }
}

} // namespace

void Generator::detect_spin_loops() {
    static const char* kPair[3] = {"gbr.bc", "gbr.de", "gbr.hl"};
    for (const std::vector<size_t>& reg : regions_) {
        for (size_t k = 0; k < reg.size(); k++) {
            Ins& b = ins_[reg[k]];
            if (b.flow != Flow::Jump && b.flow != Flow::JumpCond) continue;
            int bank_check;
            Ins* t = resolve(b, b.target, bank_check);
            if (!t || t->region != b.region || t->order > b.order) continue;

            /* Straight-line body from the head along the fall-through path
             * to the back edge; its only branches leave the loop, and no
             * other loop starts inside. */
            std::vector<Ins*> body;
            bool ok = true;
            for (Ins* in = t; ok;) {
                if (in->region != b.region || (in != t && in->loop_head) || body.size() >= 32) {
                    ok = false;
                    break;
                }
                /* The instruction emitted after an EI polls interrupts after
                 * itself (unless the EI path duplicates it, ei_dup): a poll
                 * inside the body the skip does not bound. */
                if (in->order > 0 && ins_[reg[in->order - 1]].op == 0xFB &&
                    !ins_[reg[in->order - 1]].ei_dup) {
                    ok = false;
                    break;
                }
                body.push_back(in);
                if (in == &b) break;
                if (in->flow == Flow::JumpCond) {
                    if (in->target >= t->addr && in->target <= b.addr) ok = false;
                } else if (in->flow != Flow::Seq) {
                    ok = false;
                }
                in = find(in->bank, in->next());
                if (!in) ok = false;
            }
            if (!ok) continue;
            std::vector<SpinFx> fx(body.size());
            uint8_t loop_wr = 0, loop_fdef = 0;
            bool exits = false;
            for (size_t j = 0; j < body.size() && ok; j++) {
                if (!spin_effects(*body[j], fx[j])) ok = false;
                loop_wr |= fx[j].wr;
                loop_fdef |= body[j]->def;
                if (body[j] != &b && body[j]->flow == Flow::JumpCond) exits = true;
            }
            if (!ok) continue;

            /* Delay loop: one DEC counter, the back edge taken while it is
             * nonzero, nothing else leaving the loop. */
            int cnt = -1;
            for (size_t j = 0; j < body.size(); j++) {
                const uint8_t op = body[j]->op;
                const bool dec8 = (op & 0xC7) == 0x05 && op != 0x35;
                const bool dec16 = op == 0x0B || op == 0x1B || op == 0x2B;
                if (dec8 || dec16) cnt = cnt == -1 ? static_cast<int>(j) : -2;
            }
            uint8_t cnt_bits = 0;
            int counted = 0; /* 2: 8-bit counter, 3: 16-bit counter */
            unsigned cnt_reg = 0;
            if (cnt >= 0 && !exits && b.flow == Flow::JumpCond && (b.op == 0x20 || b.op == 0xC2)) {
                const Ins& dec = *body[cnt];
                const size_t n = body.size();
                if (dec.op & 0x04) {
                    cnt_reg = (dec.op >> 3) & 7;
                    cnt_bits = rbit(cnt_reg);
                    counted = 2;
                    for (size_t j = cnt + 1; j + 1 < n; j++)
                        if (body[j]->def & FZ) counted = 0;
                } else {
                    cnt_reg = dec.op >> 4;
                    cnt_bits = pair_bits(cnt_reg);
                    const unsigned hi = cnt_reg * 2, lo = cnt_reg * 2 + 1;
                    const Ins* ld = n >= 3 ? body[n - 3] : nullptr;
                    const Ins* orr = n >= 3 ? body[n - 2] : nullptr;
                    if (ld && orr && static_cast<size_t>(cnt) < n - 3 && ld->op >= 0x78 && ld->op < 0x80 &&
                        orr->op >= 0xB0 && orr->op < 0xB8) {
                        const unsigned x = ld->op & 7, y = orr->op & 7;
                        if ((x == hi && y == lo) || (x == lo && y == hi)) counted = 3;
                    }
                }
                for (size_t j = 0; j < body.size() && counted; j++)
                    if (static_cast<int>(j) != cnt && (fx[j].wr & cnt_bits)) counted = 0;
                if (!counted) cnt_bits = 0;
            }

            /* Nothing may flow from one iteration into the next except the
             * counter: every register or flag written in the loop is written
             * before it is read. */
            const uint8_t carried = static_cast<uint8_t>(loop_wr & ~cnt_bits);
            uint8_t written = 0, fwritten = 0;
            for (size_t j = 0; j < body.size() && ok; j++) {
                const uint8_t rd = static_cast<int>(j) == cnt && counted ? 0 : fx[j].rd;
                if (rd & carried & ~written) ok = false;
                if (body[j]->use & loop_fdef & ~fwritten) ok = false;
                written |= fx[j].wr;
                fwritten |= body[j]->def;
            }
            if (!ok) continue;

            /* Reads: any number that change only when code runs, at most
             * one whose value can change with time. */
            int timed = -1;
            std::string kind = "0";
            for (size_t j = 0; j < body.size() && ok; j++) {
                if (!fx[j].mem) continue;
                if (fx[j].mem == 1) {
                    const int kd = spin_const_kind(fx[j].addr);
                    if (kd == 0) continue;
                    if (kd == 3 || counted || timed >= 0) ok = false;
                    timed = static_cast<int>(j);
                    kind = std::to_string(kd);
                } else {
                    if (counted || timed >= 0 || (loop_wr & pair_bits(fx[j].pair))) ok = false;
                    timed = static_cast<int>(j);
                    kind = std::string("gbl_spin_kind(") + kPair[fx[j].pair] + ")";
                }
            }
            if (!ok) continue;

            int period = 0, read_ofs = 0;
            for (size_t j = 0; j < body.size(); j++) {
                period += body[j]->cycles;
                if (static_cast<int>(j) == timed) read_ofs = period;
            }
            if (b.flow == Flow::JumpCond) period += b.extra;
            t->spin_head = true;
            if (timed >= 0) body[timed]->spin_read = true;
            b.spin_period = static_cast<uint16_t>(period);
            b.spin_read_ofs = static_cast<uint16_t>(read_ofs);
            b.spin = counted ? static_cast<uint8_t>(counted) : 1;
            b.spin_reg = static_cast<uint8_t>(cnt_reg);
            b.spin_kind = kind;
            if (counted)
                for (size_t j = 0; j + 1 < body.size(); j++)
                    b.spin_body.push_back(static_cast<size_t>(body[j] - ins_.data()));
            if (counted) counted_loops_++;
            else spin_loops_++;
        }
    }
}

void Generator::emit_spin(std::ostringstream& o, const Ins& in) {
    const int period = in.spin_period;
    if (in.spin == 1) {
        o << "    gbl_spin(" << period << ", " << in.spin_read_ofs << ", " << in.spin_kind << ");\n";
        return;
    }
    /* Delay loop: count the skipped iterations off, then replay the last one
     * (without its cycles) so registers and flags that depend on the counter
     * hold what they would after it: an interrupt taken at the loop head
     * sees them. */
    const std::string r = in.spin == 2 ? kR8[in.spin_reg] : kR16[in.spin_reg];
    const char* cast = in.spin == 2 ? "(uint8_t)" : "(uint16_t)";
    std::ostringstream replay;
    const int saved = pend_;
    const bool saved_ei = ei_pending_;
    for (size_t j = 0; j < in.spin_body.size(); j++) {
        Ins copy = ins_[in.spin_body[j]];
        copy.label = copy.spin_head = copy.spin_read = false;
        const Ins* following = j + 1 < in.spin_body.size() ? &ins_[in.spin_body[j + 1]] : &in;
        replaying_ = true;
        emit_ins(replay, copy, following);
        replaying_ = false;
    }
    pend_ = saved;
    ei_pending_ = saved_ei;
    o << "    { uint32_t k = gbl_spin_n(" << period << ", " << r << " - 1u);\n"
      << "    if (k) {\n    " << r << " = " << cast << "(" << r << " - k + 1u);\n";
    std::istringstream lines(replay.str());
    for (std::string line; std::getline(lines, line);)
        if (line.rfind("    CY(", 0) != 0) o << line << "\n";
    o << "    } }\n";
}

std::string Generator::const_read(std::ostringstream& o, uint16_t a) {
    if (a < 0x4000) return hex(rom_.read_banked(0, a), 2);
    if (a < 0x8000) return "gbl_rd(" + hex(a, 4) + ")";
    if (a < 0xA000) return "gbl_vram[" + hex(a - 0x8000, 4) + "]";
    if (a < 0xC000) return "gbl_rd(" + hex(a, 4) + ")";
    if (a < 0xE000) return "gbl_wram[" + hex(a - 0xC000, 4) + "]";
    if (a < 0xFE00) return "gbl_wram[" + hex(a - 0xE000, 4) + "]";
    if (a < 0xFF00) return "gbl_rd(" + hex(a, 4) + ")";
    if (a == 0xFF00) return "gbl_joyp"; /* P1: no device to advance */
    if (a < 0xFF80) {
        flush(o);
        return "gbl_io_rd(" + hex(a & 0xFF, 2) + ")";
    }
    if (a < 0xFFFF) return "gbl_hram[" + hex(a - 0xFF80, 2) + "]";
    return "gbl_ie";
}

void Generator::const_write(std::ostringstream& o, uint16_t a, const std::string& v,
                            bool& sync_after) {
    if (a >= 0x8000 && a < 0xA000) {
        o << "    GBL_VRAM_WR(" << hex(a - 0x8000, 4) << ", " << v << ");\n";
    } else if (a >= 0xC000 && a < 0xE000) {
        o << "    gbl_wram[" << hex(a - 0xC000, 4) << "] = " << v << ";\n";
    } else if (a >= 0xE000 && a < 0xFE00) {
        o << "    gbl_wram[" << hex(a - 0xE000, 4) << "] = " << v << ";\n";
    } else if (a >= 0xFF00 && a < 0xFF80) {
        flush(o);
        o << "    gbl_io_wr(" << hex(a & 0xFF, 2) << ", " << v << ");\n";
        const uint8_t n = a & 0xFF;
        if (n == 0x0F || n == 0x40 || n == 0x41 || n == 0x45) sync_after = true;
    } else if (a >= 0xFF80 && a < 0xFFFF) {
        o << "    gbl_hram[" << hex(a - 0xFF80, 2) << "] = " << v << ";\n";
    } else {
        if (a == 0xFFFF) {
            flush(o);
            sync_after = true;
        }
        o << "    gbl_wr(" << hex(a, 4) << ", " << v << ");\n";
    }
}

/* Emit a jump to target.  pc is always written first so any path out of the
 * region resumes at the right place. */
void Generator::emit_transfer(std::ostringstream& o, const Ins& from, uint16_t target,
                              bool set_pc, int extra) {
    int bank_check;
    Ins* t = resolve(from, target, bank_check);
    if (t && t->region == from.region) {
        /* one line for the taken branch's cycles and the label's entry
         * refund: on a branch whose extra equals the carry they cancel */
        const int net = extra - t->carry;
        if (net) o << "    CY(" << net << ");\n";
        o << "    goto L_" << hex4(target) << ";\n";
        return;
    }
    if (extra) o << "    CY(" << extra << ");\n";
    if (set_pc) o << "    gbr.pc = " << hex(target, 4) << ";\n";
    if (!t) {
        o << "    gbl_exec(); return;\n";
    } else if (bank_check < 0) {
        o << "    " << region_name(t->region) << "(); return;\n";
    } else {
        o << "    if (gbr.rom_bank == " << bank_check << ") { " << region_name(t->region)
          << "(); return; }\n    gbl_exec(); return;\n";
    }
}

void Generator::emit_call(std::ostringstream& o, const Ins& in) {
    const uint16_t ret = in.next();
    o << "    gbl_push(" << hex(ret, 4) << "); gbr.pc = " << hex(in.target, 4) << ";\n";
    int bank_check;
    Ins* t = resolve(in, in.target, bank_check);
    if (!t) {
        o << "    gbl_exec();\n";
    } else if (bank_check < 0) {
        o << "    " << region_name(t->region) << "();\n";
    } else {
        o << "    if (gbr.rom_bank == " << bank_check << ") " << region_name(t->region)
          << "(); else gbl_exec();\n";
    }
    o << "    if (gbr.pc != " << hex(ret, 4) << ") return;\n";
}

void Generator::emit_alu(std::ostringstream& o, unsigned grp, const std::string& src, bool mem,
                         bool src_is_a, uint8_t live) {
    const bool z = live & FZ, n = live & FN, h = live & FH, c = live & FC;
    std::string s = "    ";
    /* an instruction whose flags are all dead emits nothing: do not print
     * the indent-only line left behind */
    auto print = [&]() {
        if (s.find_first_not_of(' ') != std::string::npos) o << s << "\n";
    };
    switch (grp) {
    case 0: case 1: case 2: case 3: {
        const bool sub = grp >= 2;
        if (h) {
            /* Half carry inline: bit 4 of a ^ v ^ result is the carry (or
             * borrow) into bit 4, carry in included.  A call to the helper
             * cost more than the computation. */
            const char* carry = (grp == 1) ? " + gbr.cf" : (grp == 3) ? " - gbr.cf" : "";
            s += "{ uint32_t v = " + src + ", t = (uint32_t)gbr.a " + (sub ? "- " : "+ ") + "v" + carry +
                 "; gbr.hf = (uint8_t)(((gbr.a ^ v ^ t) >> 4) & 1);";
            if (c) s += sub ? " gbr.cf = (uint8_t)((t >> 8) & 1);" : " gbr.cf = (uint8_t)(t >> 8);";
            s += " gbr.a = (uint8_t)t;";
            if (z) s += " gbr.zr = gbr.a;";
            s += " }";
            if (n) s += sub ? " gbr.nf = 1;" : " gbr.nf = 0;";
            print();
            return;
        }
        const char* carry_in = (grp == 1) ? " + gbr.cf" : (grp == 3) ? " - gbr.cf" : "";
        if (z || c) {
            s += "{ uint32_t t = (uint32_t)gbr.a " + std::string(sub ? "- " : "+ ") + "(uint32_t)" +
                 src + carry_in + "; ";
            if (c) s += sub ? "gbr.cf = (uint8_t)((t >> 8) & 1); " : "gbr.cf = (uint8_t)(t >> 8); ";
            s += "gbr.a = (uint8_t)t; ";
            if (z) s += "gbr.zr = gbr.a; ";
            s += "}";
        } else {
            s += "gbr.a = (uint8_t)(gbr.a " + std::string(sub ? "- " : "+ ") + src + carry_in + ");";
        }
        if (n) s += sub ? " gbr.nf = 1;" : " gbr.nf = 0;";
        print();
        return;
    }
    case 4: case 5: case 6: {
        if (grp == 5 && src_is_a) s += "gbr.a = 0;";
        else if (src_is_a) s += "";
        else s += std::string("gbr.a ") + (grp == 4 ? "&= " : grp == 5 ? "^= " : "|= ") + src + ";";
        if (z) s += " gbr.zr = gbr.a;";
        if (n) s += " gbr.nf = 0;";
        if (h) s += grp == 4 ? " gbr.hf = 1;" : " gbr.hf = 0;";
        if (c) s += " gbr.cf = 0;";
        print();
        return;
    }
    default: {
        if (h) {
            s += "{ uint32_t v = " + src + ", t = (uint32_t)gbr.a - v; gbr.hf = (uint8_t)(((gbr.a ^ v ^ t) >> 4) & 1);";
            if (z) s += " gbr.zr = (uint8_t)t;";
            if (c) s += " gbr.cf = (uint8_t)((t >> 8) & 1);";
            s += " }";
            if (n) s += " gbr.nf = 1;";
            print();
            return;
        }
        if (z || c) {
            s += "{ uint32_t t = (uint32_t)gbr.a - (uint32_t)" + src + "; ";
            if (z) s += "gbr.zr = (uint8_t)t; ";
            if (c) s += "gbr.cf = (uint8_t)((t >> 8) & 1); ";
            s += "}";
        } else if (mem) {
            s += "(void)" + src + ";";
        }
        if (n) s += " gbr.nf = 1;";
        print();
        return;
    }
    }
}

void Generator::emit_cb(std::ostringstream& o, const Ins& in) {
    const uint8_t cb = in.n8;
    const unsigned r = cb & 7, grp = cb >> 6, bit = (cb >> 3) & 7;
    const bool mem = r == 6;
    const std::string src = mem ? "gbl_rd(gbr.hl)" : kR8[r];
    const uint8_t live = in.live_out;
    const std::string mask = hex(1u << bit, 2);
    auto print = [&](const std::string& s) {
        if (s.find_first_not_of(' ') != std::string::npos) o << s << "\n";
    };
    if (grp == 1) {
        std::string s = "   ";
        if (live & FZ) s += " gbr.zr = (uint8_t)(" + src + " & " + mask + ");";
        else if (mem) s += " (void)" + src + ";";
        if (live & FN) s += " gbr.nf = 0;";
        if (live & FH) s += " gbr.hf = 1;";
        print(s);
        return;
    }
    if (grp == 2 || grp == 3) {
        const std::string expr = grp == 2 ? "(uint8_t)(" + src + " & ~" + mask + ")"
                                          : "(uint8_t)(" + src + " | " + mask + ")";
        if (mem) o << "    gbl_wr(gbr.hl, " << expr << ");\n";
        else o << "    " << kR8[r] << " = " << expr << ";\n";
        return;
    }
    static const char* rexpr[8] = {"(v << 1) | (v >> 7)", "(v >> 1) | (v << 7)",
                                   "(v << 1) | gbr.cf", "(v >> 1) | ((uint32_t)gbr.cf << 7)",
                                   "v << 1", "(v >> 1) | (v & 0x80)", "(v >> 4) | (v << 4)",
                                   "v >> 1"};
    static const char* cexpr[8] = {"v >> 7", "v & 1", "v >> 7", "v & 1",
                                   "v >> 7", "v & 1", "0", "v & 1"};
    const unsigned k = (cb >> 3) & 7;
    std::string s = "    { uint32_t v = " + src + "; uint8_t r = (uint8_t)(" + rexpr[k] + ");";
    if (live & FC) s += std::string(" gbr.cf = (uint8_t)(") + cexpr[k] + ");";
    s += mem ? " gbl_wr(gbr.hl, r);" : std::string(" ") + kR8[r] + " = r;";
    if (live & FZ) s += " gbr.zr = r;";
    s += " }";
    if (live & FN) s += " gbr.nf = 0;";
    if (live & FH) s += " gbr.hf = 0;";
    o << s << "\n";
}

void Generator::emit_ins(std::ostringstream& o, Ins& in, const Ins* following,
                         const std::string* replacement) {
    const uint8_t op = in.op;
    const uint8_t live = in.live_out;
    bool sync_after = false;
    const bool after_ei = ei_pending_;
    ei_pending_ = false;
    /* an instruction whose flags are all dead emits nothing: do not print
     * the indent-only line left behind */
    auto print = [&](const std::string& s) {
        if (s.find_first_not_of(' ') != std::string::npos) o << s << "\n";
    };

    if (in.label) {
        /* Loop heads poll interrupts and need gbr.cyc exact; elsewhere the
         * pending cycles carry across the label (see Ins::carry). */
        if (in.loop_head || opt_.precise) flush(o);
        in.carry = pend_;
        o << "L_" << hex4(in.addr) << ":\n";
        if (in.loop_head && !opt_.precise) o << "    SYNC(" << hex(in.addr, 4) << ");\n";
    }
    if (opt_.emit_comments && !replaying_) o << "    /* " << hex4(in.addr) << ": " << in.text << " */\n";
    if (opt_.precise) {
        flush(o);
        if (!after_ei) o << "    SYNC(" << hex(in.addr, 4) << ");\n";
    }
    pend_ += in.cycles;
    if (opt_.precise || in.spin_read) flush(o); /* a timed read sees the exact cycle */

    auto rd = [&](unsigned r) { return r == 6 ? std::string("gbl_rd(gbr.hl)") : std::string(kR8[r]); };

    if (in.flow == Flow::Bad) {
        flush(o);
        o << "    gbr.pc = " << hex(in.addr, 4) << "; gbl_exec(); return;\n";
        return;
    }

    if (replacement) {
        o << *replacement;
    } else if (op >= 0x40 && op < 0x80 && op != 0x76) {
        const unsigned d = (op >> 3) & 7, s = op & 7;
        if (d == 6) o << "    gbl_wr(gbr.hl, " << kR8[s] << ");\n";
        else if (d != s) o << "    " << kR8[d] << " = " << rd(s) << ";\n";
    } else if (op >= 0x80 && op < 0xC0) {
        emit_alu(o, (op >> 3) & 7, rd(op & 7), (op & 7) == 6, (op & 7) == 7, live);
    } else if (op == 0xCB) {
        emit_cb(o, in);
    } else {
        switch (op) {
        case 0x00: case 0x10: break;
        case 0x01: case 0x11: case 0x21: case 0x31:
            o << "    " << kR16[op >> 4] << " = " << hex(in.n16, 4) << ";\n";
            break;
        case 0x02: o << "    gbl_wr(gbr.bc, gbr.a);\n"; break;
        case 0x12: o << "    gbl_wr(gbr.de, gbr.a);\n"; break;
        case 0x22: o << "    gbl_wr(gbr.hl, gbr.a); gbr.hl++;\n"; break;
        case 0x32: o << "    gbl_wr(gbr.hl, gbr.a); gbr.hl--;\n"; break;
        case 0x0A: o << "    gbr.a = gbl_rd(gbr.bc);\n"; break;
        case 0x1A: o << "    gbr.a = gbl_rd(gbr.de);\n"; break;
        case 0x2A: o << "    gbr.a = gbl_rd(gbr.hl); gbr.hl++;\n"; break;
        case 0x3A: o << "    gbr.a = gbl_rd(gbr.hl); gbr.hl--;\n"; break;
        case 0x03: case 0x13: case 0x23: case 0x33: o << "    " << kR16[op >> 4] << "++;\n"; break;
        case 0x0B: case 0x1B: case 0x2B: case 0x3B: o << "    " << kR16[op >> 4] << "--;\n"; break;
        case 0x09: case 0x19: case 0x29: case 0x39: {
            const char* rr = kR16[op >> 4];
            if (live & FH) {
                o << "    { uint32_t v = " << rr << ", t = (uint32_t)gbr.hl + v; gbr.hf = (uint8_t)(((gbr.hl ^ v ^ t) >> 12) & 1);";
                if (live & FC) o << " gbr.cf = (uint8_t)(t >> 16);";
                o << " gbr.hl = (uint16_t)t; }";
                if (live & FN) o << " gbr.nf = 0;";
                o << "\n";
            } else {
                if (live & FC)
                    o << "    { uint32_t t = (uint32_t)gbr.hl + " << rr
                      << "; gbr.cf = (uint8_t)(t >> 16); gbr.hl = (uint16_t)t; }\n";
                else
                    o << "    gbr.hl = (uint16_t)(gbr.hl + " << rr << ");\n";
                if (live & FN) o << "    gbr.nf = 0;\n";
            }
            break;
        }
        case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: {
            const unsigned r = (op >> 3) & 7;
            const bool inc = (op & 1) == 0;
            {
                /* H inline too: the result's low nibble wrapped. */
                const std::string expr = "(uint8_t)(" + rd(r) + (inc ? " + 1)" : " - 1)");
                std::string s = "    { uint8_t t = " + expr + ";";
                s += r == 6 ? " gbl_wr(gbr.hl, t);" : std::string(" ") + kR8[r] + " = t;";
                if (live & FZ) s += " gbr.zr = t;";
                if (live & FH) s += inc ? " gbr.hf = (t & 0x0F) == 0;" : " gbr.hf = (t & 0x0F) == 0x0F;";
                s += " }";
                if (live & FN) s += inc ? " gbr.nf = 0;" : " gbr.nf = 1;";
                print(s);
            }
            break;
        }
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E: {
            const unsigned r = (op >> 3) & 7;
            if (r == 6) o << "    gbl_wr(gbr.hl, " << hex(in.n8, 2) << ");\n";
            else o << "    " << kR8[r] << " = " << hex(in.n8, 2) << ";\n";
            break;
        }
        case 0x07: case 0x0F: case 0x17: case 0x1F: {
            static const char* rexpr[4] = {"(v << 1) | (v >> 7)", "(v >> 1) | (v << 7)",
                                           "(v << 1) | gbr.cf", "(v >> 1) | ((uint32_t)gbr.cf << 7)"};
            static const char* cexpr[4] = {"v >> 7", "v & 1", "v >> 7", "v & 1"};
            const unsigned k = (op >> 3) & 3;
            std::string s = std::string("    { uint32_t v = gbr.a; gbr.a = (uint8_t)(") + rexpr[k] + ");";
            if (live & FC) s += std::string(" gbr.cf = (uint8_t)(") + cexpr[k] + ");";
            s += " }";
            if (live & FZ) s += " gbr.zr = 1;";
            if (live & FN) s += " gbr.nf = 0;";
            if (live & FH) s += " gbr.hf = 0;";
            print(s);
            break;
        }
        case 0x08: {
            bool dummy = false;
            const_write(o, in.n16, "(uint8_t)gbr.sp", dummy);
            const_write(o, static_cast<uint16_t>(in.n16 + 1), "(uint8_t)(gbr.sp >> 8)", dummy);
            if (dummy) sync_after = true;
            break;
        }
        case 0x27: o << "    gbl_daa();\n"; break;
        case 0x2F: {
            std::string s = "    gbr.a = (uint8_t)~gbr.a;";
            if (live & FN) s += " gbr.nf = 1;";
            if (live & FH) s += " gbr.hf = 1;";
            print(s);
            break;
        }
        case 0x37: case 0x3F: {
            std::string s = "   ";
            if (live & FC) s += op == 0x37 ? " gbr.cf = 1;" : " gbr.cf ^= 1;";
            if (live & FN) s += " gbr.nf = 0;";
            if (live & FH) s += " gbr.hf = 0;";
            print(s);
            break;
        }
        case 0xC1: o << "    gbr.bc = gbl_pop();\n"; break;
        case 0xD1: o << "    gbr.de = gbl_pop();\n"; break;
        case 0xE1: o << "    gbr.hl = gbl_pop();\n"; break;
        case 0xF1: o << "    { uint16_t v = gbl_pop(); gbr.a = (uint8_t)(v >> 8); gbl_setf((uint8_t)v); }\n"; break;
        case 0xC5: o << "    gbl_push(gbr.bc);\n"; break;
        case 0xD5: o << "    gbl_push(gbr.de);\n"; break;
        case 0xE5: o << "    gbl_push(gbr.hl);\n"; break;
        case 0xF5: o << "    gbl_push((uint16_t)((gbr.a << 8) | gbl_getf()));\n"; break;
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
            emit_alu(o, (op >> 3) & 7, hex(in.n8, 2), false, false, live);
            break;
        case 0xE0: const_write(o, static_cast<uint16_t>(0xFF00 | in.n8), "gbr.a", sync_after); break;
        case 0xF0: {
            const std::string v = const_read(o, static_cast<uint16_t>(0xFF00 | in.n8));
            o << "    gbr.a = " << v << ";\n";
            break;
        }
        case 0xE2:
            flush(o);
            o << "    gbl_wr((uint16_t)(0xFF00 | gbr.c), gbr.a);\n";
            sync_after = true;
            break;
        case 0xF2:
            flush(o);
            o << "    gbr.a = gbl_rd((uint16_t)(0xFF00 | gbr.c));\n";
            break;
        case 0xEA: const_write(o, in.n16, "gbr.a", sync_after); break;
        case 0xFA: {
            const std::string v = const_read(o, in.n16);
            o << "    gbr.a = " << v << ";\n";
            break;
        }
        case 0xE8: o << "    gbr.sp = gbl_spofs(" << hex(in.n8, 2) << ");\n"; break;
        case 0xF8: o << "    gbr.hl = gbl_spofs(" << hex(in.n8, 2) << ");\n"; break;
        case 0xF9: o << "    gbr.sp = gbr.hl;\n"; break;
        case 0xF3: o << "    gbr.ime = 0;\n"; break;
        case 0xFB:
            o << "    gbr.ime = 1; gbl_update_evt();\n";
            if (in.ei_dup) {
                /* The next instruction runs before interrupts can be taken:
                 * emit it here, poll, and continue after it. */
                Ins* n = find(in.bank, in.next());
                Ins* m = find(n->bank, n->next());
                Ins copy = *n;
                copy.label = copy.loop_head = copy.spin_head = copy.spin_read = false;
                copy.ei_dup = false;
                copy.spin = 0;
                replaying_ = true;
                emit_ins(o, copy, m);
                replaying_ = false;
                flush(o);
                o << "    SYNC(" << hex(m->addr, 4) << ");\n";
                emit_transfer(o, in, m->addr, true);
                return;
            }
            ei_pending_ = true;
            break;
        case 0x76:
            flush(o);
            o << "    if (gbl_halt(" << hex(in.next(), 4) << ")) return;\n";
            break;
        case 0x18: case 0xC3:
            flush(o);
            if (in.spin) emit_spin(o, in);
            emit_transfer(o, in, in.target, true);
            return;
        case 0x20: case 0x28: case 0x30: case 0x38:
        case 0xC2: case 0xCA: case 0xD2: case 0xDA:
            flush(o);
            o << "    if (" << cond_expr(op) << ") {\n";
            if (in.spin) {
                /* gbl_spin reads gbr.cyc: the taken cycles stay before it */
                o << "    CY(" << int(in.extra) << ");\n";
                emit_spin(o, in);
                emit_transfer(o, in, in.target, true);
            } else {
                emit_transfer(o, in, in.target, true, int(in.extra));
            }
            o << "    }\n";
            break;
        case 0xE9:
            flush(o);
            o << "    gbr.pc = gbr.hl; gbl_exec(); return;\n";
            return;
        case 0xCD:
        case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            flush(o);
            emit_call(o, in);
            break;
        case 0xC4: case 0xCC: case 0xD4: case 0xDC:
            flush(o);
            o << "    if (" << cond_expr(op) << ") {\n    CY(" << int(in.extra) << ");\n";
            emit_call(o, in);
            o << "    }\n";
            break;
        case 0xC9:
            flush(o);
            o << "    gbr.pc = gbl_pop(); return;\n";
            return;
        case 0xD9:
            flush(o);
            o << "    RETI(); return;\n";
            return;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8:
            flush(o);
            o << "    if (" << cond_expr(op) << ") { CY(" << int(in.extra)
              << "); gbr.pc = gbl_pop(); return; }\n";
            break;
        default:
            break;
        }
    }

    /* Sequential continuation. */
    if (after_ei && !opt_.precise) sync_after = true;
    if (sync_after && !opt_.precise) {
        flush(o);
        o << "    SYNC(" << hex(in.next(), 4) << ");\n";
    }
    if (!in.falls()) return;
    const bool adjacent = following && following->bank == in.bank && following->addr == in.next();
    if (!adjacent) {
        flush(o);
        emit_transfer(o, in, in.next(), true);
    }
}

void Generator::emit_region(std::ostringstream& o, int r) {
    const std::vector<size_t>& reg = regions_[r];
    cur_region_ = r;
    pend_ = 0;
    ei_pending_ = false;
    /* First pass only computes each label's carry (the cycles pending when
     * execution falls into it), which the switch and the gotos need. */
    for (int pass = 0; pass < 2; pass++) {
        std::ostringstream scratch;
        std::ostringstream& out = pass ? o : scratch;
        pend_ = 0;
        ei_pending_ = false;
        if (pass) {
            out << "\nvoid " << region_name(r) << "(void) {\n    ENTER();\n";
            if (!opt_.precise) out << "    SYNC(gbr.pc);\n";
            out << "    switch (gbr.pc) {\n";
            for (size_t idx : reg) {
                const Ins& in = ins_[idx];
                if (!in.sw_case) continue;
                out << "    case " << hex(in.addr, 4) << ": ";
                if (in.carry) out << "CY(" << -in.carry << "); ";
                out << "goto L_" << hex4(in.addr) << ";\n";
            }
            out << "    default: gbl_exec(); return;\n    }\n";
        }
        for (size_t k = 0; k < reg.size(); k++) {
            const Ins* following = k + 1 < reg.size() ? &ins_[reg[k + 1]] : nullptr;
            RegisterIR ir;
            size_t end = k;
            // Precise mode retains each observation point. EI's delayed poll
            // and spin-loop replay must keep their original instruction path.
            if (opt_.thumb && !opt_.precise && !ei_pending_) {
                while (end < reg.size()) {
                    const Ins& in = ins_[reg[end]];
                    if ((end != k && (in.label || ins_[reg[end - 1]].next() != in.addr)) ||
                        in.spin || in.spin_head || in.spin_read ||
                        !ir.append(in.op, in.n8, in.n16)) break;
                    ++end;
                }
            }
            if (end > k + 1) {
                const std::string code = ir.emit(), empty;
                if (pass) {
                    ++register_blocks_;
                    if (code.find("__asm__") != std::string::npos) ++thumb_blocks_;
                }
                for (size_t j = k; j < end; ++j) {
                    const Ins* next = j + 1 < reg.size() ? &ins_[reg[j + 1]] : nullptr;
                    emit_ins(out, ins_[reg[j]], next, j + 1 == end ? &code : &empty);
                }
                k = end - 1;
            } else emit_ins(out, ins_[reg[k]], following);
        }
        flush(out);
        if (pass) out << "}\n";
    }
}

std::string Generator::emit_code() {
    std::ostringstream o;
    o << "/* Generated by gbrecomp --target meta. Do not edit. */\n"
      << "#include \"gbl_gen.h\"\n\n"
      << "#if GBL_GEN_OPTIMIZE_SIZE && !defined(__clang__)\n#pragma GCC optimize(\"Os\")\n#endif\n"
      << "#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n";
    if (opt_.thumb) {
        o << "#include <stddef.h>\n";
        const char* fields[] = {"c", "b", "e", "d", "l", "h", "a"};
        const int offsets[] = {0, 1, 2, 3, 4, 5, 10};
        for (int i = 0; i < 7; ++i)
            o << "_Static_assert(offsetof(gbl_cpu_t, " << fields[i] << ") == " << offsets[i]
              << ", \"register IR layout\");\n";
    }
    o << "#if defined(__clang__) && GBL_GEN_OPTIMIZE_SIZE\n"
      << "#define GBL_GENERATED_ATTR __attribute__((minsize))\n#else\n"
      << "#define GBL_GENERATED_ATTR\n#endif\n";
    for (size_t r = 0; r < regions_.size(); r++)
        o << "GBL_GENERATED_ATTR void " << region_name(static_cast<int>(r)) << "(void);\n";
    for (size_t r = 0; r < regions_.size(); r++) emit_region(o, static_cast<int>(r));

    /* Dispatch tables: every switch entry, sorted per bank. */
    const unsigned banks = rom_.bank_count();
    std::vector<std::vector<const Ins*>> per_bank(banks);
    for (const Ins& in : ins_)
        if (in.sw_case) per_bank[in.bank].push_back(&in);
    o << "\n";
    if (ins_.empty()) o << "/* No coverage profile: every instruction runs in the interpreter. */\n";
    for (unsigned b = 0; b < banks; b++) {
        if (per_bank[b].empty()) continue;
        o << "static const uint16_t t" << b << "_addr[] GBL_TABLE = {";
        for (size_t i = 0; i < per_bank[b].size(); i++)
            o << (i % 12 ? " " : "\n    ") << hex(per_bank[b][i]->addr, 4) << ",";
        o << "\n};\nstatic const gbl_fn t" << b << "_fn[] GBL_TABLE = {";
        for (size_t i = 0; i < per_bank[b].size(); i++)
            o << (i % 6 ? " " : "\n    ") << region_name(per_bank[b][i]->region) << ",";
        /* gbl_lookup's index (gbl.h): first entry of each 1 KiB block, then
         * a bit per 128-byte page that holds an entry. */
        uint16_t idx[17 + 8] = {};
        for (unsigned k = 0, i = 0; k <= 16; k++) {
            while (i < per_bank[b].size() && (per_bank[b][i]->addr & 0x3FFFu) < k * 0x400u) i++;
            idx[k] = static_cast<uint16_t>(k == 16 ? per_bank[b].size() : i);
        }
        for (const Ins* in : per_bank[b]) {
            const unsigned page = (in->addr & 0x3FFFu) >> 7;
            idx[17 + page / 16] = static_cast<uint16_t>(idx[17 + page / 16] | (1u << (page % 16)));
        }
        o << "\n};\nstatic const uint16_t t" << b << "_idx[] GBL_TABLE = {";
        for (unsigned k = 0; k < 17 + 8; k++) o << (k % 12 ? " " : "\n    ") << hex(idx[k], 4) << ",";
        o << "\n};\n";
    }
    o << "const gbl_bank_table gbl_tables[" << banks << "] = {\n";
    for (unsigned b = 0; b < banks; b++) {
        if (per_bank[b].empty()) o << "    {0, 0, 0, 0},\n";
        else
            o << "    {" << per_bank[b].size() << ", t" << b << "_addr, t" << b << "_fn, t" << b << "_idx},\n";
    }
    o << "};\nconst uint16_t gbl_table_count = " << banks << ";\n";
    return o.str();
}

namespace {
void emit_bytes(std::ostringstream& o, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i % 24 == 0) o << "\n";
        o << unsigned(d[i]) << ",";
    }
}
} // namespace

/* Host only: the whole ROM, which a streaming firmware reads from the SD. */
std::string Generator::emit_rom_full() const {
    std::ostringstream o;
    o << "/* Whole ROM for the host harness of a streaming project (the META\n"
      << " * firmware reads it from the SD card). */\n#include <stdint.h>\n\n"
      << "const uint8_t gbl_rom_full[" << rom_.size() << "] __attribute__((aligned(4))) = {";
    emit_bytes(o, rom_.data(), rom_.size());
    o << "\n};\n";
    return o.str();
}

std::vector<uint8_t> Generator::espboy_image() const {
    if (!pack_) return {}; // the ROM is in the application
    std::vector<uint8_t> image;
    for (size_t k = 32; k < packed_sector_.size(); ++k)
        image.insert(image.end(), packed_sector_[k].begin(), packed_sector_[k].end());
    image.insert(image.end(), {0, 0});
    return image;
}

std::string Generator::emit_rom() const {
    std::ostringstream o;
    if (opt_.espboy && pack_) {
        o << "#include <stdint.h>\n#if defined(ESP8266)\n#include <pgmspace.h>\n#define ROM_ATTR PROGMEM __attribute__((aligned(4)))\n#else\n#define ROM_ATTR __attribute__((aligned(4)))\n#endif\n"
          << "const uint32_t gbl_rom_size = " << rom_.size() << "u;\n"
          << "const uint8_t gbl_rom_chunk_map[" << chunk_weight_.size() << "] = {0};\n"
          << "const uint8_t gbl_rom[16384] ROM_ATTR = {";
        emit_bytes(o, rom_.data(), 16384);
        o << "\n};\n#if !defined(ESP8266)\nconst uint8_t gbl_rom_packed[] = {";
        auto image = espboy_image();
        emit_bytes(o, image.data(), image.size());
        o << "\n};\nconst uint8_t gbl_rom_resident[1][4096] = {{0}};\n#endif\n"
          << "const uint32_t gbl_rom_packed_off[] ROM_ATTR = {";
        size_t offset = 0;
        for (const auto& sector : packed_sector_) { o << offset << "u,"; offset += sector.size(); }
        o << offset << "u};\n";
        return o.str();
    }
    if (stream_) {
        const size_t chunks = chunk_weight_.size();
        std::vector<size_t> slots;
        for (size_t c = 4; c < chunks; c++)
            if (resident_index_[c] >= 0) slots.push_back(c);
        std::sort(slots.begin(), slots.end(),
                  [&](size_t a, size_t b) { return resident_index_[a] < resident_index_[b]; });
        o << "/* ROM for gbl, streaming build: bank 0 and the resident 4 KiB chunks\n"
          << " * in flash, every other chunk read from the SD card. */\n#include <stdint.h>\n\n"
          << "const uint32_t gbl_rom_size = " << rom_.size() << "u;\n"
          << "const uint8_t gbl_rom[16384] __attribute__((aligned(4))) = {";
        emit_bytes(o, rom_.data(), 16384);
        o << "\n};\nconst uint8_t gbl_rom_resident[" << (slots.empty() ? 1 : slots.size())
          << "][4096] __attribute__((aligned(4))) = {";
        for (size_t c : slots) {
            o << "\n{";
            emit_bytes(o, rom_.data() + c * 4096, std::min<size_t>(4096, rom_.size() - c * 4096));
            o << "\n},";
        }
        if (slots.empty()) o << "{0}";
        o << "\n};\n/* Per chunk: 1 + slot in gbl_rom_resident, or 0 on the card. */\n"
          << "const uint8_t gbl_rom_chunk_map[" << chunks << "] = {";
        for (size_t c = 0; c < chunks; c++) {
            if (c % 32 == 0) o << "\n";
            o << (c < 4 ? 0 : resident_index_[c] + 1) << ",";
        }
        o << "\n};\n";
        if (pack_) {
            /* Sectors of streamed chunks, LZSS-packed (512 bytes: stored). */
            std::vector<uint8_t> blob;
            std::vector<uint32_t> offs;
            for (size_t k = 0; k < packed_sector_.size(); k++) {
                offs.push_back(static_cast<uint32_t>(blob.size()));
                const size_t c = k / 8;
                if (k >= 32 && c < chunks && resident_index_[c] < 0 && (!partial_pack_ || packed_keep_[k]))
                    blob.insert(blob.end(), packed_sector_[k].begin(), packed_sector_[k].end());
            }
            offs.push_back(static_cast<uint32_t>(blob.size()));
            blob.push_back(0);
            blob.push_back(0); /* the decoders may read two bytes ahead */
            o << "/* Streamed sectors, LZSS-packed in flash (GBL_ROM_PACKED). */\n"
              << "const uint8_t gbl_rom_packed[" << blob.size() << "] = {";
            emit_bytes(o, blob.data(), blob.size());
            o << "\n};\nconst uint32_t gbl_rom_packed_off[" << offs.size() << "] = {";
            for (size_t k = 0; k < offs.size(); k++) {
                if (k % 12 == 0) o << "\n";
                o << offs[k] << "u,";
            }
            o << "\n};\n";
        }
        return o.str();
    }
    o << "/* ROM image for gbl (flash resident). */\n#include <stdint.h>\n\n"
      << "const uint32_t gbl_rom_size = " << rom_.size() << "u;\n"
      << "#if defined(ESP8266)\n#include <pgmspace.h>\n#define GBL_ROM_ATTR PROGMEM __attribute__((aligned(4)))\n#else\n#define GBL_ROM_ATTR __attribute__((aligned(4)))\n#endif\n"
      << "const uint8_t gbl_rom[" << rom_.size() << "] GBL_ROM_ATTR = {";
    const uint8_t* d = rom_.data();
    for (size_t i = 0; i < rom_.size(); i++) {
        if (i % 24 == 0) o << "\n";
        o << unsigned(d[i]) << ",";
    }
    o << "\n};\n";
    return o.str();
}

/* The Game Boy Color boot ROM coloured DMG games from their title: the
 * 8-bit sum of the 16 raw title bytes selects one of its palette
 * combinations (checksums that collide are told apart by the 4th title
 * letter), and the combination's BG third becomes the game's four
 * colours.  Resolved here, at conversion time, so the firmware only ever
 * embeds the one palette it needs (four colours) instead of the tables.
 * Transcribed from the SameBoy boot ROM reimplementation (LIJI32/SameBoy,
 * BootROMs/cgb_boot.asm), which matches the original BIOS. */
struct CgbPalette {
    /* The boot ROM's 30 base palettes, BGR555, lightest first. */
    static const uint16_t base[30][4];
    /* BG byte offset into the flattened base palettes, per combination
     * (51 of them; the four raw ones read across two base palettes). */
    static const uint8_t comb_bg[51];
    /* Title checksums (65 unique), then 29 that repeat and are told apart
     * by the 4th title letter (dup_letters, aligned with dup_checksums). */
    static const uint8_t checksums[65];
    static const uint8_t dup_checksums[29];
    static const char dup_letters[30]; /* 29 letters + the terminator */
    /* BG combination per title, indexed like the checksum tables. */
    static const uint8_t bg_combo[94];
};
const uint16_t CgbPalette::base[30][4] = {
    {0x7FFF, 0x32BF, 0x00D0, 0x0000}, {0x639F, 0x4279, 0x15B0, 0x04CB},
    {0x7FFF, 0x6E31, 0x454A, 0x0000}, {0x7FFF, 0x1BEF, 0x0200, 0x0000},
    {0x7FFF, 0x421F, 0x1CF2, 0x0000}, {0x7FFF, 0x5294, 0x294A, 0x0000},
    {0x7FFF, 0x03FF, 0x012F, 0x0000}, {0x7FFF, 0x03EF, 0x01D6, 0x0000},
    {0x7FFF, 0x42B5, 0x3DC8, 0x0000}, {0x7E74, 0x03FF, 0x0180, 0x0000},
    {0x67FF, 0x77AC, 0x1A13, 0x2D6B}, {0x7ED6, 0x4BFF, 0x2175, 0x0000},
    {0x53FF, 0x4A5F, 0x7E52, 0x0000}, {0x4FFF, 0x7ED2, 0x3A4C, 0x1CE0},
    {0x03ED, 0x7FFF, 0x255F, 0x0000}, {0x036A, 0x021F, 0x03FF, 0x7FFF},
    {0x7FFF, 0x01DF, 0x0112, 0x0000}, {0x231F, 0x035F, 0x00F2, 0x0009},
    {0x7FFF, 0x03EA, 0x011F, 0x0000}, {0x299F, 0x001A, 0x000C, 0x0000},
    {0x7FFF, 0x027F, 0x001F, 0x0000}, {0x7FFF, 0x03E0, 0x0206, 0x0120},
    {0x7FFF, 0x7EEB, 0x001F, 0x7C00}, {0x7FFF, 0x3FFF, 0x7E00, 0x001F},
    {0x7FFF, 0x03FF, 0x001F, 0x0000}, {0x03FF, 0x001F, 0x000C, 0x0000},
    {0x7FFF, 0x033F, 0x0193, 0x0000}, {0x0000, 0x4200, 0x037F, 0x7FFF},
    {0x7FFF, 0x7E8C, 0x7C00, 0x0000}, {0x7FFF, 0x1BEF, 0x6180, 0x0000},
};
const uint8_t CgbPalette::comb_bg[51] = {
    232, 144, 160, 192, 72, 0, 216, 40, 96, 208, 64, 224, 16, 32, 232, 224,
    16, 64, 56, 144, 160, 72, 44, 16, 16, 24, 0, 0, 8, 144, 160, 192, 64,
    104, 56, 60, 36, 80, 224, 16, 16, 24, 0, 32, 32, 0, 224, 64, 224, 48, 232,
};
const uint8_t CgbPalette::checksums[65] = {
    0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
    0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
    0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
    0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
    0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
    0x5D, 0x6D, 0x67, 0x3F, 0x6B,
};
const uint8_t CgbPalette::dup_checksums[29] = {
    0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF,
    0x0D, 0xF4, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66,
    0x6A, 0xBF, 0x0D, 0xF4, 0xB3,
};
const char CgbPalette::dup_letters[30] = "BEFAARBEKEK R-URAR INAILICE R";
const uint8_t CgbPalette::bg_combo[94] = {
    0, 4, 5, 35, 34, 3, 31, 15, 10, 5, 19, 36, 7, 37, 30, 44, 21, 32, 31, 20,
    5, 33, 13, 14, 5, 29, 5, 18, 9, 3, 2, 26, 25, 25, 41, 42, 26, 45, 42, 45,
    36, 38, 26, 42, 30, 41, 34, 34, 5, 42, 6, 5, 33, 25, 42, 42, 40, 2, 16,
    25, 42, 42, 5, 0, 39, 36, 22, 25, 6, 32, 12, 36, 11, 39, 18, 39, 24, 31,
    50, 17, 46, 6, 27, 0, 47, 41, 41, 0, 0, 19, 34, 23, 18, 29,
};

/* The game's GBC boot ROM colours (0xRRGGBB, lightest first), or false
 * when the boot ROM would not have coloured it (no Nintendo licensee, or
 * a title it does not know). */
static bool cgb_game_palette(const ROM& rom, const ROMHeader& h,
                             uint32_t out[4], unsigned* combo_out) {
    bool nintendo = h.old_licensee_code == 0x01 ||
                    h.new_licensee_code == "01";
    if (!nintendo) return false;
    uint8_t sum = 0, fourth = 0;
    for (unsigned i = 0; i < 16; i++) {
        uint8_t b = rom.read_banked(0, (uint16_t)(0x134u + i));
        sum += b;
        if (i == 3) fourth = b;
    }
    for (unsigned i = 0; i < 94; i++) {
        uint8_t ck = i < 65 ? CgbPalette::checksums[i]
                            : CgbPalette::dup_checksums[i - 65];
        if (ck != sum) continue;
        if (i >= 65 && CgbPalette::dup_letters[i - 65] != (char)fourth) continue;
        unsigned combo = CgbPalette::bg_combo[i];
        unsigned off = CgbPalette::comb_bg[combo];
        for (unsigned k = 0; k < 4; k++) {
            unsigned word = off / 2 + k;
            uint16_t bgr = CgbPalette::base[word / 4][word % 4];
            unsigned r = (bgr & 31) * 255 / 31;
            unsigned g = ((bgr >> 5) & 31) * 255 / 31;
            unsigned b = ((bgr >> 10) & 31) * 255 / 31;
            out[k] = r << 16 | g << 8 | b;
        }
        if (combo_out) *combo_out = combo;
        return true;
    }
    return false;
}

/* The boot ROM's generic palettes, which its key combinations select at
 * power-on (a direction, plus A and/or B): the 30 base palettes and the
 * 12 combinations as (OBJ0, OBJ1, BG) base indices — the games only show
 * the BG third today, the triplets are there for the day sprites get
 * their own palettes. */
static void cgb_generic_palettes(std::string& out) {
    static const uint8_t combos[12][3] = {
        {18, 18, 18}, /* Right */
        {4, 3, 28},   /* Left */
        {0, 0, 0},    /* Up */
        {12, 12, 12}, /* Down */
        {4, 4, 29},   /* Right + A */
        {4, 0, 2},    /* Left + A */
        {3, 28, 4},   /* Up + A */
        {24, 24, 24}, /* Down + A */
        {27, 27, 27}, /* Right + B */
        {5, 5, 5},    /* Left + B */
        {0, 0, 1},    /* Up + B */
        {28, 3, 6},   /* Down + B */
    };
    auto rgb = [](unsigned word) {
        uint16_t bgr = CgbPalette::base[word / 4][word % 4];
        unsigned r = (bgr & 31) * 255 / 31;
        unsigned g = ((bgr >> 5) & 31) * 255 / 31;
        unsigned b = ((bgr >> 10) & 31) * 255 / 31;
        char buf[16];
        snprintf(buf, sizeof buf, "0x%02X%02X%02Xu", r, g, b);
        return std::string(buf);
    };
    out += "/* Palettes generiques du boot ROM GBC : les 30 de base, puis les\n"
           " * 12 combos touches (OBJ0, OBJ1, BG) en indice de base — la\n"
           " * separation BG/sprites consommera les trois colonnes. */\n"
           "#define GBL_GBC_BASE ";
    for (unsigned p = 0; p < 30; p++) {
        out += "{";
        for (unsigned k = 0; k < 4; k++) out += rgb(p * 4 + k) + ((k < 3) ? ", " : "");
        out += p < 29 ? "}, " : "}";
    }
    out += "\n#define GBL_GBC_COMBOS ";
    for (unsigned c = 0; c < 12; c++)
        out += "{" + std::to_string(combos[c][0]) + "," + std::to_string(combos[c][1]) + "," +
                std::to_string(combos[c][2]) + (c < 11 ? "}, " : "}");
    out += "\n";
}

/* ESPboy: identity of the game in the save area, and its state slots. */
std::string Generator::espboy_state_config(size_t eram, bool battery) const {
    uint32_t id = 2166136261u;
    for (size_t i = 0; i < rom_.size(); i++) id = (id ^ rom_.data()[i]) * 16777619u;
    const size_t sectors = 1 + (18432 + eram + 511) / 512, slot_blocks = (sectors + 7) / 8;
    const size_t ram_blocks = battery ? 2 * ((eram + 4095) / 4096 + 1) : 0;
    const size_t slots = std::min<size_t>(10, kEspSaveBlocks > ram_blocks ? (kEspSaveBlocks - ram_blocks) / slot_blocks : 0);
    std::ostringstream o;
    o << "#define GBL_FLASH_GAME_ID 0x" << std::hex << id << std::dec << "u\n";
    if (slots) o << "#define GBL_STATE_SLOTS " << slots << "\n#define GBL_STATE_SLOT_SECTORS " << sectors << "\n";
    return o.str();
}

std::string Generator::emit_config(std::string& error) const {
    const ROMHeader& h = rom_.header();
    int mbc = -1;
    switch (h.mbc_type) {
    case MBCType::NONE: case MBCType::ROM_RAM: case MBCType::ROM_RAM_BATTERY: mbc = 0; break;
    case MBCType::MBC1: case MBCType::MBC1_RAM: case MBCType::MBC1_RAM_BATTERY: mbc = 1; break;
    case MBCType::MBC2: case MBCType::MBC2_BATTERY: mbc = 2; break;
    case MBCType::MBC3: case MBCType::MBC3_RAM: case MBCType::MBC3_RAM_BATTERY:
    case MBCType::MBC3_TIMER_BATTERY: case MBCType::MBC3_TIMER_RAM_BATTERY: mbc = 3; break;
    case MBCType::MBC5: case MBCType::MBC5_RAM: case MBCType::MBC5_RAM_BATTERY:
    case MBCType::MBC5_RUMBLE: case MBCType::MBC5_RUMBLE_RAM:
    case MBCType::MBC5_RUMBLE_RAM_BATTERY: mbc = 5; break;
    default: break;
    }
    if (mbc < 0) {
        error = std::string("unsupported cartridge type for the gbl runtime: ") +
                mbc_type_name(h.mbc_type);
        return {};
    }
    if (h.is_cgb_only) {
        error = "CGB-only ROMs are not supported by the DMG-only gbl runtime";
        return {};
    }
    size_t eram = mbc == 2 ? 512 : h.ram_size_bytes;
    if (opt_.espboy && eram > 16384) {
        error = "ESPboy keeps the cartridge RAM in its 80 KiB of RAM: " + std::to_string(eram) +
                " bytes are more than the 16 KiB supported";
        return {};
    }
    /* Battery-backed cartridge types (header byte 0x147). */
    const uint8_t cart = rom_.read_banked(0, 0x147);
    const bool battery = cart == 0x03 || cart == 0x06 || cart == 0x09 || cart == 0x0D ||
                         cart == 0x0F || cart == 0x10 || cart == 0x13 || cart == 0x1B ||
                         cart == 0x1E || cart == 0xFF;
    unsigned banks = rom_.bank_count();
    const std::string sd = opt_.sd_name.empty() ? stream_name(opt_.name) : opt_.sd_name;
    std::ostringstream o;
    o << "/* Generated cartridge configuration for gbl. */\n#ifndef GBL_CONFIG_H\n#define GBL_CONFIG_H\n"
      << "#define GBL_MBC " << mbc << "\n"
      << "#define GBL_ROM_BANKS " << banks << "\n"
      << "#define GBL_ERAM_SIZE " << eram << "\n"
      << (battery && eram ? "#define GBL_BATTERY 1\n" : "")
      /* The game's files on the card share the .BIN's 8.3 base; the
       * firmware looks for them in the folder of its own .BIN. */
      << (opt_.espboy ? std::string() : "#define GBL_SD_NAME \"" + sd + "\"\n#define GBL_BIN_NAME83 \"" + name83(sd, "BIN") + "\"\n")
      << (eram && !opt_.espboy ? "#define GBL_SAVE_NAME \"" + sd + ".SAV\"\n#define GBL_SAVE_NAME83 \"" + name83(sd, "SAV") + "\"\n"
               : std::string())
      /* Save states: GBL_STATE_SLOTS slots of GBL_STATE_SLOT_SECTORS sectors
       * (a header, the machine within 18 KiB, the cartridge RAM) in a file
       * the converters create; the firmware offers them when it is there.
       * ESPboy: the slots that fit in the save area, after the two copies
       * of the battery RAM. */
      << (opt_.espboy ? espboy_state_config(eram, battery && eram)
                      : "#define GBL_STATE_NAME \"" + sd + ".STA\"\n#define GBL_STATE_NAME83 \"" + name83(sd, "STA") +
                            "\"\n#define GBL_STATE_SLOTS 10\n#define GBL_STATE_SLOT_SECTORS " +
                            std::to_string(1 + (18432 + eram + 511) / 512) + "\n")
      << (stream_ ? "#define GBL_ROM_STREAM 1\n#define GBL_ROM_CHUNKS " +
                        std::to_string(chunk_weight_.size()) + "\n" +
                         (pack_ ? std::string("#define GBL_ROM_PACKED 1\n") : std::string()) +
                         (!pack_ || partial_pack_ ? "#define GBL_ROM_SD 1\n#define GBL_STREAM_NAME \"" + sd +
                                                        ".GB\"\n#define GBL_STREAM_NAME83 \"" + name83(sd, "GB") + "\"\n"
                                                  : std::string())
                  : std::string())
      /* The console; its optimisation switches are in gbl_profile.h. */
      << (opt_.espboy
              ? "#define GBL_TARGET_ESPBOY 1\n#define GBL_ERAM_ON_SD 0\n"
              : std::string())
      << (opt_.pc ? "#define GBL_TARGET_PC 1\n#define GBL_ERAM_ON_SD 0\n"
                    /* the PC window plays the DMG's own 160x144 (NES and SG
                     * write the same geometry into their configs); guarded,
                     * m0_target.h keeps its defaults elsewhere */
                    "#ifndef PC_ROWS\n#define PC_ROWS 144\n#endif\n"
                    "#ifndef M0_SCREEN_ROWS\n#define M0_SCREEN_ROWS 144\n#endif\n"
                  : std::string())
      << (opt_.pokitto ? "#define GBL_TARGET_POKITTO 1\n" : "")
      << (opt_.odroid ? "#define GBL_TARGET_ODROID 1\n" : "")
      << (opt_.aka ? "#define GBL_TARGET_AKA 1\n#define GBL_ERAM_ON_SD 0\n" : "")
      << "#define GBL_TITLE \"" << [&] {
             std::string t;
             for (char c : h.title)
                 if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') t += c;
             return t;
         }() << "\"\n"
      /* The CGB boot ROM picks a game's palette from the 8-bit sum of the
       * 16 raw title bytes (spaces and zeros included, GBL_TITLE drops
       * them); the resolution runs here, so the firmware only embeds the
       * one palette when the boot ROM knows this title. */
      << "#define GBL_TITLE_CKSUM " << [&] {
             unsigned s = 0;
             for (unsigned i = 0; i < 16; i++)
                 s += rom_.read_banked(0, 0x134u + i);
             return s & 0xFF;
         }() << "\n"
      << [&] {
             uint32_t pal[4];
             unsigned combo;
             if (!cgb_game_palette(rom_, h, pal, &combo)) return std::string();
             char buf[128];
             snprintf(buf, sizeof buf,
                      "/* GBC boot ROM palette for this title (combination %u). */\n"
                      "#define GBL_PALETTE_GBC 0x%06Xu, 0x%06Xu, 0x%06Xu, 0x%06Xu\n",
                      combo, pal[0], pal[1], pal[2], pal[3]);
             return std::string(buf);
         }()
      << [&] {
             std::string s;
             cgb_generic_palettes(s);
             return s;
         }()
      << "#define GBL_GEN_VERSION \"" << (opt_.espboy ? GBRECOMP_ESPBOY_VERSION : opt_.pc ? GBRECOMP_PC_VERSION
                       : opt_.odroid ? GBRECOMP_ODROID_VERSION : opt_.aka ? GBRECOMP_AKA_VERSION
                       : opt_.pokitto ? GBRECOMP_POKITTO_VERSION : GBRECOMP_META_VERSION) << "\"\n"
      << "#endif\n";
    return o.str();
}

bool write_file(const fs::path& path, const std::string& text, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

/* Regular files of `from` into `to`, templates (*.in) left out. */
bool copy_dir_files(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        error = "missing template directory " + from.string();
        return false;
    }
    fs::create_directories(to, ec);
    for (const auto& entry : fs::directory_iterator(from, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() == ".in") continue;
        fs::copy_file(entry.path(), to / entry.path().filename(),
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy " + entry.path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::string substitute(std::string text, const std::string& name) {
    const std::string token = "@NAME@";
    for (size_t p = text.find(token); p != std::string::npos; p = text.find(token, p))
        text.replace(p, token.size(), name);
    return text;
}

bool read_file(const fs::path& path, std::string& text, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path.string();
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

} // namespace

bool generate_meta_project(const ROM& rom, const MetaTargetOptions& options,
                           MetaTargetStats& stats, std::string& error) {
    if (options.compiler != "gcc" && options.compiler != "clang") {
        error = "firmware compiler must be gcc or clang";
        return false;
    }
    if (options.espboy + options.pokitto + options.pc + options.odroid + options.aka > 1) {
        error = "choose one destination console";
        return false;
    }
    if (options.espboy && (options.stream == 1 || options.thumb || options.compiler != "gcc")) {
        error = "ESPboy requires portable C/GCC and a flash-resident ROM (no SD or --meta-thumb)";
        return false;
    }
    if (options.pc && (options.stream == 1 || options.thumb)) {
        error = "the PC embeds the whole ROM in its executable (no SD streaming, no --meta-thumb)";
        return false;
    }
    if (options.esp32_console() && (options.stream == 1 || options.thumb)) {
        error = "the ESP32 consoles embed the ROM in their application partition (no SD streaming yet, no --meta-thumb)";
        return false;
    }
    Generator gen(rom, options);
    if (gen.streaming() && !gen.plan_stream(stats, error)) return false;
    if (!gen.load_coverage(stats, error)) return false;
    if (gen.streaming()) {
        gen.choose_resident(stats);
        if (!stats.packed || stats.sd) stats.stream_file =
            (options.sd_name.empty() ? stream_name(options.name) : options.sd_name) + ".GB";
    }
    gen.build();

    const std::string config = gen.emit_config(error);
    if (config.empty()) return false;

    const fs::path out = options.output_dir;
    const fs::path src = out / "src";
    const fs::path tpl = options.template_dir;
    std::error_code ec;
    /* The templates: targets/meta (the Game Boy core's runtime and the
     * frontend in platform/, shared by every console), targets/common (the
     * console-shared frontend files, m0_*) and targets/consoles/<console>
     * (each console's drivers and project templates, *.in; the META also has
     * a PlatformIO board directory).  Files the other console left in this
     * directory (an earlier generation, or an earlier name: the meta_* ones
     * before they became m0_*) are removed: the builds compile every source
     * in src/.  So is the Arduino variant directory of META projects
     * generated before the firmware went bare metal.  */
    const char* console = options.espboy ? "espboy" : options.pc ? "pc"
                         : options.odroid ? "odroid" : options.aka ? "aka"
                         : options.pokitto ? "pokitto" : "meta";
    const fs::path consoles = tpl.parent_path() / "consoles";
    const fs::path common = tpl.parent_path() / "common";
    const fs::path platform_tpl = consoles / console;
    fs::create_directories(src, ec);
    fs::create_directories(out / "host", ec);
    fs::create_directories(out / "coverage", ec);
    if (ec) {
        error = "cannot create " + out.string() + ": " + ec.message();
        return false;
    }
    for (const char* other : {"meta", "pokitto", "espboy", "pc", "odroid", "aka"}) {
      if (std::string(other) == console) continue;
      for (const auto& entry : fs::directory_iterator(consoles / other, ec)) {
        const fs::path name = entry.path().filename();
        if (entry.is_directory()) fs::remove_all(out / name, ec); /* boards/ */
        else if (name.extension() == ".in" && name.stem() != "README.md") fs::remove(out / name.stem(), ec);
    }
    }
    /* Template files in src/ that this generation does not copy (the other
     * console's, or renamed since) would be compiled too: remove them. */
    {
        std::set<std::string> fresh;
        for (const fs::path& dir : {tpl / "runtime", tpl / "platform", common / "platform", platform_tpl})
            for (const auto& entry : fs::directory_iterator(dir, ec))
                if (entry.is_regular_file()) fresh.insert(entry.path().filename().string());
        std::vector<fs::path> stale;
        for (const auto& entry : fs::directory_iterator(src, ec)) {
            const std::string n = entry.path().filename().string();
            const bool templated = n.rfind("gbl_", 0) == 0 || n.rfind("meta_", 0) == 0 || n.rfind("m0_", 0) == 0 || n.rfind("gb_", 0) == 0 ||
                                   n.rfind("pokitto_", 0) == 0 || n.rfind("espboy_", 0) == 0 || n.rfind("pc_", 0) == 0 ||
                                   n.rfind("esp32_", 0) == 0 || n.rfind("odroid_", 0) == 0 || n.rfind("aka_", 0) == 0 || n == "lpc11u6x.h";
            if (templated && n != "gbl_config.h" && !fresh.count(n)) stale.push_back(entry.path());
        }
        for (const auto& path : stale) fs::remove(path, ec);
    }
    if (!copy_dir_files(tpl / "runtime", src, error) ||
        !copy_dir_files(tpl / "platform", src, error) ||
        !copy_dir_files(common / "platform", src, error) ||
        !copy_dir_files(platform_tpl, src, error))
        return false;
    if (options.esp32_console()) { /* the family's shared core (esp32_hw,
        * esp32_sd) the two consoles build on */
        if (!copy_dir_files(tpl.parent_path() / "common" / "consoles" / "esp32", src, error))
            return false;
    }
    if (options.pokitto) /* its own display modes (pokitto_display.c): the GB
                         * scaled modes' C and layout stay out */
        for (const char* name : {"gbl_display.c", "gbl_display_layout.h"}) fs::remove(src / name, ec);
    if (options.espboy) {
        const auto bytes = gen.espboy_image();
        if (!bytes.empty()) {
        std::ofstream image(out / "rom.bin", std::ios::binary);
        image.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!image) { error = "cannot write ESPboy ROM flash image"; return false; }
        }
        for (const char* name : {"gbl_display.c", "gbl_display_layout.h"})
            fs::copy_file(tpl / "platform" / name, src / name, fs::copy_options::overwrite_existing);
    }
    if (!copy_dir_files(common / "toolchain", out / "toolchain", error)) return false;
    fs::copy_file(tpl.parent_path() / "common/runtime/m0_rom_codec.h", src / "m0_rom_codec.h",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "cannot copy shared ROM codecs: " + ec.message(); return false; }
    fs::copy_file(tpl.parent_path() / "common/runtime/m0_rom_lzss.inc", src / "m0_rom_lzss.inc",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { error = "cannot copy shared ROM assembler: " + ec.message(); return false; }
    for (const char* name : { "m0_cache.h", "m0_cache.inc", "m0_stack_pool.h", "m0_copy.h", "m0_copy.inc", "m0_scale.h" }) {
        fs::copy_file(tpl.parent_path() / "common/runtime" / name, src / name, fs::copy_options::overwrite_existing, ec);
        if (ec) { error = std::string("cannot copy the shared card cache: ") + ec.message(); return false; }
    }
    if (!options.espboy || gen.espboy_image().empty()) fs::remove(out / "rom.bin", ec);
    fs::remove_all(out / "variants", ec);
    /* Subdirectories (the META's boards/) as they are. */
    for (const auto& entry : fs::directory_iterator(platform_tpl)) {
        if (!entry.is_directory()) continue;
        std::error_code cec;
        fs::copy(entry.path(), out / entry.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, cec);
        if (cec) {
            error = "cannot copy " + entry.path().string() + ": " + cec.message();
            return false;
        }
    }
    fs::copy_file(tpl / "host" / "gbl_host.c", out / "host" / "gbl_host.c",
                  fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::copy_file(tpl / "host" / "gbl_metaview.c", out / "host" / "gbl_metaview.c",
                      fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::copy_file(tpl / "host" / "prefer_compiler.cmake", out / "host" / "prefer_compiler.cmake",
                      fs::copy_options::overwrite_existing, ec);
    /* The explorer and the ROM trace shared with the SG-1000 and Game Gear harnesses. */
    if (!ec)
        fs::copy_file(tpl.parent_path() / "common" / "host" / "m0_explore.h", out / "host" / "m0_explore.h",
                      fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::copy_file(tpl.parent_path() / "common" / "host" / "m0_rom_trace.h", out / "host" / "m0_rom_trace.h",
                      fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy host harness: " + ec.message();
        return false;
    }
    /* Project files: every *.in of the console's directory, @NAME@ filled. */
    for (const auto& entry : fs::directory_iterator(platform_tpl, ec)) {
        if (entry.is_directory() || entry.path().extension() != ".in") continue;
        std::string text;
        if (!read_file(entry.path(), text, error)) return false;
        text = substitute(text, options.name);
        const std::string token = "@COMPILER@";
        for (size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos))
            text.replace(pos, token.size(), options.compiler);
        if (!write_file(out / entry.path().stem(), text, error)) return false;
    }
    {
        std::string text;
        if (!read_file(tpl / "host" / "CMakeLists.txt.in", text, error)) return false;
        if (!write_file(out / "host" / "CMakeLists.txt", substitute(text, options.name), error))
            return false;
    }
    for (size_t i = 0; i < options.coverage_files.size(); i++) {
        fs::copy_file(options.coverage_files[i],
                      out / "coverage" / options.coverage_files[i].filename(),
                      fs::copy_options::overwrite_existing, ec);
    }

    /* A previous generation into this directory may have used another
     * prefix; its generated sources would be linked in as well. */
    for (const auto& entry : fs::directory_iterator(src, ec)) {
        const std::string fname = entry.path().filename().string();
        const bool generated = (fname.size() > 6 && fname.compare(fname.size() - 6, 6, "_gen.c") == 0) ||
                               (fname.size() > 6 && fname.compare(fname.size() - 6, 6, "_rom.c") == 0);
        if (generated && fname != options.name + "_gen.c" && fname != options.name + "_rom.c")
            fs::remove(entry.path(), ec);
    }
    if (!write_file(src / "gbl_config.h", config, error) ||
        !write_file(src / (options.name + "_gen.c"), gen.emit_code(), error) ||
        !write_file(src / (options.name + "_rom.c"), gen.emit_rom(), error))
        return false;
    /* The host harness of a streaming project needs the whole ROM. */
    for (const auto& entry : fs::directory_iterator(out / "host", ec)) {
        const std::string fname = entry.path().filename().string();
        if (fname.size() > 10 && fname.compare(fname.size() - 10, 10, "_romfull.c") == 0)
            fs::remove(entry.path(), ec);
    }
    if (gen.streaming() &&
        !write_file(out / "host" / (options.name + "_romfull.c"), gen.emit_rom_full(), error))
        return false;

    stats.compiled = gen.compiled();
    stats.dropped = gen.dropped();
    stats.regions = gen.regions();
    stats.dispatch_entries = gen.entries();
    stats.register_blocks = gen.register_blocks();
    stats.thumb_blocks = gen.thumb_blocks();
    stats.spin_loops = gen.spin_loops();
    stats.counted_loops = gen.counted_loops();
    return true;
}

} // namespace gbrecomp
