/* NV2A register combiners -> Metal Shading Language fragment function.
 *
 * Up to 8 general combiner stages, each with an RGB and an alpha half computing A*B (or A.B) and C*D (or C.D)
 * and their sum (or a mux on r0.a), with input mappings and output scale/bias; then the "final combiner"
 * (d + mix(c, b, a), alpha = g). Texture stages feed t0..t3 according to the per-stage texture mode.
 * Semantics follow the NV2A documentation as implemented in xemu (psh.c, GPL-2.0-or-later); independent
 * implementation emitting MSL.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"

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

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5, R_T0 = 8, R_T1 = 9, R_T2 = 10, R_T3 = 11,
       R_R0 = 12, R_R1 = 13, R_V1R0_SUM = 14, R_EF_PROD = 15 };
enum { UNIQUE_C0 = 0x10, UNIQUE_C1 = 0x100, MUX_MSB = 1 };

typedef struct { int reg, chan, mod; } Input;
typedef struct { Input a, b, c, d; } Inputs;
typedef struct { int ab, cd, muxsum, flags, ab_dot, cd_dot, mux, mapping, ab_b2a, cd_b2a; } Output;

typedef struct {
    const PshState *s;
    int cur_stage;                           /* 0..7, or 8 for the final combiner */
    int flags;
    int final_clamp_sum, final_inv_v1, final_inv_r0;
    char var_e[96], var_f[96];
} Ctx;

static Input parse_input(uint32_t v) { Input i = { v & 0xF, v & 0x10, v & 0xE0 }; return i; }
static Inputs parse_inputs(uint32_t v) {
    Inputs r = { parse_input(v >> 24), parse_input(v >> 16), parse_input(v >> 8), parse_input(v) };
    return r;
}
static Output parse_output(uint32_t v) {
    Output o;
    o.cd = v & 0xF; o.ab = (v >> 4) & 0xF; o.muxsum = (v >> 8) & 0xF;
    int f = v >> 12;
    o.flags = f; o.cd_dot = f & 1; o.ab_dot = f & 2; o.mux = f & 4; o.mapping = f & 0x38;
    o.ab_b2a = f & 0x80; o.cd_b2a = f & 0x40;
    return o;
}

/* Source expression for a register read (is_dest=0) or the register name to assign (is_dest=1; "" = discard). */
static void reg_name(Ctx *c, int reg, int is_dest, char *out, size_t n) {
    switch (reg) {
    case R_ZERO: snprintf(out, n, "%s", is_dest ? "" : "float4(0.0)"); break;
    case R_C0: snprintf(out, n, "k.c0[%d]", (c->flags & UNIQUE_C0) || c->cur_stage == 8 ? c->cur_stage : 0); break;
    case R_C1: snprintf(out, n, "k.c1[%d]", (c->flags & UNIQUE_C1) || c->cur_stage == 8 ? c->cur_stage : 0); break;
    case R_FOG: snprintf(out, n, "pFog"); break;
    case R_V0: snprintf(out, n, "v0"); break;
    case R_V1: snprintf(out, n, "v1"); break;
    case R_T0: case R_T1: case R_T2: case R_T3: snprintf(out, n, "t%d", reg - R_T0); break;
    case R_R0: snprintf(out, n, "r0"); break;
    case R_R1: snprintf(out, n, "r1"); break;
    case R_V1R0_SUM:
        snprintf(out, n, "float4(%s%sv1%s.rgb + %sr0%s.rgb%s, 0.0)", c->final_clamp_sum ? "clamp(" : "",
                 c->final_inv_v1 ? "(1.0 - " : "", c->final_inv_v1 ? ")" : "",
                 c->final_inv_r0 ? "(1.0 - " : "", c->final_inv_r0 ? ")" : "", c->final_clamp_sum ? ", 0.0, 1.0)" : "");
        break;
    case R_EF_PROD: snprintf(out, n, "float4(%s * %s, 0.0)", c->var_e, c->var_f); break;
    default: snprintf(out, n, "float4(0.0)"); break;
    }
}

static void input_expr(Ctx *c, Input in, int is_alpha, char *out, size_t n) {
    char reg[160], sel[200];
    reg_name(c, in.reg, 0, reg, sizeof reg);
    if (!is_alpha) snprintf(sel, sizeof sel, "(%s)%s", reg, in.chan == 0x10 ? ".aaa" : ".rgb");
    else snprintf(sel, sizeof sel, "(%s)%s", reg, in.chan == 0x10 ? ".a" : ".b");
    switch (in.mod) {
    case 0x00: snprintf(out, n, "max(%s, 0.0)", sel); break;
    case 0x20: snprintf(out, n, "(1.0 - clamp(%s, 0.0, 1.0))", sel); break;
    case 0x40: snprintf(out, n, "(2.0 * max(%s, 0.0) - 1.0)", sel); break;
    case 0x60: snprintf(out, n, "(-2.0 * max(%s, 0.0) + 1.0)", sel); break;
    case 0x80: snprintf(out, n, "(max(%s, 0.0) - 0.5)", sel); break;
    case 0xA0: snprintf(out, n, "(-max(%s, 0.0) + 0.5)", sel); break;
    case 0xC0: snprintf(out, n, "%s", sel); break;
    default:   snprintf(out, n, "(-%s)", sel); break;
    }
}

static void map_output(int mapping, const char *x, char *out, size_t n) {
    switch (mapping) {
    case 0x08: snprintf(out, n, "(%s - 0.5)", x); break;
    case 0x10: snprintf(out, n, "(%s * 2.0)", x); break;
    case 0x18: snprintf(out, n, "((%s - 0.5) * 2.0)", x); break;
    case 0x20: snprintf(out, n, "(%s * 4.0)", x); break;
    case 0x30: snprintf(out, n, "(%s / 2.0)", x); break;
    default:   snprintf(out, n, "%s", x); break;
    }
}

/* One half-stage. Computation goes to `code`; register assignments to `assign` (applied after both halves). */
static void stage_half(Ctx *c, Str *code, Str *assign, Inputs in, Output o, int is_alpha, int stage) {
    const char *mask = is_alpha ? "a" : "rgb";
    const char *cast = is_alpha ? "" : "float3";
    char a[300], b[300], cc[300], d[300];
    input_expr(c, in.a, is_alpha, a, sizeof a); input_expr(c, in.b, is_alpha, b, sizeof b);
    input_expr(c, in.c, is_alpha, cc, sizeof cc); input_expr(c, in.d, is_alpha, d, sizeof d);
    char ab[700], cd[700], sum[1500], tmp[1600], m[1700];
    if (o.ab_dot && !is_alpha) snprintf(ab, sizeof ab, "dot(%s, %s)", a, b); else snprintf(ab, sizeof ab, "(%s * %s)", a, b);
    if (o.cd_dot && !is_alpha) snprintf(cd, sizeof cd, "dot(%s, %s)", cc, d); else snprintf(cd, sizeof cd, "(%s * %s)", cc, d);
    char abdst[32], cddst[32], msdst[32];
    reg_name(c, o.ab, 1, abdst, sizeof abdst); reg_name(c, o.cd, 1, cddst, sizeof cddst); reg_name(c, o.muxsum, 1, msdst, sizeof msdst);
    char abm[1700], cdm[1700];
    map_output(o.mapping, ab, abm, sizeof abm); map_output(o.mapping, cd, cdm, sizeof cdm);
    int s = stage;
    if (abdst[0]) { sadd(code, "  ab%d_%s = clamp(%s(%s), -1.0, 1.0);\n", s, mask, cast, abm); }
    if (cddst[0]) { sadd(code, "  cd%d_%s = clamp(%s(%s), -1.0, 1.0);\n", s, mask, cast, cdm); }
    if (!o.mux) snprintf(sum, sizeof sum, "(%s + %s)", ab, cd);
    else snprintf(sum, sizeof sum, "((%s) ? %s(%s) : %s(%s))", (c->flags & MUX_MSB) ? "r0.a >= 0.5" : "(uint(r0.a * 255.0) & 1u) == 1u",
                  cast, cd, cast, ab);
    map_output(o.mapping, sum, m, sizeof m);
    if (msdst[0]) sadd(code, "  ms%d_%s = clamp(%s(%s), -1.0, 1.0);\n", s, mask, cast, m);
    (void)tmp;
    if (abdst[0]) {
        sadd(assign, "  %s.%s = ab%d_%s;\n", abdst, mask, s, mask);
        if (!is_alpha && o.ab_b2a) sadd(assign, "  %s.a = ab%d_%s.b;\n", abdst, s, mask);
    }
    if (cddst[0]) {
        sadd(assign, "  %s.%s = cd%d_%s;\n", cddst, mask, s, mask);
        if (!is_alpha && o.cd_b2a) sadd(assign, "  %s.a = cd%d_%s.b;\n", cddst, s, mask);
    }
    if (msdst[0]) sadd(assign, "  %s.%s = ms%d_%s;\n", msdst, mask, s, mask);
}

/* Returns a malloc'd MSL source containing `float4 fs_main(...)`. Expects the VSOut struct (vsh.c prelude). */
char *psh_to_msl(const PshState *s) {
    Ctx c = { .s = s, .flags = s->control >> 8 };
    int nst = s->control & 0xFF;
    if (nst > 8) nst = 8;
    Str body = {0}, decl = {0};

    /* texture stage inputs */
    int mode[4];
    for (int i = 0; i < 4; i++) mode[i] = (s->stage_program >> (i * 5)) & 0x1F;
    int input_tex[4] = { -1, 0, (int)((s->other_stage_input >> 16) & 0xF), (int)((s->other_stage_input >> 20) & 0xF) };
    for (int i = 0; i < 4; i++) {
        if (mode[i] == 0) { sadd(&body, "  float4 t%d = float4(0.0);\n", i); continue; }
        if (mode[i] == 4) { sadd(&body, "  float4 t%d = in.t%d;\n", i, i); continue; }        /* PASSTHRU */
        if ((mode[i] == 6 || mode[i] == 7) && i >= 1 && input_tex[i] >= 0 && input_tex[i] < i) {   /* BUMPENVMAP(_LUM) */
            /* (du,dv) comes from an earlier stage's texture as signed bytes; the 2x2 bump matrix scales it and it is added to
             * this stage's coordinates before sampling. _LUM also modulates the result by luminance (red channel). */
            int src = input_tex[i];
            sadd(&body, "  float4 t%d = float4(0.0);\n"
                        "  if (k.tex_flags[%d][1]) {\n"
                        "    float2 dsdt = float2(sign3(t%d.b), sign3(t%d.g));\n"
                        "    dsdt = float2x2(float2(k.bump_mat[%d][0], k.bump_mat[%d][1]), float2(k.bump_mat[%d][2], k.bump_mat[%d][3])) * dsdt;\n"
                        "    float2 uv = in.t%d.xy + dsdt;\n"
                        "    if (!k.tex_flags[%d][0]) uv /= k.tex_size[%d].xy;\n"
                        "    t%d = tex%d.sample(smp%d, uv);\n", i, i, src, src, i, i, i, i, i, i, i, i, i, i);
            if (mode[i] == 7) sadd(&body, "    t%d = t%d * (k.bump_scale[%d][0] * t%d.r + k.bump_offset[%d][0]);\n", i, i, i, src, i);
            sadd(&body, "  }\n");
            continue;
        }
        sadd(&body, "  float4 t%d = float4(0.0);\n"
                    "  if (k.tex_flags[%d][1]) { float w = in.t%d.w; float2 uv = in.t%d.xy / (w == 0.0 ? 1.0 : w);\n"
                    "    if (!k.tex_flags[%d][0]) uv /= k.tex_size[%d].xy;\n"
                    "    t%d = smpl(tex%d, smp%d, uv, k.tex_flags[%d][2]); }\n", i, i, i, i, i, i, i, i, i, i);
    }
    sadd(&body, "  float4 v0 = in.d0, v1 = in.d1;\n  float4 r0 = float4(0.0), r1 = float4(0.0);\n");
    sadd(&body, mode[0] != 0 ? "  r0.a = t0.a;\n" : "  r0.a = 1.0;\n");
    sadd(&body, "  float4 pFog = float4(k.fog.rgb, clamp(in.fog.x, 0.0, 1.0));\n");

    for (int i = 0; i < nst; i++) {
        c.cur_stage = i;
        Str code = {0}, assign = {0};
        sadd(&decl, "  float3 ab%d_rgb, cd%d_rgb, ms%d_rgb; float ab%d_a, cd%d_a, ms%d_a;\n", i, i, i, i, i, i);
        stage_half(&c, &code, &assign, parse_inputs(s->rgb_icw[i]), parse_output(s->rgb_ocw[i]), 0, i);
        stage_half(&c, &code, &assign, parse_inputs(s->alpha_icw[i]), parse_output(s->alpha_ocw[i]), 1, i);
        sadd(&body, "  /* stage %d */\n%s%s", i, code.p ? code.p : "", assign.p ? assign.p : "");
        free(code.p); free(assign.p);
    }

    if (s->final0 || s->final1) {
        c.cur_stage = 8;
        int fl = s->final1 & 0xFF;
        c.final_clamp_sum = fl & 0x80; c.final_inv_v1 = fl & 0x40; c.final_inv_r0 = fl & 0x20;
        Inputs f0 = parse_inputs(s->final0), f1 = parse_inputs(s->final1);     /* f1: e,f,g,(unused) */
        char e[300], f[300], a[300], b[300], cc[300], d[300], g[300];
        input_expr(&c, f1.a, 0, e, sizeof e); input_expr(&c, f1.b, 0, f, sizeof f);
        snprintf(c.var_e, sizeof c.var_e, "%s", e); snprintf(c.var_f, sizeof c.var_f, "%s", f);
        input_expr(&c, f0.a, 0, a, sizeof a); input_expr(&c, f0.b, 0, b, sizeof b);
        input_expr(&c, f0.c, 0, cc, sizeof cc); input_expr(&c, f0.d, 0, d, sizeof d);
        input_expr(&c, f1.c, 1, g, sizeof g);
        sadd(&body, "  /* final combiner */\n  float4 fragColor;\n  fragColor.rgb = %s + mix(float3(%s), float3(%s), float3(%s));\n  fragColor.a = %s;\n",
             d, cc, b, a, g);
    } else {
        sadd(&body, "  float4 fragColor = float4(r0.rgb, r0.a);\n");
    }

    const char *cmp = NULL;
    switch (s->alpha_func) {
    case 0x201: cmp = "<"; break; case 0x202: cmp = "=="; break; case 0x203: cmp = "<="; break;
    case 0x204: cmp = ">"; break; case 0x205: cmp = "!="; break; case 0x206: cmp = ">="; break;
    default: break;
    }
    if (s->alpha_test) {
        if (s->alpha_func == 0x200) sadd(&body, "  discard_fragment();\n");
        else if (cmp) sadd(&body, "  { int fa = int(round(fragColor.a * 255.0)); if (!(fa %s int(k.alpha_ref))) discard_fragment(); }\n", cmp);
    }

    Str out = {0};
    sadd(&out,
        "struct PshK { float4 c0[9]; float4 c1[9]; float4 fog; float4 tex_size[4]; uint4 tex_flags[4]; float4 bump_mat[4]; float4 bump_scale[4]; float4 bump_offset[4]; uint alpha_ref; uint3 pad; };\n"
        "float sign3(float x) { x *= 255.0; return x >= 128.0 ? (x - 256.0) / 127.0 : x / 127.0; }\n"
        "/* Smooth upsampling for the low-resolution bloom textures (tex_flags.z = 1): cubic B-spline through four bilinear taps. */\n"
        "float4 smpl(texture2d<float> t, sampler s, float2 uv, uint hq) {\n"
        "  if (hq == 0u) return t.sample(s, uv);\n"
        "  constexpr sampler ls(filter::linear, address::clamp_to_edge);\n"
        "  float2 sz = float2(t.get_width(), t.get_height());\n"
        "  float2 x = uv * sz - 0.5, ix = floor(x), f = x - ix, f2 = f * f, f3 = f2 * f, omf = 1.0 - f;\n"
        "  float2 w0 = omf * omf * omf / 6.0, w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0, w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0, w3 = f3 / 6.0;\n"
        "  float2 g0 = w0 + w1, g1 = w2 + w3;\n"
        "  float2 p0 = (ix - 1.0 + w1 / g0 + 0.5) / sz, p1 = (ix + 1.0 + w3 / g1 + 0.5) / sz;\n"
        "  return g0.y * (g0.x * t.sample(ls, float2(p0.x, p0.y)) + g1.x * t.sample(ls, float2(p1.x, p0.y))) +\n"
        "         g1.y * (g0.x * t.sample(ls, float2(p0.x, p1.y)) + g1.x * t.sample(ls, float2(p1.x, p1.y))); }\n"
        "fragment float4 fs_main(VSOut in [[stage_in]], constant PshK &k [[buffer(0)]],\n"
        "    texture2d<float> tex0 [[texture(0)]], texture2d<float> tex1 [[texture(1)]],\n"
        "    texture2d<float> tex2 [[texture(2)]], texture2d<float> tex3 [[texture(3)]],\n"
        "    sampler smp0 [[sampler(0)]], sampler smp1 [[sampler(1)]], sampler smp2 [[sampler(2)]], sampler smp3 [[sampler(3)]]) {\n"
        "%s%s  return fragColor; }\n", decl.p ? decl.p : "", body.p ? body.p : "");
    free(decl.p); free(body.p);
    return out.p;
}
