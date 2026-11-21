/* TLCS-900H -> C for the NGP runtime (targets/ngp/runtime, ngp_gen.c). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace m0boy::ngp {

struct EmitOptions {
    size_t max_instructions = 0; /* 0: every instruction the walk reaches */
};

struct EmitStats {
    size_t compiled = 0;  /* instructions translated (one entry each) */
    size_t regions = 0;   /* == compiled: one function per instruction */
    size_t entries = 0;   /* == compiled: every translated pc is an entry */
    size_t reached = 0;   /* instructions the walk decoded */
};

/* the generated ngp_gen.c: one function per instruction (the fetch, the
 * addressing and the primary dispatch folded), the dispatcher tables and
 * the hash lookup */
std::string emit_code(const std::vector<uint8_t>& rom, const EmitOptions& options, EmitStats& stats);

} // namespace m0boy::ngp
