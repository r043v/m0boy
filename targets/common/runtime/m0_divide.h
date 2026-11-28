/* Which divisions the target computes in hardware.  Where it cannot, code run
   during emulation takes a variant without them; both macros can be forced from
   the command line, to test either variant on any host. */
#ifndef M0_DIVIDE_H
#define M0_DIVIDE_H

#include <stdint.h>

#if defined(__XTENSA__)
#include <xtensa/config/core-isa.h>
#endif

#ifndef M0_DIVIDE32
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86) || \
    defined(__aarch64__) || defined(__ARM_FEATURE_IDIV) || defined(__riscv_div) || \
    (defined(__XTENSA__) && XCHAL_HAVE_DIV32)
#define M0_DIVIDE32 1
#else
#define M0_DIVIDE32 0
#endif
#endif

#ifndef M0_DIVIDE64
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86) || \
    defined(__aarch64__)
#define M0_DIVIDE64 1
#else
#define M0_DIVIDE64 0
#endif
#endif

/* floor(n * 256 / d) for n < d < 2^24. */
static inline uint32_t m0_ratio256(uint32_t n, uint32_t d) {
#if M0_DIVIDE32
    return n * 256u / d;
#else
    uint32_t quotient = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        n <<= 1;
        quotient <<= 1;
        if (n >= d) { n -= d; quotient |= 1u; }
    }
    return quotient;
#endif
}

/* n / d and n % d for d < 2^16, in 32-bit divisions where 64-bit ones are
   software routines. */
static inline uint64_t m0_div64_16(uint64_t n, uint32_t d, uint32_t *remainder) {
#if M0_DIVIDE64
    *remainder = (uint32_t)(n % d);
    return n / d;
#else
    uint64_t quotient = 0;
    uint32_t r = 0;
    for (int shift = 48; shift >= 0; shift -= 16) {
        const uint32_t part = r << 16 | (uint32_t)(n >> shift & 0xffffu), q = part / d;
        r = part - q * d;
        quotient |= (uint64_t)q << shift;
    }
    *remainder = r;
    return quotient;
#endif
}

/* n / d and the remainder, for the small quotients of a target without a
   divider: one step per bit of the quotient. */
static inline uint32_t m0_udivmod(uint32_t n, uint32_t d, uint32_t *remainder) {
#if M0_DIVIDE32
    *remainder = n % d;
    return n / d;
#else
    uint32_t quotient = 0, bit = 1;
    while (d < n && !(d & 0x80000000u)) { d <<= 1; bit <<= 1; }
    for (; bit; bit >>= 1, d >>= 1)
        if (n >= d) { n -= d; quotient |= bit; }
    *remainder = n;
    return quotient;
#endif
}

#endif
