/* Replacements for the static XDK libraries (D3D, DSOUND, XGRPH, XPP, XMV, D3DX). */
#ifndef STW_LIB_H
#define STW_LIB_H
#include "rt.h"

/* args points at the stack arguments (after the return address); ecx/edx are in the CPU. The handler's
 * return value goes to eax and `pop` bytes of arguments are removed by the caller-side stub. */
typedef uint32_t (*LibHandler)(CPU *c, const uint32_t *args);

typedef struct { uint32_t addr; const char *section; uint32_t pop; const char *name; LibHandler fn; } LibStub;
extern const LibStub LIBSTUBS[];

/* Handle a call to a library entry point; returns 0 if t is not a known library address. */
int lib_dispatch(CPU *c, uint32_t t);
#endif
