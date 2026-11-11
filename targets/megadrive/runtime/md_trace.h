/* Optional tracing hooks for host tools (deepeel).  Compiled in only with
 * -DMD_TRACE; without it every call site below folds to nothing, so the
 * emulator and the generated code are unchanged.  Reads/writes are the
 * data-side bus accesses (instruction fetches are silenced by `quiet`). */
#ifndef M0BOY_MD_TRACE_H
#define M0BOY_MD_TRACE_H
#include <stdint.h>
#include "md_full.h"

typedef struct {
    void *user;
    void (*step)(void *user, MdFull *m, uint32_t pc, uint16_t op);
    void (*end)(void *user, MdFull *m, uint32_t pc, uint32_t next_pc); /* after execute */
    void (*rd)(void *user, MdFull *m, uint32_t addr, unsigned size);
    void (*wr)(void *user, MdFull *m, uint32_t addr, unsigned size, uint32_t value);
    uint32_t fetched; /* extension bytes fetched by the current instruction */
    int quiet;   /* >0: instruction fetch in progress, do not report reads */
} MdTrace;
/* deepeel runs several machines in parallel: it builds with -DMD_THREAD_LOCAL=__thread */
#ifndef MD_THREAD_LOCAL
#define MD_THREAD_LOCAL
#endif
extern MD_THREAD_LOCAL MdTrace md_trace;
#endif
