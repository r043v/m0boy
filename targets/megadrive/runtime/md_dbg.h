#ifndef MD_DBG_H
#define MD_DBG_H
/* Frame selector for opt-in diagnostics. Spec syntax (env var): "N", "A-B", "all", or a comma list, e.g. "33100-33110,33920,37000-37005".
 * Unset or empty: disabled (returns 0). One MdFrameSel per call site (static); the spec is parsed once. */
#include <stdlib.h>
#include <string.h>
typedef struct { int init; unsigned n; unsigned long lo[32], hi[32]; } MdFrameSel;
static inline int md_frame_sel(MdFrameSel *s, const char *env, unsigned long f)
{
    if (!s->init) {
        s->init = 1; s->n = 0;
        const char *p = env ? getenv(env) : NULL;
        if (p && !strcmp(p, "all")) { s->lo[0] = 0; s->hi[0] = ~0ul; s->n = 1; }
        while (p && *p && s->n < 32) {
            char *e; unsigned long a = strtoul(p, &e, 10), b = a;
            if (e == p) break;
            if (*e == '-') { p = e + 1; b = strtoul(p, &e, 10); }
            s->lo[s->n] = a; s->hi[s->n++] = b; p = (*e == ',') ? e + 1 : e;
            if (*e != ',') break;
        }
    }
    for (unsigned i = 0; i < s->n; ++i) if (f >= s->lo[i] && f <= s->hi[i]) return 1;
    return 0;
}
#endif
