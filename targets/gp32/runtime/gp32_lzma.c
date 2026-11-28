/* LZMA "alone" decoder: properties byte, dictionary size (4 bytes), unpacked
   size (8 bytes), then the range coded stream.  The whole output is in memory,
   so it also serves as the dictionary. */
#include "gp32_lzma.h"

#include <stdlib.h>
#include <string.h>

typedef uint16_t Prob;

typedef struct {
    const uint8_t *in, *end;
    uint32_t range, code;
    bool overrun;
} Range;

static void normalize(Range *r)
{
    if (r->range >= 1u << 24)
        return;
    r->range <<= 8;
    r->code <<= 8;
    if (r->in < r->end)
        r->code |= *r->in++;
    else
        r->overrun = true;
}

static unsigned bit(Range *r, Prob *prob)
{
    normalize(r);
    uint32_t bound = (r->range >> 11) * *prob;
    if (r->code < bound) {
        r->range = bound;
        *prob = (Prob)(*prob + ((2048 - *prob) >> 5));
        return 0;
    }
    r->range -= bound;
    r->code -= bound;
    *prob = (Prob)(*prob - (*prob >> 5));
    return 1;
}

static unsigned tree(Range *r, Prob *probs, unsigned bits)
{
    unsigned m = 1;
    for (unsigned i = 0; i < bits; ++i)
        m = (m << 1) | bit(r, probs + m);
    return m - (1u << bits);
}

static unsigned reverse_tree(Range *r, Prob *probs, unsigned bits)
{
    unsigned m = 1, value = 0;
    for (unsigned i = 0; i < bits; ++i) {
        unsigned b = bit(r, probs + m);
        m = (m << 1) | b;
        value |= b << i;
    }
    return value;
}

typedef struct {
    Prob choice[2], low[16][8], mid[16][8], high[256];
} Length;

static unsigned length(Range *r, Length *l, unsigned position)
{
    if (!bit(r, &l->choice[0]))
        return tree(r, l->low[position], 3);
    if (!bit(r, &l->choice[1]))
        return 8 + tree(r, l->mid[position], 3);
    return 16 + tree(r, l->high, 8);
}

/* Every member is made of Prob, so the model can be initialised as one array. */
typedef struct {
    Prob match[12][16], rep[12], rep0[12], rep1[12], rep2[12], long0[12][16];
    Prob slot[4][64], special[115], align[16];
    Length matched, repeated;
} Model;

bool gp32_lzma_decode(uint8_t *out, size_t size, const uint8_t *in, size_t in_size)
{
    if (in_size < 18 || in[0] >= 9 * 5 * 5)
        return false;
    unsigned lc = in[0] % 9, lp = in[0] / 9 % 5, pb = in[0] / 45;
    if (lc + lp > 4)
        return false;
    uint64_t stated = 0;
    for (int i = 0; i < 8; ++i)
        stated |= (uint64_t)in[5 + i] << (8 * i);
    if (stated != size && stated != UINT64_MAX)
        return false;
    size_t literals = (size_t)0x300 << (lc + lp);
    Model *m = malloc(sizeof(*m) + literals * sizeof(Prob));
    if (!m)
        return false;
    Prob *literal = (Prob *)(m + 1);
    for (size_t i = 0; i < sizeof(*m) / sizeof(Prob) + literals; ++i)
        ((Prob *)m)[i] = 1024;

    Range r = {in + 14, in + in_size, UINT32_MAX, 0, false};
    for (int i = 0; i < 4; ++i)
        r.code = (r.code << 8) | *r.in++;
    unsigned state = 0;
    size_t at = 0, rep0 = 1, rep1 = 1, rep2 = 1, rep3 = 1; /* distances */
    bool ok = true;
    while (ok && at < size && !r.overrun) {
        unsigned position = (unsigned)at & ((1u << pb) - 1);
        unsigned len;
        if (!bit(&r, &m->match[state][position])) {
            unsigned previous = at ? out[at - 1] : 0;
            Prob *probs = literal + 0x300 * ((((unsigned)at & ((1u << lp) - 1)) << lc) +
                                             (previous >> (8 - lc)));
            unsigned symbol = 1;
            if (state >= 7) {
                if (rep0 > at)
                    break;
                unsigned match = out[at - rep0];
                while (symbol < 0x100) {
                    unsigned expected = (match >> 7) & 1;
                    match <<= 1;
                    unsigned b = bit(&r, probs + ((1 + expected) << 8) + symbol);
                    symbol = (symbol << 1) | b;
                    if (b != expected)
                        break;
                }
            }
            while (symbol < 0x100)
                symbol = (symbol << 1) | bit(&r, probs + symbol);
            out[at++] = (uint8_t)symbol;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        if (bit(&r, &m->rep[state])) {
            if (!bit(&r, &m->rep0[state])) {
                if (!bit(&r, &m->long0[state][position])) {
                    if (rep0 > at)
                        break;
                    out[at] = out[at - rep0];
                    ++at;
                    state = state < 7 ? 9 : 11;
                    continue;
                }
            } else {
                size_t distance;
                if (!bit(&r, &m->rep1[state])) {
                    distance = rep1;
                } else {
                    if (!bit(&r, &m->rep2[state])) {
                        distance = rep2;
                    } else {
                        distance = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = distance;
            }
            len = length(&r, &m->repeated, position);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = length(&r, &m->matched, position);
            state = state < 7 ? 7 : 10;
            unsigned slot = tree(&r, m->slot[len < 4 ? len : 3], 6);
            uint32_t distance = slot;
            if (slot >= 4) {
                unsigned direct = (slot >> 1) - 1;
                distance = (uint32_t)(2 | (slot & 1)) << direct;
                if (slot < 14) {
                    distance += reverse_tree(&r, m->special + distance - slot, direct);
                } else {
                    uint32_t middle = 0;
                    for (unsigned i = 0; i < direct - 4; ++i) {
                        normalize(&r);
                        r.range >>= 1;
                        unsigned b = r.code >= r.range;
                        if (b)
                            r.code -= r.range;
                        middle = (middle << 1) | b;
                    }
                    distance += middle << 4;
                    distance += reverse_tree(&r, m->align, 4);
                }
            }
            if (distance == UINT32_MAX) /* end marker */
                break;
            rep0 = (size_t)distance + 1;
        }
        len += 2;
        ok = rep0 <= at && len <= size - at;
        for (; ok && len; --len, ++at)
            out[at] = out[at - rep0];
    }
    ok = ok && at == size && !r.overrun;
    free(m);
    return ok;
}
