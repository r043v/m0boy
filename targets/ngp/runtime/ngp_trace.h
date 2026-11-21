/* The explorer's observation hooks.  deepeel compiles the runtime with
 * -DNGP_TRACE and provides these symbols (its machine wrapper); every other
 * build compiles them out and the machine runs exactly as before.  The
 * contract mirrors the MD/NES/PCE trace hooks: one call per instruction about
 * to run, one call per data byte the CPU moves, one call per micro-DMA unit. */
#ifndef M0_NGP_TRACE_H
#define M0_NGP_TRACE_H
#ifdef NGP_TRACE
#include <stdint.h>
/* one instruction about to run at `pc`, first opcode byte `op` */
extern void ngp_trace_step(uint32_t pc, uint8_t op);
/* a data byte the CPU read / wrote (opcode fetches and the micro-DMA are not
 * CPU accesses: they stay unobserved here) */
extern void ngp_trace_read(uint32_t addr, uint8_t v);
extern void ngp_trace_write(uint32_t addr, uint8_t v);
/* a micro-DMA unit moved from `addr` (`width` bytes: the source the hardware
 * read, the CPU never saw it) */
extern void ngp_trace_dma(uint32_t addr, unsigned width);
#define NGP_TRACE_HOOK(pc, op) ngp_trace_step((pc), (op))
#else
#define NGP_TRACE_HOOK(pc, op) ((void)0)
#endif
#endif
