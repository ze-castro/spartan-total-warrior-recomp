/* Library call dispatch. Entry points with a handler (`lib_<Name>`) run it; everything else is a
 * logging stand-in that returns 0 and pops the callee's argument bytes, which records which library
 * functions the game uses, in order. */
#include <stdio.h>
#include <stdlib.h>
#include "lib.h"
#include "gametables.h"
#include "kernel.h"

int lib_dispatch(CPU *c, uint32_t t) {
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_TRACE") != NULL || getenv("STW_LIBTRACE") != NULL;
    for (const LibStub *s = stw_tables->libstubs; s->addr; s++) {
        if (s->addr != t) continue;
        uint32_t args[8];
        for (int i = 0; i < 8; i++) args[i] = LD32(c->esp + 4 + 4 * i);
        if (trace)
            fprintf(stderr, "[lib] %s %s@%x(%#x, %#x, %#x, %#x) ecx=%#x from %#x%s\n", s->section, s->name, t,
                    args[0], args[1], args[2], args[3], c->ecx, LD32(c->esp), s->fn ? "" : "  [stub]");
        if (!s->fn) {                                  /* never let an unimplemented library call pass silently */
            static uint32_t seen[256]; static int nseen;
            int dup = 0;
            for (int i = 0; i < nseen; i++) dup |= seen[i] == t;
            if (!dup && nseen < 256) {
                seen[nseen++] = t;
                fprintf(stderr, "stw: UNIMPLEMENTED library call %s %s (%#x) from %#x, returning 0\n", s->section, s->name, t, LD32(c->esp));
            }
        }
        c->eax = s->fn ? s->fn(c, args) : 0;
        c->esp += 4 + s->pop;
        return 1;
    }
    return 0;
}
