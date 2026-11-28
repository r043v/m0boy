#ifndef M0BOY_ARM920T_H
#define M0BOY_ARM920T_H

#include "s3c2400_bus.h"

typedef enum {
    ARM920T_INSTRUCTION, ARM920T_EXCEPTION, ARM920T_WAITING
} Arm920tStep;

typedef struct {
    uint32_t pc, physical, opcode;
    bool thumb, executed;
} Arm920tTrace;

typedef struct {
    S3c2400AccessKind kind;
    uint32_t virtual_address, physical, value;
    uint8_t width;
    bool cache_hit, buffered;
} Arm920tAccess;

typedef struct {
    const S3c2400Bus *bus;
    void (*trace)(void *context, const Arm920tTrace *event);
    void *context;
    /* Called before advancing PC or applying instruction effects. */
    bool (*predicate)(void *context, const Arm920tTrace *event);
    /* Completed CPU accesses, including those served entirely in cache. */
    void (*access)(void *context, const Arm920tAccess *event);
    /* A handled software interrupt resumes with the next instruction. */
    bool (*software_interrupt)(void *context, uint32_t number, uint32_t pc);
    void *software_interrupt_context;
} Arm920tConfig;

typedef struct {
    uint32_t virtual_base, physical_base, offset_mask;
    uint8_t domain, permissions, attributes;
    bool preserved;
} Arm920tTlbEntry;

typedef struct {
    uint32_t tag, physical, words[8];
    uint8_t dirty;
    bool valid;
} Arm920tCacheLine;

typedef struct {
    Arm920tCacheLine lines[8][64];
    uint8_t base, victim[8];
} Arm920tCache;

typedef struct {
    uint32_t physical, value;
    uint8_t width;
    S3c2400AccessKind kind;
} Arm920tBufferedWrite;

/* A 1 KiB page whose cacheable reads were last allowed. Any TLB change
   advances the generation; register changes are compared directly. */
typedef struct {
    uint32_t page, physical, control, domains, generation;
    bool user;
} Arm920tReadPage;

typedef struct {
    Arm920tConfig config;
    uint32_t r[16], cpsr;
    uint32_t shared[5], fiq[5], bank[6][2], spsr[6];
    uint32_t control, ttbr, domains, dfsr, ifsr, far, pid;
    Arm920tTlbEntry tlb[2][64];
    unsigned victim[2], tlb_base[2];
    bool tlb_preserve[2];
    Arm920tCache cache[2];
    /* Direct mapped guesses of the matching TLB slot (by page) and cache way
       (by line address). Always checked against the entry itself. */
    uint8_t tlb_hint[2][256], way_hint[2][1024];
    Arm920tReadPage read_pages[2][64];
    uint32_t tlb_generation;
    /* Advances on anything a repeated instruction sequence could not redo
       unchanged: stores, device reads, fills, walks, exceptions, mode or
       coprocessor changes. */
    uint32_t effects;
    /* Loads from outside SDRAM, which the machine may report as repeatable. */
    uint32_t device_reads;
    uint32_t replacement_random;
    Arm920tBufferedWrite writes[16];
    unsigned write_head, write_count;
    bool irq, fiq_line, waiting;
} Arm920t;

void arm920t_reset(Arm920t *cpu, Arm920tConfig config, bool high_vectors);
void arm920t_set_status(Arm920t *cpu, uint32_t cpsr);
bool arm920t_data_byte(Arm920t *cpu, uint32_t address, bool load, uint8_t *byte);
Arm920tStep arm920t_step(Arm920t *cpu);

/* Whether the emulation may divide. It does where the host divides fast (x86);
   everywhere else, and always where there is no division instruction, it
   multiplies by inverses computed beforehand. */
#ifndef M0_GP32_DIVIDE
#if defined(__x86_64__) || defined(__i386__)
#define M0_GP32_DIVIDE 1
#else
#define M0_GP32_DIVIDE 0
#endif
#endif

/* Screen shots carrying the machine state and the coverage map of what the
   recompiled code lacks are development tools of the PC builds only. */
#ifndef M0_GP32_PC_TOOLS
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define M0_GP32_PC_TOOLS 1
#else
#define M0_GP32_PC_TOOLS 0
#endif
#endif

/* n / d. Without division, `inverse` is 2^32 / d rounded up: the product is then
   at most one over. */
static inline uint32_t arm920t_quotient(uint32_t n, uint32_t d, uint64_t inverse)
{
#if M0_GP32_DIVIDE
    (void)inverse;
    return n / d;
#else
    uint32_t q = (uint32_t)((n * inverse) >> 32);
    return q - ((uint64_t)q * d > n);
#endif
}

/* Accounting shared with the machine while recompiled code runs. */
typedef struct {
    uint64_t *steps;          /* instructions executed */
    uint64_t left;            /* instructions the caller still accepts */
    unsigned *budget;         /* clocks before the next peripheral event */
    unsigned *pending;        /* clocks elapsed and not yet applied */
    unsigned cost;            /* clocks per instruction */
    uint64_t *interpreted;    /* instructions left to the interpreter */
    uint32_t pc;              /* out: address of the last instruction */
    unsigned extra;           /* out: clocks past the budget (fast blocks) */
    uint32_t effects;         /* cpu->effects at the last short backward branch */
    uint64_t inverse;         /* 2^32 / cost, rounded up, where the host cannot divide */
} Arm920tAot;

/* Defined by the generated file of a recompiled build. Runs one or more
   instructions without observers. Every one but the last is fully accounted;
   the caller accounts the last exactly as after arm920t_step. */
Arm920tStep arm920t_aot_run(Arm920t *cpu, Arm920tAot *run);
#if M0_GP32_PC_TOOLS
/* Also defined there: every instruction the recompiled code does not hold is
   added to the coverage map at `path` (kept from one run to the next), written
   by the flush and when the program exits.  Call from the emulating thread. */
void arm920t_aot_found(const char *path);
void arm920t_aot_flush(void);
#endif

#endif
