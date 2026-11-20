#define STW_RUNTIME 1
#include <dlfcn.h>
#include <stdio.h>
#include "gametables.h"

const StwGameTables *stw_tables;
void (*stw_call_ind)(CPU *, uint32_t);

/* Loads the translated game code. The library's undefined symbols (G, mmio_write, k_*, lib_*, ...) are resolved against this program, which is
 * linked with -export_dynamic. */
int stw_load_game_lib(const char *path, char *err, size_t errlen) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(err, errlen, "%s", dlerror()); return -1; }
    const StwGameTables *t = dlsym(h, "stw_game_tables");
    int (*nf)(void) = (int (*)(void))dlsym(h, "stw_game_nfuncs_fn");
    if (!t) { snprintf(err, errlen, "%s has no stw_game_tables", path); return -1; }
    if (t->abi != STW_TABLES_ABI) { snprintf(err, errlen, "%s was built for another runtime version (abi %u, expected %u)", path, t->abi, STW_TABLES_ABI); return -1; }
    static StwGameTables copy;
    copy = *t;
    if (nf) copy.nfuncs = nf();
    stw_tables = &copy;
    stw_call_ind = t->call_ind;
    return 0;
}

#ifdef STW_STATIC
extern const StwGameTables stw_game_tables;
extern const int NFUNCS;
void stw_use_static_tables(void) {
    static StwGameTables copy;
    copy = stw_game_tables;
    copy.nfuncs = NFUNCS;
    stw_tables = &copy;
    stw_call_ind = copy.call_ind;
}
#else
void stw_use_static_tables(void) {}
#endif
