/* Compiled together with the generated lifted.c / ktable.c / libstubs.c (into lifted.dylib, or into the program in the developer build). */
#include "gametables.h"
extern const KExport KEXPORTS[];
extern const LibStub LIBSTUBS[];
void call_ind(CPU *c, uint32_t target);
const StwGameTables stw_game_tables = { STW_TABLES_ABI, KEXPORTS, LIBSTUBS, 0, call_ind };
/* nfuncs is only informational; filled lazily so this file does not need the generated FUNCS table */
int stw_game_nfuncs_fn(void) { extern const int NFUNCS; return NFUNCS; }
