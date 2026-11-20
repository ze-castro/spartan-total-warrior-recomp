/* NV2A vertex program -> Metal Shading Language.
 *
 * The microcode is 128-bit VLIW: each instruction can issue a "MAC" (vector) and an "ILU" (scalar) op at
 * once, with a shared input C, and an output multiplexer that decides which of the two results goes to
 * the output register. Field layout and semantics follow the NV2A documentation as implemented in xemu
 * (hw/xbox/nv2a/pgraph/glsl/vsh-prog.c, GPL-2.0-or-later); this is an independent C implementation that
 * emits MSL instead of GLSL.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"

typedef enum { FLD_ILU, FLD_MAC, FLD_CONST, FLD_V,
               FLD_A_NEG, FLD_A_SWZ_X, FLD_A_SWZ_Y, FLD_A_SWZ_Z, FLD_A_SWZ_W, FLD_A_R, FLD_A_MUX,
               FLD_B_NEG, FLD_B_SWZ_X, FLD_B_SWZ_Y, FLD_B_SWZ_Z, FLD_B_SWZ_W, FLD_B_R, FLD_B_MUX,
               FLD_C_NEG, FLD_C_SWZ_X, FLD_C_SWZ_Y, FLD_C_SWZ_Z, FLD_C_SWZ_W, FLD_C_R_HIGH, FLD_C_R_LOW, FLD_C_MUX,
               FLD_OUT_MAC_MASK, FLD_OUT_R, FLD_OUT_ILU_MASK, FLD_OUT_O_MASK, FLD_OUT_ORB, FLD_OUT_ADDRESS,
               FLD_OUT_MUX, FLD_A0X, FLD_FINAL } Fld;

static const struct { uint8_t dword, bit, len; } MAP[] = {
    [FLD_ILU] = {1, 25, 3}, [FLD_MAC] = {1, 21, 4}, [FLD_CONST] = {1, 13, 8}, [FLD_V] = {1, 9, 4},
    [FLD_A_NEG] = {1, 8, 1}, [FLD_A_SWZ_X] = {1, 6, 2}, [FLD_A_SWZ_Y] = {1, 4, 2}, [FLD_A_SWZ_Z] = {1, 2, 2},
    [FLD_A_SWZ_W] = {1, 0, 2}, [FLD_A_R] = {2, 28, 4}, [FLD_A_MUX] = {2, 26, 2},
    [FLD_B_NEG] = {2, 25, 1}, [FLD_B_SWZ_X] = {2, 23, 2}, [FLD_B_SWZ_Y] = {2, 21, 2}, [FLD_B_SWZ_Z] = {2, 19, 2},
    [FLD_B_SWZ_W] = {2, 17, 2}, [FLD_B_R] = {2, 13, 4}, [FLD_B_MUX] = {2, 11, 2},
    [FLD_C_NEG] = {2, 10, 1}, [FLD_C_SWZ_X] = {2, 8, 2}, [FLD_C_SWZ_Y] = {2, 6, 2}, [FLD_C_SWZ_Z] = {2, 4, 2},
    [FLD_C_SWZ_W] = {2, 2, 2}, [FLD_C_R_HIGH] = {2, 0, 2}, [FLD_C_R_LOW] = {3, 30, 2}, [FLD_C_MUX] = {3, 28, 2},
    [FLD_OUT_MAC_MASK] = {3, 24, 4}, [FLD_OUT_R] = {3, 20, 4}, [FLD_OUT_ILU_MASK] = {3, 16, 4},
    [FLD_OUT_O_MASK] = {3, 12, 4}, [FLD_OUT_ORB] = {3, 11, 1}, [FLD_OUT_ADDRESS] = {3, 3, 8},
    [FLD_OUT_MUX] = {3, 2, 1}, [FLD_A0X] = {3, 1, 1}, [FLD_FINAL] = {3, 0, 1},
};
static unsigned F(const uint32_t *t, Fld f) { return (t[MAP[f].dword] >> MAP[f].bit) & ((1u << MAP[f].len) - 1); }

enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { OMUX_MAC = 0, OMUX_ILU = 1 };
enum { PARAM_R = 1, PARAM_V = 2, PARAM_C = 3 };

static const char *mac_name[] = { "NOP", "MOV", "MUL", "ADD", "MAD", "DP3", "DPH", "DP4", "DST", "MIN", "MAX", "SLT", "SGE", "ARL" };
static const char *ilu_name[] = { "NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT" };
static const int mac_params[][3] = { {0,0,0}, {1,0,0}, {1,1,0}, {1,0,1}, {1,1,1}, {1,1,0}, {1,1,0}, {1,1,0}, {1,1,0}, {1,1,0}, {1,1,0}, {1,1,0}, {1,1,0}, {1,0,0} };
static const int ilu_scalar[] = { 0, 0, 1, 1, 1, 1, 1, 0 };
static const char *mask_str[] = { ",", ",w", ",z", ",zw", ",y", ",yw", ",yz", ",yzw", ",x", ",xw", ",xz", ",xzw", ",xy", ",xyw", ",xyz", ",xyzw" };
static const char *fog_mask_str[] = { ",", ",x", ",x", ",xy", ",x", ",xy", ",xy", ",xyz", ",x", ",xy", ",xy", ",xyz", ",xy", ",xyz", ",xyz", ",xyzw" };
static const char *out_reg[] = { "oPos", "???", "???", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "???", "???", "A0.x" };

/* ---- tiny growable string ---- */
typedef struct { char *p; size_t n, cap; } Str;
static void sadd(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int k = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (k < 0) { va_end(ap2); return; }
    if (s->n + k + 1 > s->cap) { s->cap = (s->n + k + 1) * 2 + 256; s->p = realloc(s->p, s->cap); }
    vsnprintf(s->p + s->n, k + 1, fmt, ap2);
    va_end(ap2);
    s->n += k;
}

static int c_reg(int r) { return ((((r >> 5) & 7) - 3) * 32) + (r & 31) + 96; }    /* -96..95 -> 0..191 */

static void swizzle(Str *o, const uint32_t *t, Fld base) {
    static const char *xyzw = "xyzw";
    int x, y, z, w;
    if (base == FLD_C_SWZ_X && ilu_scalar[F(t, FLD_ILU)]) x = y = z = w = F(t, base);
    else { x = F(t, base); y = F(t, base + 1); z = F(t, base + 2); w = F(t, base + 3); }
    if (x == 0 && y == 1 && z == 2 && w == 3) return;
    sadd(o, ".");
    sadd(o, "%c", xyzw[x]);
    if (x == y && y == z && z == w) return;
    sadd(o, "%c", xyzw[y]);
    if (y == z && z == w) return;
    sadd(o, "%c", xyzw[z]);
    if (z == w) return;
    sadd(o, "%c", xyzw[w]);
}

static void input(Str *o, const uint32_t *t, int param, Fld neg, int reg) {
    if (F(t, neg)) sadd(o, "-");
    if (param == PARAM_R) sadd(o, "R%d", reg);
    else if (param == PARAM_V) sadd(o, "v%d", F(t, FLD_V));
    else if (param == PARAM_C) {
        if (F(t, FLD_A0X)) sadd(o, "c[A0+%d]", c_reg(F(t, FLD_CONST)));
        else sadd(o, "c[%d]", c_reg(F(t, FLD_CONST)));
    } else sadd(o, "float4(0)");
    swizzle(o, t, neg + 1);
}

/* One half (MAC or ILU) of an instruction. `suffix` collects deferred writes for paired ops. */
static void opcode(Str *o, Str *suffix, const uint32_t *t, int out_mux, unsigned mask, const char *name, const char *inputs) {
    int reg = F(t, FLD_OUT_R);
    int use_temp = 0, use_temp_opos = 0;
    const char *wmask = NULL;
    if (out_mux == OMUX_MAC && F(t, FLD_ILU) != ILU_NOP) {
        use_temp = 1;
        if (reg == 1) mask = 0;                     /* paired MAC writes to R1 are ignored */
    } else if (out_mux == OMUX_ILU && F(t, FLD_MAC) != MAC_NOP) reg = 1;     /* paired ILU can only write R1 */

    if ((int)F(t, FLD_OUT_MUX) == out_mux && F(t, FLD_OUT_O_MASK) != 0) {
        sadd(o, "  %s(", name);
        if (F(t, FLD_OUT_ORB) == 0) {
            sadd(o, "c%d", c_reg(F(t, FLD_OUT_ADDRESS)));          /* writable constants: not emulated */
        } else {
            int r = F(t, FLD_OUT_ADDRESS) & 0xF;
            if (r == 0 && (use_temp || mask)) { use_temp_opos = 1; sadd(o, "_temp_r12"); }
            else sadd(o, "%s", out_reg[r]);
            wmask = (r == 5) ? fog_mask_str[F(t, FLD_OUT_O_MASK)] : mask_str[F(t, FLD_OUT_O_MASK)];
        }
        if (!wmask) wmask = mask_str[F(t, FLD_OUT_O_MASK)];
        sadd(o, "%s%s);\n", wmask, inputs);
    }
    if (use_temp) {
        if (!strcmp(name, "ARL")) {
            sadd(o, "  ARL(_temp_addr%s);\n", inputs);
            sadd(suffix, "  A0 = _temp_addr;\n");
        } else if (mask > 0) {
            sadd(o, "  %s(_temp_vec%s%s);\n", name, mask_str[mask], inputs);
            const char *m = &mask_str[mask][1];
            if (m[0]) sadd(suffix, "  R%d.%s = _temp_vec.%s;\n", reg, m, m);
            else sadd(suffix, "  R%d = _temp_vec;\n", reg);
        }
    } else {
        if (!strcmp(name, "ARL")) sadd(o, "  ARL(A0%s);\n", inputs);
        else if (mask > 0) sadd(o, "  %s(R%d%s%s);\n", name, reg, mask_str[mask], inputs);
    }
    if (use_temp_opos) {
        const char *m = wmask + 1;
        sadd(suffix, "  oPos.%s = _temp_r12.%s;\n", m, m);
    }
}

static void token(Str *body, const uint32_t *t) {
    int mac = F(t, FLD_MAC), ilu = F(t, FLD_ILU);
    if (mac == MAC_NOP && ilu == ILU_NOP) return;
    Str cin = {0}, suffix = {0};
    input(&cin, t, F(t, FLD_C_MUX), FLD_C_NEG, (F(t, FLD_C_R_HIGH) << 2) | F(t, FLD_C_R_LOW));
    if (mac != MAC_NOP) {
        Str in = {0};
        if (mac_params[mac][0]) { sadd(&in, ", "); input(&in, t, F(t, FLD_A_MUX), FLD_A_NEG, F(t, FLD_A_R)); }
        if (mac_params[mac][1]) { sadd(&in, ", "); input(&in, t, F(t, FLD_B_MUX), FLD_B_NEG, F(t, FLD_B_R)); }
        if (mac_params[mac][2]) sadd(&in, ", %s", cin.p ? cin.p : "");
        opcode(body, &suffix, t, OMUX_MAC, F(t, FLD_OUT_MAC_MASK), mac_name[mac], in.p ? in.p : "");
        free(in.p);
    }
    if (ilu != ILU_NOP) {
        Str in = {0};
        sadd(&in, ", %s", cin.p ? cin.p : "");
        opcode(body, &suffix, t, OMUX_ILU, F(t, FLD_OUT_ILU_MASK), ilu_name[ilu], in.p ? in.p : "");
        free(in.p);
    }
    free(cin.p);
    if (suffix.p) { sadd(body, "%s", suffix.p); free(suffix.p); }
}

/* Helper functions and ops (Metal translation of the NV2A instruction semantics). */
static const char *PRELUDE =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "static float4 NaNToOne(float4 v) { return select(v, float4(1.0), isnan(v)); }\n"
    "static float clampAwayZeroInf(float t) {\n"
    "  float a = clamp(fabs(t), 5.42101e-20, 1.884467e19);\n"
    "  return (t < 0.0) ? -a : a; }\n"
    "struct VertexInfo { uint mask; uint stride; uint pad0; uint pad1; float4 cur[16]; };\n"
    "static float4 _in(float v) { return float4(v); }\n"
    "static float4 _in(float2 v) { return v.xyyy; }\n"
    "static float4 _in(float3 v) { return v.xyzz; }\n"
    "static float4 _in(float4 v) { return v; }\n"
    "#define MOV(dest, mask, src) dest.mask = _MOV(_in(src)).mask\n"
    "static float4 _MOV(float4 s) { return s; }\n"
    "#define MUL(dest, mask, a, b) dest.mask = _MUL(_in(a), _in(b)).mask\n"
    "static float4 _MUL(float4 a, float4 b) {\n"
    "  float4 z = sign(NaNToOne(a)) * sign(NaNToOne(b)); float4 r = a * b;\n"
    "  if (z.x == 0.0) r.x = 0.0; if (z.y == 0.0) r.y = 0.0; if (z.z == 0.0) r.z = 0.0; if (z.w == 0.0) r.w = 0.0;\n"
    "  return r; }\n"
    "#define ADD(dest, mask, a, b) dest.mask = (_in(a) + _in(b)).mask\n"
    "#define MAD(dest, mask, a, b, c) dest.mask = (_MUL(_in(a), _in(b)) + _in(c)).mask\n"
    "#define DP3(dest, mask, a, b) dest.mask = float4(dot(_in(a).xyz, _in(b).xyz)).mask\n"
    "#define DPH(dest, mask, a, b) dest.mask = float4(dot(float4(_in(a).xyz, 1.0), _in(b))).mask\n"
    "#define DP4(dest, mask, a, b) dest.mask = float4(dot(_in(a), _in(b))).mask\n"
    "#define DST(dest, mask, a, b) dest.mask = float4(1.0, _in(a).y * _in(b).y, _in(a).z, _in(b).w).mask\n"
    "#define MIN(dest, mask, a, b) dest.mask = min(_in(a), _in(b)).mask\n"
    "#define MAX(dest, mask, a, b) dest.mask = max(_in(a), _in(b)).mask\n"
    "#define SLT(dest, mask, a, b) dest.mask = float4(_in(a) < _in(b)).mask\n"
    "#define SGE(dest, mask, a, b) dest.mask = float4(_in(a) >= _in(b)).mask\n"
    "#define ARL(dest, src) dest = int(floor(_in(src).x + 0.001))\n"
    "#define RCP(dest, mask, s) dest.mask = float4(1.0 / _in(s).x).mask\n"
    "#define RCC(dest, mask, s) dest.mask = float4(clampAwayZeroInf(1.0 / _in(s).x)).mask\n"
    "#define RSQ(dest, mask, s) dest.mask = _RSQ(_in(s).x).mask\n"
    "static float4 _RSQ(float s) { if (s == 0.0) return float4(INFINITY); if (isinf(s)) return float4(0.0); return float4(rsqrt(fabs(s))); }\n"
    "#define EXP(dest, mask, s) dest.mask = _EXP(_in(s).x).mask\n"
    "static float4 _EXP(float s) { return float4(exp2(floor(s)), s - floor(s), exp2(s), 1.0); }\n"
    "#define LOG(dest, mask, s) dest.mask = _LOG(_in(s).x).mask\n"
    "static float4 _LOG(float s) { float t = fabs(s); if (t == 0.0) return float4(-INFINITY, 1.0, -INFINITY, 1.0);\n"
    "  return float4(floor(log2(t)), t / exp2(floor(log2(t))), log2(t), 1.0); }\n"
    "#define LIT(dest, mask, s) dest.mask = _LIT(_in(s)).mask\n"
    "static float4 _LIT(float4 s) { s.w = clamp(s.w, -(128.0 - 1.0/256.0), 128.0 - 1.0/256.0); s.x = max(s.x, 0.0); s.y = max(s.y, 0.0);\n"
    "  return float4(1.0, s.x, (s.x > 0.0) ? exp2(s.w * log2(s.y)) : 0.0, 1.0); }\n"
    "struct VSOut { float4 pos [[position]]; float4 d0; float4 d1; float4 b0; float4 b1; float4 fog; float psize [[point_size]];\n"
    "               float4 t0; float4 t1; float4 t2; float4 t3; };\n"
    "struct SurfaceInfo { float2 size; float2 clip_range; float2 aa; float xsq; float pad; };\n";

/* Returns a malloc'd MSL source containing `VSOut vs_main(...)` for the program starting at `prog[start]`. */
char *vsh_to_msl(const uint32_t *prog, int start, int nslots) {
    Str s = {0}, body = {0};
    sadd(&s, "%s", PRELUDE);
    int has_final = 0;
    for (int i = start; i < nslots; i++) {
        const uint32_t *t = &prog[i * 4];
        sadd(&body, "  /* %d: %08x %08x %08x */\n", i, t[1], t[2], t[3]);
        token(&body, t);
        if (F(t, FLD_FINAL)) { has_final = 1; break; }
    }
    (void)has_final;
    /* NV2A window coordinates have pixel centres on integers; Metal's are at +0.5. Without this shift quads written for the
     * hardware ([-0.5, w-0.5) post-process passes, [0, 63) letterbox bars) lose their last row/column and sample off-phase. */
    const float pixoff = 0.5f;
    sadd(&s,
        "vertex VSOut vs_main(uint vid [[vertex_id]], const device float4 *verts [[buffer(0)]],\n"
        "                     constant float4 *c [[buffer(1)]], constant SurfaceInfo &surf [[buffer(2)]],\n"
        "                     constant VertexInfo &vk [[buffer(3)]]) {\n"
        "  const device float4 *vp = verts + vid * vk.stride;\n");
    for (int i = 0; i < 16; i++)
        sadd(&s, "  float4 v%d = (vk.mask & %uu) ? vp[popcount(vk.mask & %uu)] : vk.cur[%d];\n", i, 1u << i, (1u << i) - 1, i);
    sadd(&s,
        "  int A0 = 0, _temp_addr = 0;\n"
        "  float4 R0=0,R1=0,R2=0,R3=0,R4=0,R5=0,R6=0,R7=0,R8=0,R9=0,R10=0,R11=0,_temp_vec=0,_temp_r12=0;\n"
        "  float4 oPos=0, oD0=float4(1), oD1=float4(0), oB0=float4(1), oB1=float4(0), oFog=0, oPts=float4(1);\n"
        "  float4 oT0=0, oT1=0, oT2=0, oT3=0;\n"
        "#define R12 oPos\n"
        "%s"
        "#undef R12\n"
        "  oPos.w = clampAwayZeroInf(oPos.w);\n"
        "  oPos.xy *= surf.aa;\n"                      /* antialiased surfaces: programs emit logical window coordinates; the buffer holds aa.x * aa.y samples per pixel */
        "  oPos.xy = float2((2.0 * (oPos.x + %g) - surf.size.x) / surf.size.x, -(2.0 * (oPos.y + %g) - surf.size.y) / surf.size.y);\n"
        "  if (surf.xsq != 1.0 && fabs(oPos.w - 1.0) < 1e-3) oPos.x *= surf.xsq;\n"   /* widescreen: orthographic (w = 1) elements are squeezed about the centre so the stretched picture keeps their proportions */
        "  oPos.z = oPos.z / surf.clip_range.y;\n"
        "  oPos.xyz *= oPos.w;\n"
        "  VSOut o; o.pos = oPos; o.d0 = oD0; o.d1 = oD1; o.b0 = oB0; o.b1 = oB1; o.fog = oFog; o.psize = oPts.x;\n"
        "  o.t0 = oT0; o.t1 = oT1; o.t2 = oT2; o.t3 = oT3;\n"
        "  return o; }\n", body.p ? body.p : "", pixoff, pixoff);
    free(body.p);
    return s.p;
}
