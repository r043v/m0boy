/* Trace hooks for an external observer (deepeel's explorer): -DNES_TRACE,
 * off by default so the interpreter's hot loop keeps its shape, as MD_TRACE
 * does for the 68k core.  A hook sees only what it registers for; the
 * observer sets `stop` to end the current run slice at the next
 * instruction boundary (the cycles actually consumed stay in cpu->cycles). */
#ifndef M0BOY_NES_TRACE_H
#define M0BOY_NES_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NesTrace {
    void *user;
    void (*step)(void *user, unsigned pc, unsigned op); /* before the
                        * instruction executes (opcode fetched, not counted) */
    void (*rd)(void *user, unsigned addr, unsigned v); /* data reads: operands
                        * included, the opcode fetch at pc excluded, stack excluded;
                        * v is the byte the read returned (post-read, value-exact) */
    void (*wr)(void *user, unsigned addr, unsigned v); /* data writes,
                        * stack excluded */
    void (*interrupt)(void *user, unsigned vector); /* an NMI ($FFFA), IRQ
                        * or BRK ($FFFE) sequence starts */
    int stop; /* the observer sets it; the core honours it at the next
               * instruction boundary */
} NesTrace;

extern NesTrace nes_trace;

#ifdef __cplusplus
}
#endif

#endif
