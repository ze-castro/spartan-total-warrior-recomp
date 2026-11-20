/* The parts of the runtime that depend on the game's executable: the lifted-function dispatcher and the tables generated from default.xbe
 * (kernel imports, library stubs). They are built from the user's own disc, either linked into the program (developer build, make) or
 * compiled into <data dir>/lifted/.../lifted.dylib on first run (the app), and reach the runtime only through this struct. */
#ifndef STW_GAMETABLES_H
#define STW_GAMETABLES_H
#include "rt.h"
#include "kernel.h"
#include "lib.h"
#define STW_TABLES_ABI 1
typedef struct StwGameTables {
    uint32_t abi;
    const KExport *kexports;                  /* build/ktable.c */
    const LibStub *libstubs;                  /* build/libstubs.c */
    int nfuncs;                               /* number of lifted functions */
    void (*call_ind)(CPU *c, uint32_t target);   /* binary search over the lifted functions, falls back to rt_bad_indirect */
} StwGameTables;
extern const StwGameTables *stw_tables;       /* set by stw_load_game_lib() or stw_use_static_tables() */
int stw_load_game_lib(const char *path, char *err, size_t errlen);
void stw_use_static_tables(void);             /* developer build: the tables are linked in */
#endif
