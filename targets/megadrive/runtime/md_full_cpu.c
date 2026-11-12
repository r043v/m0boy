#include <stdlib.h>
/* 68000 reference interpreter and the line scheduler.  Times come from
 * md_m68k_time.h, semantics from md_m68k_ops.h: the recompiled C uses the
 * same two, so both count and do the same thing (run_hash must match). */
#include "md_m68k_ops.h"

typedef struct { unsigned kind, reg; uint32_t addr, imm; } Ea;

#ifdef MD_TRACE
MD_THREAD_LOCAL MdTrace md_trace;
#define MD_QUIET(x) (++md_trace.quiet, (x))
#define MD_UNQUIET() (--md_trace.quiet)
#define MD_FETCHED(n) (md_trace.fetched += (n))
#else
#define MD_QUIET(x) (x)
#define MD_UNQUIET() ((void)0)
#define MD_FETCHED(n) ((void)0)
#endif
static uint16_t fetch16(MdFull *m) { const uint16_t v = MD_QUIET(md_rd16(m, m->pc)); MD_UNQUIET(); MD_FETCHED(2u); m->pc += 2u; return v; }
static uint32_t fetch32(MdFull *m) { const uint32_t v = MD_QUIET(md_rd32(m, m->pc)); MD_UNQUIET(); MD_FETCHED(4u); m->pc += 4u; return v; }
static uint32_t index_disp(MdFull *m, uint32_t base) {
    const uint16_t ext = fetch16(m);
    const unsigned reg = (ext >> 12) & 7u;
    int32_t index = (ext & 0x8000u) ? (int32_t)m->a[reg] : (int32_t)m->d[reg];
    if (!(ext & 0x0800u)) index = (int16_t)index;
    return base + (uint32_t)index + (uint32_t)(int32_t)(int8_t)ext;
}
/* Kinds 0..6 as the mode field, 7 absolute or PC-relative, 8 immediate. */
static Ea ea(MdFull *m, unsigned bits, unsigned size) {
    Ea e = {bits >> 3, bits & 7u, 0, 0};
    switch (e.kind) {
    case 0: case 1: break;
    case 2: e.addr = m->a[e.reg]; break;
    case 3: e.addr = m->a[e.reg]; m->a[e.reg] += size == 1u && e.reg == 7u ? 2u : size; break;
    case 4: m->a[e.reg] -= size == 1u && e.reg == 7u ? 2u : size; e.addr = m->a[e.reg]; break;
    case 5: e.addr = m->a[e.reg] + (uint32_t)(int32_t)(int16_t)fetch16(m); break;
    case 6: e.addr = index_disp(m, m->a[e.reg]); break;
    default:
        switch (e.reg) {
        case 0: e.addr = (uint32_t)(int32_t)(int16_t)fetch16(m); break;
        case 1: e.addr = fetch32(m); break;
        case 2: { const uint32_t pc = m->pc; e.addr = pc + (uint32_t)(int32_t)(int16_t)fetch16(m); break; }
        case 3: { const uint32_t pc = m->pc; e.addr = index_disp(m, pc); break; }
        case 4: e.kind = 8; e.imm = size == 4u ? fetch32(m) : size == 2u ? fetch16(m) : (fetch16(m) & 0xffu); break;
        default: m->fault = 2; break;
        }
    }
    return e;
}
static uint32_t ea_read(MdFull *m, Ea e, unsigned size) {
    if (e.kind == 0u) return m->d[e.reg] & md_mask(size);
    if (e.kind == 1u) return m->a[e.reg] & md_mask(size);
    if (e.kind == 8u) return e.imm & md_mask(size);
    return md_rd(m, e.addr, size);
}
static void ea_write(MdFull *m, Ea e, uint32_t v, unsigned size) {
    if (e.kind == 0u) md_setd(m, e.reg, v, size);
    else if (e.kind == 1u) m->a[e.reg] = md_sext(v, size);
    else if (e.kind == 8u) m->fault = 2;
    else md_wr(m, e.addr, size, v);
}
static uint32_t ea_address(MdFull *m, unsigned bits) {
    const Ea e = ea(m, bits, 4);
    if (e.kind <= 1u || e.kind == 8u) m->fault = 2;
    return e.addr;
}

void md_full_execute(MdFull *m, uint16_t op) {
    const unsigned top = op >> 12, reg = (op >> 9) & 7u, bits = op & 63u, mode = (op >> 3) & 7u;
    const uint32_t start = m->fault_pc;
    unsigned size;
    m->last_opcode = op;
    md_spend(m, md_m68k_cycles(op));
    ++m->instructions;
    switch (top) {
    case 1: case 2: case 3: {
        size = top == 1u ? 1u : top == 2u ? 4u : 2u;
        const uint32_t v = ea_read(m, ea(m, bits, size), size);
        const unsigned dmode = (op >> 6) & 7u;
        if (dmode == 1u) { m->a[reg] = md_sext(v, size); return; }
        ea_write(m, ea(m, dmode << 3 | reg, size), v, size);
        md_logic(m, v, size);
        return;
    }
    case 7:
        if (op & 0x0100u) break;
        m->d[reg] = (uint32_t)(int32_t)(int8_t)op;
        md_logic(m, m->d[reg], 4);
        return;
    case 6: {
        const unsigned cc = (op >> 8) & 15u;
        int32_t disp = (int8_t)op;
        if (!(op & 255u)) disp = (int16_t)fetch16(m);
        const uint32_t target = start + 2u + (uint32_t)disp;
        if (cc == 1u) { md_push32(m, m->pc); m->pc = target; return; }
        if (cc == 0u) { m->pc = target; return; }
        if (md_cc(m, cc)) { md_spend(m, (unsigned)(int)md_m68k_branch_taken(op)); m->pc = target; }
        return;
    }
    case 5:
        if ((op & 0x00c0u) == 0x00c0u) {
            const unsigned cc = (op >> 8) & 15u;
            if (mode == 1u) {
                const int16_t disp = (int16_t)fetch16(m);
                if (md_cc(m, cc)) return;
                const uint16_t v = (uint16_t)(m->d[op & 7u] - 1u);
                md_setd(m, op & 7u, v, 2);
                if (v != 0xffffu) { md_spend(m, (unsigned)MD_DBCC_LOOP); m->pc = start + 2u + (uint32_t)(int32_t)disp; }
                else md_spend(m, MD_DBCC_EXPIRED);
                return;
            }
            const int t = md_cc(m, cc);
            if (t && mode == 0u) md_spend(m, MD_SCC_TRUE_DN);
            ea_write(m, ea(m, bits, 1), t ? 0xffu : 0u, 1);
            return;
        } else {
            size = 1u << ((op >> 6) & 3u);
            const uint32_t v = reg ? reg : 8u;
            if (mode == 1u) { m->a[op & 7u] += (op & 0x0100u) ? (uint32_t)-v : v; return; }
            const Ea dst = ea(m, bits, size);
            const uint32_t old = ea_read(m, dst, size);
            ea_write(m, dst, (op & 0x0100u) ? md_sub(m, old, v, size) : md_add(m, old, v, size), size);
            return;
        }
    case 0: {
        if ((op & 0x0138u) == 0x0108u) {                   /* MOVEP */
            const uint32_t at = m->a[op & 7u] + (uint32_t)(int32_t)(int16_t)fetch16(m);
            const unsigned n = (op & 0x0040u) ? 4u : 2u;
            if (op & 0x0080u)
                for (unsigned i = 0; i < n; ++i) md_wr8(m, at + 2u * i, (uint8_t)(m->d[reg] >> (8u * (n - 1u - i))));
            else {
                uint32_t v = 0;
                for (unsigned i = 0; i < n; ++i) v = v << 8 | md_rd8(m, at + 2u * i);
                md_setd(m, reg, v, n);
            }
            return;
        }
        if ((op & 0x0100u) || (op & 0x0f00u) == 0x0800u) {  /* BTST/BCHG/BCLR/BSET */
            unsigned bit = (op & 0x0100u) ? m->d[reg] : (fetch16(m) & 0xffu);
            const unsigned kind = (op >> 6) & 3u;
            size = mode == 0u ? 4u : 1u;
            const Ea dst = ea(m, bits, size);
            if (mode == 0u && kind && (bit & 31u) >= 16u) md_spend(m, 2u);   /* register bit ops: +2 for bits 16-31 */
            bit &= size == 4u ? 31u : 7u;
            const uint32_t old = ea_read(m, dst, size), b = 1u << bit;
            m->sr = (uint16_t)((m->sr & ~MD_Z) | (old & b ? 0 : MD_Z));
            if (kind) ea_write(m, dst, kind == 1u ? old ^ b : kind == 2u ? old & ~b : old | b, size);
            return;
        }
        if ((op & 0xffbfu) == 0x003cu || (op & 0xffbfu) == 0x023cu || (op & 0xffbfu) == 0x0a3cu) {
            const uint16_t v = fetch16(m);
            const unsigned k = (op >> 9) & 7u;
            if (op & 0x0040u) {                              /* to SR */
                md_set_sr(m, k == 0u ? m->sr | v : k == 1u ? m->sr & v : m->sr ^ v);
            } else {
                const uint16_t c = v & 0x1fu;
                m->sr = (uint16_t)(k == 0u ? m->sr | c : k == 1u ? m->sr & (0xffe0u | c) : m->sr ^ c);
            }
            return;
        }
        const unsigned k = (op >> 9) & 7u;
        size = (op >> 6) & 3u;
        if (size == 3u || k == 4u || k == 7u) break;
        size = 1u << size;
        const uint32_t imm = size == 4u ? fetch32(m) : (fetch16(m) & md_mask(size));
        const Ea dst = ea(m, bits, size);
        const uint32_t old = ea_read(m, dst, size);
        switch (k) {
        case 0: { const uint32_t r = old | imm; ea_write(m, dst, r, size); md_logic(m, r, size); break; }
        case 1: { const uint32_t r = old & imm; ea_write(m, dst, r, size); md_logic(m, r, size); break; }
        case 2: ea_write(m, dst, md_sub(m, old, imm, size), size); break;
        case 3: ea_write(m, dst, md_add(m, old, imm, size), size); break;
        case 5: { const uint32_t r = old ^ imm; ea_write(m, dst, r, size); md_logic(m, r, size); break; }
        default: md_cmp(m, old, imm, size); break;
        }
        return;
    }
    case 4: {
        switch (op) {
        case 0x4e70: return;                                 /* RESET: devices only */
        case 0x4e71: return;
        case 0x4e72: { const uint16_t v = fetch16(m); md_set_sr(m, v); m->stopped = 1; return; }
        case 0x4e73: {
            const uint16_t sr = md_pop16(m);
            m->pc = md_pop32(m);
            md_set_sr(m, sr);
            return;
        }
        case 0x4e75: m->pc = md_pop32(m); return;
        case 0x4e76: if (m->sr & MD_V) { md_spend(m, 30); md_exception(m, 7, m->pc); } return;
        case 0x4e77: {
            const uint16_t ccr = md_pop16(m);
            m->sr = (uint16_t)((m->sr & 0xff00u) | (ccr & 0x1fu));
            m->pc = md_pop32(m);
            return;
        }
        default: break;
        }
        if ((op & 0xfff0u) == 0x4e40u) { md_exception(m, 32u + (op & 15u), m->pc); return; }
        if ((op & 0xfff8u) == 0x4e50u) {
            const int16_t disp = (int16_t)fetch16(m);
            md_push32(m, m->a[op & 7u]);
            m->a[op & 7u] = m->a[7];
            m->a[7] += (uint32_t)(int32_t)disp;
            return;
        }
        if ((op & 0xfff8u) == 0x4e58u) { m->a[7] = m->a[op & 7u]; m->a[op & 7u] = md_pop32(m); return; }
        if ((op & 0xfff0u) == 0x4e60u) {
            if (op & 8u) m->a[op & 7u] = m->usp; else m->usp = m->a[op & 7u];
            return;
        }
        if ((op & 0xffc0u) == 0x4e80u || (op & 0xffc0u) == 0x4ec0u) {
            const uint32_t target = ea_address(m, bits);
            if (!(op & 0x0040u)) md_push32(m, m->pc);
            m->pc = target;
            return;
        }
        if ((op & 0xf1c0u) == 0x41c0u) { m->a[reg] = ea_address(m, bits); return; }
        if ((op & 0xf1c0u) == 0x4180u) {                     /* CHK.W */
            const int16_t bound = (int16_t)ea_read(m, ea(m, bits, 2), 2), v = (int16_t)m->d[reg];
            if (v < 0 || v > bound) {
                m->sr = (uint16_t)((m->sr & ~MD_N) | (v < 0 ? MD_N : 0));
                md_spend(m, 30);
                md_exception(m, 6, m->pc);
            }
            return;
        }
        if ((op & 0xfff8u) == 0x4840u) {
            const uint32_t v = m->d[op & 7u];
            m->d[op & 7u] = v << 16 | v >> 16;
            md_logic(m, m->d[op & 7u], 4);
            return;
        }
        if ((op & 0xffc0u) == 0x4840u) { md_push32(m, ea_address(m, bits)); return; }
        if ((op & 0xffb8u) == 0x4880u) {
            const unsigned r = op & 7u;
            if (op & 0x0040u) { m->d[r] = (uint32_t)(int32_t)(int16_t)m->d[r]; md_logic(m, m->d[r], 4); }
            else { md_setd(m, r, (uint32_t)(int32_t)(int8_t)m->d[r], 2); md_logic(m, m->d[r], 2); }
            return;
        }
        if ((op & 0xfb80u) == 0x4880u) {                     /* MOVEM */
            size = (op & 0x0040u) ? 4u : 2u;
            const uint16_t list = fetch16(m);
            const unsigned ar = op & 7u;
            unsigned count = 0;
            for (uint16_t l = list; l; l &= (uint16_t)(l - 1u)) ++count;
            md_spend(m, count * (size == 4u ? 8u : 4u));
            if (mode == 4u) {                                /* registers to -(An), A7 first */
                uint32_t addr = m->a[ar];
                for (unsigned r = 0; r < 16u; ++r) if (list & (1u << r)) {
                    addr -= size;
                    md_wr(m, addr, size, r < 8u ? m->a[7u - r] : m->d[15u - r]);
                }
                m->a[ar] = addr;
                return;
            }
            uint32_t addr = mode == 3u ? m->a[ar] : ea_address(m, bits);
            if (op & 0x0400u) {
                for (unsigned r = 0; r < 16u; ++r) if (list & (1u << r)) {
                    const uint32_t v = md_sext(md_rd(m, addr, size), size);
                    addr += size;
                    if (r < 8u) m->d[r] = v; else m->a[r - 8u] = v;
                }
                if (mode == 3u) m->a[ar] = addr;
            } else
                for (unsigned r = 0; r < 16u; ++r) if (list & (1u << r)) {
                    md_wr(m, addr, size, r < 8u ? m->d[r] : m->a[r - 8u]);
                    addr += size;
                }
            return;
        }
        if ((op & 0xffc0u) == 0x40c0u) { ea_write(m, ea(m, bits, 2), m->sr, 2); return; }
        if ((op & 0xffc0u) == 0x44c0u) {
            const uint32_t v = ea_read(m, ea(m, bits, 2), 2);
            m->sr = (uint16_t)((m->sr & 0xff00u) | (v & 0x1fu));
            return;
        }
        if ((op & 0xffc0u) == 0x46c0u) { md_set_sr(m, (uint16_t)ea_read(m, ea(m, bits, 2), 2)); return; }
        if ((op & 0xffc0u) == 0x4800u) {                     /* NBCD */
            const Ea dst = ea(m, bits, 1);
            ea_write(m, dst, md_sbcd(m, 0, (uint8_t)ea_read(m, dst, 1)), 1);
            return;
        }
        if ((op & 0xffc0u) == 0x4ac0u) {                     /* TAS */
            const Ea dst = ea(m, bits, 1);
            const uint32_t v = ea_read(m, dst, 1);
            md_logic(m, v, 1);
            ea_write(m, dst, v | 0x80u, 1);
            return;
        }
        const unsigned k = (op >> 8) & 15u;
        size = (op >> 6) & 3u;
        if (size == 3u || (k != 0u && k != 2u && k != 4u && k != 6u && k != 10u)) break;
        size = 1u << size;
        const Ea dst = ea(m, bits, size);
        const uint32_t old = ea_read(m, dst, size);
        switch (k) {
        case 0: ea_write(m, dst, md_subx(m, 0, old, size), size); break;
        case 2: ea_write(m, dst, 0, size); md_logic(m, 0, size); break;
        case 4: ea_write(m, dst, md_sub(m, 0, old, size), size); break;
        case 6: ea_write(m, dst, ~old, size); md_logic(m, ~old, size); break;
        default: md_logic(m, old, size); break;
        }
        return;
    }
    case 8: case 9: case 11: case 12: case 13: {
        const unsigned opmode = (op >> 6) & 7u;
        if ((top == 9u || top == 11u || top == 13u) && (opmode == 3u || opmode == 7u)) {
            size = opmode == 3u ? 2u : 4u;
            const uint32_t v = md_sext(ea_read(m, ea(m, bits, size), size), size);
            if (top == 9u) m->a[reg] -= v;
            else if (top == 13u) m->a[reg] += v;
            else md_cmp(m, m->a[reg], v, 4);
            return;
        }
        if ((top == 8u || top == 12u) && (opmode == 3u || opmode == 7u)) {
            const uint16_t v = (uint16_t)ea_read(m, ea(m, bits, 2), 2);
            if (top == 12u) { if (opmode == 3u) md_mulu(m, reg, v); else md_muls(m, reg, v); }
            else if (opmode == 3u) md_divu(m, reg, v, m->pc);
            else md_divs(m, reg, v, m->pc);
            return;
        }
        if ((top == 8u || top == 12u) && opmode == 4u && mode <= 1u) {   /* SBCD/ABCD */
            const unsigned ry = op & 7u;
            if (!mode) {
                const uint8_t r = top == 8u ? md_sbcd(m, (uint8_t)m->d[reg], (uint8_t)m->d[ry])
                                            : md_abcd(m, (uint8_t)m->d[reg], (uint8_t)m->d[ry]);
                md_setd(m, reg, r, 1);
            } else {
                m->a[ry] -= ry == 7u ? 2u : 1u;
                const uint8_t src = md_rd8(m, m->a[ry]);
                m->a[reg] -= reg == 7u ? 2u : 1u;
                const uint8_t dst = md_rd8(m, m->a[reg]);
                md_wr8(m, m->a[reg], top == 8u ? md_sbcd(m, dst, src) : md_abcd(m, dst, src));
            }
            return;
        }
        if (top == 12u && (op & 0x0130u) == 0x0100u && (opmode == 5u || opmode == 6u)) {   /* EXG */
            const unsigned r = op & 7u;
            uint32_t *x = opmode == 5u && mode == 1u ? &m->a[reg] : &m->d[reg];
            uint32_t *y = mode == 1u ? &m->a[r] : &m->d[r];
            const uint32_t t = *x; *x = *y; *y = t;
            return;
        }
        if ((top == 9u || top == 13u) && opmode >= 4u && mode <= 1u) {  /* SUBX/ADDX */
            size = 1u << (opmode - 4u);
            const unsigned ry = op & 7u;
            if (!mode) {
                const uint32_t r = top == 9u ? md_subx(m, m->d[reg], m->d[ry], size) : md_addx(m, m->d[reg], m->d[ry], size);
                md_setd(m, reg, r, size);
            } else {
                m->a[ry] -= size == 1u && ry == 7u ? 2u : size;
                const uint32_t src = md_rd(m, m->a[ry], size);
                m->a[reg] -= size == 1u && reg == 7u ? 2u : size;
                const uint32_t dst = md_rd(m, m->a[reg], size);
                md_wr(m, m->a[reg], size, top == 9u ? md_subx(m, dst, src, size) : md_addx(m, dst, src, size));
            }
            return;
        }
        if (top == 11u && opmode >= 4u && mode == 1u) {    /* CMPM */
            size = 1u << (opmode - 4u);
            const unsigned ry = op & 7u;
            const uint32_t src = md_rd(m, m->a[ry], size);
            m->a[ry] += size == 1u && ry == 7u ? 2u : size;
            const uint32_t dst = md_rd(m, m->a[reg], size);
            m->a[reg] += size == 1u && reg == 7u ? 2u : size;
            md_cmp(m, dst, src, size);
            return;
        }
        if (top == 11u && opmode >= 4u) {                  /* EOR */
            size = 1u << (opmode - 4u);
            const Ea dst = ea(m, bits, size);
            const uint32_t r = ea_read(m, dst, size) ^ m->d[reg];
            ea_write(m, dst, r, size);
            md_logic(m, r, size);
            return;
        }
        if (top == 11u) {                                  /* CMP */
            size = 1u << opmode;
            md_cmp(m, m->d[reg], ea_read(m, ea(m, bits, size), size), size);
            return;
        }
        size = 1u << (opmode & 3u);
        if (opmode <= 2u) {
            const uint32_t v = ea_read(m, ea(m, bits, size), size), old = m->d[reg];
            uint32_t r;
            if (top == 8u) { r = old | v; md_logic(m, r, size); }
            else if (top == 12u) { r = old & v; md_logic(m, r, size); }
            else if (top == 9u) r = md_sub(m, old, v, size);
            else r = md_add(m, old, v, size);
            md_setd(m, reg, r, size);
        } else {
            const Ea dst = ea(m, bits, size);
            const uint32_t old = ea_read(m, dst, size), v = m->d[reg];
            uint32_t r;
            if (top == 8u) { r = old | v; md_logic(m, r, size); }
            else if (top == 12u) { r = old & v; md_logic(m, r, size); }
            else if (top == 9u) r = md_sub(m, old, v, size);
            else r = md_add(m, old, v, size);
            ea_write(m, dst, r, size);
        }
        return;
    }
    case 14: {
        if (((op >> 6) & 3u) == 3u) {                     /* memory, one bit, word */
            if (op & 0x0800u) break;
            const Ea dst = ea(m, bits, 2);
            const uint32_t v = ea_read(m, dst, 2);
            ea_write(m, dst, md_shift(m, (op >> 9) & 3u, !!(op & 0x0100u), v, 1, 2), 2);
            return;
        }
        size = 1u << ((op >> 6) & 3u);
        unsigned count;
        if (op & 0x0020u) { count = m->d[reg] & 63u; md_spend(m, 2u * count); }
        else count = reg ? reg : 8u;
        const unsigned r = op & 7u;
        md_setd(m, r, md_shift(m, (op >> 3) & 3u, !!(op & 0x0100u), m->d[r], count, size), size);
        return;
    }
    default: break;
    }
    /* Line A, line F and what is left: illegal-instruction exceptions. */
    md_exception(m, top == 10u ? 10u : top == 15u ? 11u : 4u, start);
}

void md_full_step_interp(MdFull *m) {
    if (m->fault) return;
    ++m->fallback_hits;
    const uint32_t pc = m->pc;
    m->fault_pc = pc;
    if (m->coverage && pc < m->coverage_bytes * 8u) {
        const uint8_t bit = (uint8_t)(1u << (pc & 7u));
        if (!(m->coverage[pc >> 3] & bit)) {
            m->coverage[pc >> 3] |= bit;
            m->coverage_locations++; /* the explorer's novelty */
        }
    }
    const uint16_t op = MD_QUIET(md_rd16(m, pc));   /* timed by md_full_execute */
    MD_UNQUIET();
    m->pc = pc + 2u;
    if (m->fault) return;
#ifdef MD_TRACE
    md_trace.fetched = 0;
    if (md_trace.step) md_trace.step(md_trace.user, m, pc, op);
    if (m->fault) return;   /* a budget cut: the instruction must not run (its effects would replay) */
#endif
    md_full_execute(m, op);
#ifdef MD_TRACE
    if (md_trace.end) md_trace.end(md_trace.user, m, pc, m->pc);
#endif
    /* An idle loop's test just ran: spend the rest of the spin at once, as
     * the recompiled loop does (md_m68k_idle_loop). */
    if (pc < md_rom_size && ((op & 0xff00u) == 0x4a00u || (op & 0xff00u) == 0x0800u || (op & 0xff00u) == 0x0c00u) &&
        m->frame_cycles < m->slice_limit) {
        const unsigned len = md_m68k_idle_loop(md_rom, md_rom_size, pc);
        if (len) {
            const uint16_t bop = MD_QUIET(md_rd16(m, pc + len));
            MD_UNQUIET();
            if (md_cc(m, (bop >> 8) & 15u)) md_full_idle(m, md_m68k_cycles(op), 10u, pc, pc + len);
        }
    }
}

/* The spin of an idle loop, entered just after its test (time `a`, the
 * branch back taking `b`): ends where stepping would have, on the first
 * instruction boundary at or past the slice's end. */
void md_full_idle(MdFull *m, unsigned a, unsigned b, uint32_t test_pc, uint32_t branch_pc) {
    const uint32_t left = m->slice_limit - m->frame_cycles, p = a + b, q = left / p, r = left % p;
    if (!r) { md_spend(m, q * p); m->pc = branch_pc; }
    else if (r <= b) { md_spend(m, q * p + b); m->pc = test_pc; }
    else { md_spend(m, (q + 1u) * p); m->pc = branch_pc; }
}

static int interrupt(MdFull *m) {
    const unsigned mask = (m->sr >> 8) & 7u;
    unsigned level;
    if (m->irq_pending && (m->vdp_reg[1] & 0x20u) && mask < 6u) { level = 6; m->irq_pending = 0; }
    else if (m->hint_pending && (m->vdp_reg[0] & 0x10u) && mask < 4u) { level = 4; m->hint_pending = 0; }
    else return 0;
    md_spend(m, 44);
    const uint16_t old = m->sr;
    md_set_sr(m, (uint16_t)(((old | MD_S) & ~0x8000u & 0xf8ffu) | (level << 8)));
    md_push32(m, m->pc);
    md_push16(m, old);
    m->pc = md_rd32(m, (24u + level) * 4u);
    m->stopped = 0;
    return 1;
}

/* One slice: until frame_cycles reaches `end`.  Interrupts are looked at
 * before each instruction (interpreter) or each region entry (generated
 * code, which leaves its region whenever one could become possible). */
/* Reference-core interrupt latency (Clown68000): the pending level is latched before an instruction and
 * entered after it, so one more instruction runs between the request and the stacked return address. */
static unsigned irq_latched_level(const MdFull *m) {
    if (m->irq_pending && (m->vdp_reg[1] & 0x20u)) return 6;
    if (m->hint_pending && (m->vdp_reg[0] & 0x10u)) return 4;
    return 0;
}

static void run_slice(MdFull *m, uint32_t end, int native) {
    static int latency = -1;
    if (latency < 0) latency = getenv("MD_IRQ_IMMEDIATE") == NULL;   /* default: the reference latency */
    while (m->frame_cycles < end && !m->fault) {
        unsigned latched = 0;
        if (latency && !native && !m->stopped) {
            latched = irq_latched_level(m);
            if (latched) {
                m->slice_limit = end;
                md_full_step_interp(m);
                if (m->fault) break;
                if (latched > ((m->sr >> 8) & 7u)) interrupt(m);
                continue;
            }
        } else interrupt(m);
        if (m->stopped) { md_spend(m, end - m->frame_cycles); break; }
        m->slice_limit = end;
        if (native) md_full_step_native(m); else md_full_step_interp(m);
        if (m->instructions > 2000000000u) { m->fault = 6; break; }
    }
}

enum { LINE_CYCLES = 488, FRAME_LINES = 262, ACTIVE_LINES = 224, VBLANK_LINES = 38 };
/* A frame starts at the vertical interrupt (line 0 here is VDP line 224), like the reference core's frame and
 * the way pads are latched: the game reads its input in the V-int that opens the frame. Active line n is slot 38+n. */

void md_full_frame(MdFull *m, int native) {
    for (unsigned line = 0; line < FRAME_LINES && !m->fault; ++line) {
        m->line = (uint16_t)line;
        if (line == 0) m->irq_pending = !m->no_vint0;        /* vertical blank */
        run_slice(m, (line + 1u) * (m->line_master ? m->line_master : 3420u) / 7u, native);
        if (line >= VBLANK_LINES) {
            /* the frontend hook first (the ESP32 one captures the line's
             * registers for its second core), then the reference row */
            if (m->line_hook) m->line_hook(m->hook_user, m, line - VBLANK_LINES);
            if (m->screen) md_full_render_line(m, line - VBLANK_LINES, m->screen + (line - VBLANK_LINES) * 320u);
        }
        /* The horizontal counter counts active lines (and the first blank
         * one) and is reloaded during the vertical blank. */
        if (line == 0 || line >= VBLANK_LINES) {
            if (m->hint_counter == 0) { m->hint_counter = (uint8_t)m->vdp_reg[10]; m->hint_pending = 1; }
            else --m->hint_counter;
        } else m->hint_counter = (uint8_t)m->vdp_reg[10];
    }
    m->frame_cycles -= FRAME_LINES * (m->line_master ? m->line_master : 3420u) / 7u;
    m->no_vint0 = 0;
    ++m->frames;
    md_full_sound_frame(m);
}

uint32_t md_full_hash(const MdFull *m) {
    uint32_t h = 2166136261u;
    const uint8_t *data[] = {(const uint8_t *)m->d, (const uint8_t *)m->a,
        (const uint8_t *)m->ram, (const uint8_t *)m->vram, (const uint8_t *)m->cram};
    const size_t sizes[] = {sizeof m->d, sizeof m->a, sizeof m->ram, sizeof m->vram, sizeof m->cram};
    for (unsigned j = 0; j < 5; ++j)
        for (size_t i = 0; i < sizes[j]; ++i) h = (h ^ data[j][i]) * 16777619u;
    for (unsigned i = 0; i < 4; ++i) h = (h ^ ((m->pc >> (i * 8)) & 255u)) * 16777619u;
    return h;
}
