/* Runtime support for lifted original-Xbox x86 code.
 *
 * Guest memory is one flat 4 GB host mapping; a guest address is an offset
 * from G. Each lifted function takes the CPU register file, loads registers
 * into locals (so the C compiler can allocate them), and spills them back
 * around calls and on return. Flags are computed eagerly into locals and
 * left to the compiler's dead-code elimination to trim.
 *
 * SSE: the Xbox CPU is a Pentium III, so only SSE1 (single precision) exists.
 * All float helpers below reproduce x86 semantics bit-for-bit where x86 and
 * ARM differ: NaN propagation / default NaN (0xFFC00000), min/max operand
 * order, and out-of-range float->int conversion. Compile with
 * -ffp-contract=off so ARM never fuses a*b+c.
 */
#ifndef STW_RT_H
#define STW_RT_H
#include <stdint.h>
#include <string.h>

typedef union { float f[4]; uint32_t u[4]; uint64_t q[2]; } XMM;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    XMM xmm[8];
    double fst[8];                 /* x87 physical registers (53-bit precision model) */
    uint32_t ftop, fcc, fcw;       /* TOP, condition codes C0/C2/C3 (bits 8/10/14), control word */
    uint32_t flags;                /* CF|PF<<2|AF<<4|ZF<<6|SF<<7|OF<<11, carried across tail calls */
    uint32_t fs;                   /* guest address of this thread's KPCR/TIB: what fs: selects */
    uint64_t mm[8];                /* MMX registers (kept apart from the x87 stack; emms is a no-op) */
} CPU;

extern uint8_t *G;
#define FS_BASE (c->fs)       /* lifted code addresses fs: relative to the running thread's KPCR */

/* indirect call/jmp through the generated FUNCS table; rt_bad_indirect handles unlifted targets */
#ifdef STW_RUNTIME
extern void (*stw_call_ind)(CPU *c, uint32_t target);   /* the dispatcher lives in the generated code (a dylib in the app build) */
#define call_ind(c, t) stw_call_ind((c), (t))
#else
void call_ind(CPU *c, uint32_t target);
#endif
void rt_bad_indirect(CPU *c, uint32_t target);   /* targets call_ind cannot find: kernel stubs, lib code; may not return */

/* ---- guest memory ---- */
#define LD8(a)  (*(uint8_t *)(G + (uint32_t)(a)))
static inline uint16_t LD16(uint32_t a) { uint16_t v; memcpy(&v, G + a, 2); return v; }
static inline uint32_t LD32(uint32_t a) { uint32_t v; memcpy(&v, G + a, 4); return v; }
static inline uint64_t LD64(uint32_t a) { uint64_t v; memcpy(&v, G + a, 8); return v; }
static inline XMM LDX(uint32_t a) { XMM v; memcpy(&v, G + a, 16); return v; }
static inline float LDF(uint32_t a) { float v; memcpy(&v, G + a, 4); return v; }
/* Stores into the NV2A register window (0xFD000000..0xFDFFFFFF) go through mmio_write so registers can
 * have hardware semantics (write-1-to-clear, kicks, flush bits). Everything else is a plain store.
 * The ...RAW variants are for the device model itself. */
#define IS_MMIO(a) ((((uint32_t)(a)) >> 24) == 0xFDu)
void mmio_write(uint32_t addr, int size, uint64_t value);
#define ST8RAW(a, v)  (*(uint8_t *)(G + (uint32_t)(a)) = (uint8_t)(v))
static inline void ST16RAW(uint32_t a, uint16_t v) { memcpy(G + a, &v, 2); }
static inline void ST32RAW(uint32_t a, uint32_t v) { memcpy(G + a, &v, 4); }
static inline void ST64RAW(uint32_t a, uint64_t v) { memcpy(G + a, &v, 8); }
#define ST8(a, v)  do { uint32_t _a = (uint32_t)(a); if (__builtin_expect(IS_MMIO(_a), 0)) mmio_write(_a, 1, (uint8_t)(v)); else ST8RAW(_a, v); } while (0)
static inline void ST16(uint32_t a, uint16_t v) { if (__builtin_expect(IS_MMIO(a), 0)) mmio_write(a, 2, v); else ST16RAW(a, v); }
static inline void ST32(uint32_t a, uint32_t v) { if (__builtin_expect(IS_MMIO(a), 0)) mmio_write(a, 4, v); else ST32RAW(a, v); }
static inline void ST64(uint32_t a, uint64_t v) { if (__builtin_expect(IS_MMIO(a), 0)) { mmio_write(a, 4, (uint32_t)v); mmio_write(a + 4, 4, (uint32_t)(v >> 32)); } else ST64RAW(a, v); }
static inline void STX(uint32_t a, XMM v) { memcpy(G + a, &v, 16); }

/* Flags are locals; at a tail call / fallthrough into another discovered function (which may be a
 * mid-function split) they are handed over through the CPU struct. */
#define FSPILL() (c->flags = CF | (PF << 2) | (AF << 4) | (ZF << 6) | (SF << 7) | (OF << 11))
#define PAR(r) (!(__builtin_popcount((uint8_t)(r)) & 1))

#define SPILL()  do { c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; \
                      c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)
#define RELOAD() do { eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; \
                      esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)

/* ---- SSE1 scalar/packed float helpers ---- */
static inline float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float x_q(float a) { return bf(fb(a) | 0x00400000u); }         /* quieten a NaN */
#define X_DEFNAN 0xFFC00000u                                                   /* x86 "QNaN indefinite" */

/* x86: if dest is NaN return it (quieted), else if src is NaN return it,
 * else an invalid operation yields the default NaN. */
#ifdef QEMU_REF
/* Test-only: reproduce the NaN selection of the QEMU 5.0 softfloat inside Unicorn, which applies
 * the x87 rule to SSE (confirmed wrong against Rosetta/Intel SDM). Lets the differential test verify
 * everything else without NaN-selection noise. Never defined in a production build. */
static inline float x_fix2(float a, float b, float r) {
    int an = (a != a), bn = (b != b);
    if (an || bn) {
        if (an && bn) {
            uint32_t ua = fb(a), ub = fb(b);
            int as = !(ua & 0x400000u), bs = !(ub & 0x400000u);
            if (as && !bs) return x_q(b);
            if (!as && bs) return x_q(a);
            uint32_t ma = ua << 1, mb = ub << 1;
            int a_larger = (ma < mb) ? 0 : (mb < ma) ? 1 : (ua < ub);
            return x_q(a_larger ? a : b);
        }
        return x_q(an ? a : b);
    }
    return (r != r) ? bf(X_DEFNAN) : r;
}
#else
static inline float x_fix2(float a, float b, float r) {
    if (a != a) return x_q(a);
    if (b != b) return x_q(b);
    if (r != r) return bf(X_DEFNAN);
    return r;
}
#endif
static inline float x_add(float a, float b) { return x_fix2(a, b, a + b); }
static inline float x_sub(float a, float b) { return x_fix2(a, b, a - b); }
static inline float x_mul(float a, float b) { return x_fix2(a, b, a * b); }
static inline float x_div(float a, float b) { return x_fix2(a, b, a / b); }
/* x86 min/max return the *source* operand when unordered or equal */
static inline float x_min(float a, float b) { return a < b ? a : b; }
static inline float x_max(float a, float b) { return a > b ? a : b; }
static inline float x_sqrt(float a) {
    if (a != a) return x_q(a);
    float r = __builtin_sqrtf(a);
    return (r != r) ? bf(X_DEFNAN) : r;
}
/* Real hardware gives a 12-bit approximation; we use the exact result
 * (what QEMU/Unicorn computes). Games normally follow with Newton-Raphson. */
static inline float x_rcp(float a) { return x_div(1.0f, a); }
static inline float x_rsqrt(float a) { return x_div(1.0f, x_sqrt(a)); }

#define P2(name, fn) \
    static inline XMM p_##name(XMM a, XMM b) { XMM r; for (int i = 0; i < 4; i++) r.f[i] = fn(a.f[i], b.f[i]); return r; } \
    static inline XMM s_##name(XMM a, float b) { a.f[0] = fn(a.f[0], b); return a; }
P2(add, x_add) P2(sub, x_sub) P2(mul, x_mul) P2(div, x_div) P2(min, x_min) P2(max, x_max)
#define P1(name, fn) \
    static inline XMM p_##name(XMM b) { XMM r; for (int i = 0; i < 4; i++) r.f[i] = fn(b.f[i]); return r; } \
    static inline XMM s_##name(XMM a, float b) { a.f[0] = fn(b); return a; }
P1(sqrt, x_sqrt) P1(rcp, x_rcp) P1(rsqrt, x_rsqrt)

static inline XMM p_and(XMM a, XMM b)  { a.q[0] &= b.q[0]; a.q[1] &= b.q[1]; return a; }
static inline XMM p_andn(XMM a, XMM b) { a.q[0] = ~a.q[0] & b.q[0]; a.q[1] = ~a.q[1] & b.q[1]; return a; }
static inline XMM p_or(XMM a, XMM b)   { a.q[0] |= b.q[0]; a.q[1] |= b.q[1]; return a; }
static inline XMM p_xor(XMM a, XMM b)  { a.q[0] ^= b.q[0]; a.q[1] ^= b.q[1]; return a; }

static inline uint32_t x_cmp(int p, float a, float b) {
    int r;
    switch (p) {
    case 0: r = a == b; break;
    case 1: r = a < b; break;
    case 2: r = a <= b; break;
    case 3: r = (a != a) || (b != b); break;
    case 4: r = !(a == b); break;
    case 5: r = !(a < b); break;
    case 6: r = !(a <= b); break;
    default: r = (a == a) && (b == b); break;
    }
    return r ? 0xFFFFFFFFu : 0;
}
static inline XMM p_cmp(int p, XMM a, XMM b) { XMM r; for (int i = 0; i < 4; i++) r.u[i] = x_cmp(p, a.f[i], b.f[i]); return r; }
static inline XMM s_cmp(int p, XMM a, float b) { a.u[0] = x_cmp(p, a.f[0], b); return a; }

static inline XMM s_cvtsi(XMM a, int32_t i) { a.f[0] = (float)i; return a; }
/* out-of-range or NaN -> 0x80000000 ("integer indefinite") */
static inline uint32_t x_cvtt(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline uint32_t x_cvt(float f) {                 /* round-to-nearest-even (MXCSR default) */
    float r = __builtin_rintf(f);
    if (!(r >= -2147483648.0f && r < 2147483648.0f)) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

static inline XMM p_shuf(XMM a, XMM b, int i) {
    XMM r;
    r.u[0] = a.u[i & 3]; r.u[1] = a.u[(i >> 2) & 3];
    r.u[2] = b.u[(i >> 4) & 3]; r.u[3] = b.u[(i >> 6) & 3];
    return r;
}
static inline XMM p_unpcklps(XMM a, XMM b) { XMM r; r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1]; return r; }
static inline XMM p_unpckhps(XMM a, XMM b) { XMM r; r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3]; return r; }
static inline uint32_t p_movmsk(XMM a) { return (a.u[0] >> 31) | ((a.u[1] >> 31) << 1) | ((a.u[2] >> 31) << 2) | ((a.u[3] >> 31) << 3); }

/* ---- x87 ----
 * The stack is modelled as 8 doubles. The game never changes the precision
 * bits (only rounding control, around fistp), and the Xbox kernel's default
 * control word is 0x027F (53-bit precision), so a double reproduces the
 * hardware's results exactly apart from the wider exponent range.
 * Arithmetic ignores the rounding-control bits (nearest-even); rounding
 * *conversions* (frndint, fist/fistp) honour them.
 * Locals used by lifted code: fs[8], ftop, fcc.  fcw lives in c->fcw. */
#include <math.h>
static inline uint64_t db(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double bd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
#define XD_QBIT   0x0008000000000000ull
#define XD_SIGN   0x8000000000000000ull
#define XD_DEFNAN 0xFFF8000000000000ull        /* x87 "real indefinite" */
static inline double xd_q(double a) { return bd(db(a) | XD_QBIT); }
/* x87 NaN rules: SNaN vs QNaN -> the QNaN; two NaNs of the same kind -> larger
 * significand; result is always quieted; invalid operation -> real indefinite. */
static inline double xd_fix2(double a, double b, double r) {
    int an = (a != a), bn = (b != b);
    if (an || bn) {
        if (an && bn) {
            uint64_t ua = db(a), ub = db(b);
            int as = !(ua & XD_QBIT), bs = !(ub & XD_QBIT);
            if (as && !bs) return b;
            if (!as && bs) return a;
            uint64_t ma = ua & 0xFFFFFFFFFFFFFull, mb = ub & 0xFFFFFFFFFFFFFull;
            return xd_q(mb > ma ? b : a);
        }
        return xd_q(an ? a : b);
    }
    return (r != r) ? bd(XD_DEFNAN) : r;
}
static inline double xd_fix1(double a, double r) { return xd_fix2(a, a, r); }
static inline double xd_add(double a, double b) { return xd_fix2(a, b, a + b); }
static inline double xd_sub(double a, double b) { return xd_fix2(a, b, a - b); }
static inline double xd_mul(double a, double b) { return xd_fix2(a, b, a * b); }
static inline double xd_div(double a, double b) { return xd_fix2(a, b, a / b); }
static inline double xd_sqrt(double a) { return xd_fix1(a, __builtin_sqrt(a)); }
static inline double xd_round(double v, uint32_t rc) {
    switch (rc & 3) {
    case 0: return __builtin_rint(v);
    case 1: return __builtin_floor(v);
    case 2: return __builtin_ceil(v);
    default: return __builtin_trunc(v);
    }
}
/* fist/fistp: result truncated to `bits`; out-of-range/NaN -> integer indefinite */
static inline uint64_t xd_fist(double v, int bits, uint32_t rc) {
    double r = xd_round(v, rc), lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return 1ull << (bits - 1);
    return (uint64_t)(int64_t)r;
}
static inline uint32_t xd_cc(double a, double b) {      /* C3|C2|C0 for fcom/fucom/fcomi */
    if (a != a || b != b) return 0x4000 | 0x400 | 0x100;
    if (a > b) return 0;
    if (a < b) return 0x100;
    return 0x4000;
}
static inline double xd_sin(double x) {
    return (__builtin_fabs(x) >= 9223372036854775808.0) ? x : xd_fix1(x, __builtin_sin(x));
}
static inline double xd_cos(double x) {
    return (__builtin_fabs(x) >= 9223372036854775808.0) ? x : xd_fix1(x, __builtin_cos(x));
}


/* ---- MMX: 64-bit packed integer lanes ---- */
typedef union { uint64_t q; uint32_t d[2]; int32_t sd[2]; uint16_t w[4]; int16_t sw[4]; uint8_t b[8]; int8_t sb[8]; } MMV;
static inline int32_t mm_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline int32_t mm_sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }
static inline int32_t mm_usat8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : v; }
static inline int32_t mm_usat16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : v; }
#define MMLANES(NAME, T, F, N, EXPR) \
    static inline uint64_t NAME(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; \
        for (int i = 0; i < N; i++) { T p = a.F[i], q = b.F[i]; (void)p; (void)q; r.F[i] = (EXPR); } return r.q; }
MMLANES(mm_paddb, uint8_t, b, 8, p + q)
MMLANES(mm_paddw, uint16_t, w, 4, p + q)
MMLANES(mm_paddd, uint32_t, d, 2, p + q)
MMLANES(mm_psubb, uint8_t, b, 8, p - q)
MMLANES(mm_psubw, uint16_t, w, 4, p - q)
MMLANES(mm_psubd, uint32_t, d, 2, p - q)
MMLANES(mm_paddsb, int8_t, sb, 8, mm_sat8(p + q))
MMLANES(mm_paddsw, int16_t, sw, 4, mm_sat16(p + q))
MMLANES(mm_psubsb, int8_t, sb, 8, mm_sat8(p - q))
MMLANES(mm_psubsw, int16_t, sw, 4, mm_sat16(p - q))
MMLANES(mm_paddusb, uint8_t, b, 8, mm_usat8(p + q))
MMLANES(mm_paddusw, uint16_t, w, 4, mm_usat16(p + q))
MMLANES(mm_psubusb, uint8_t, b, 8, mm_usat8(p - q))
MMLANES(mm_psubusw, uint16_t, w, 4, mm_usat16(p - q))
MMLANES(mm_pmullw, int16_t, sw, 4, (int16_t)(p * q))
MMLANES(mm_pmulhw, int16_t, sw, 4, (int16_t)((p * q) >> 16))
MMLANES(mm_pmulhuw, uint16_t, w, 4, (uint16_t)(((uint32_t)p * q) >> 16))
MMLANES(mm_pcmpeqb, uint8_t, b, 8, p == q ? 0xFF : 0)
MMLANES(mm_pcmpeqw, uint16_t, w, 4, p == q ? 0xFFFF : 0)
MMLANES(mm_pcmpeqd, uint32_t, d, 2, p == q ? 0xFFFFFFFFu : 0)
MMLANES(mm_pcmpgtb, int8_t, sb, 8, p > q ? -1 : 0)
MMLANES(mm_pcmpgtw, int16_t, sw, 4, p > q ? -1 : 0)
MMLANES(mm_pcmpgtd, int32_t, sd, 2, p > q ? -1 : 0)
MMLANES(mm_pavgb, uint8_t, b, 8, (p + q + 1) >> 1)
MMLANES(mm_pavgw, uint16_t, w, 4, (p + q + 1) >> 1)
MMLANES(mm_pminub, uint8_t, b, 8, p < q ? p : q)
MMLANES(mm_pmaxub, uint8_t, b, 8, p > q ? p : q)
MMLANES(mm_pminsw, int16_t, sw, 4, p < q ? p : q)
MMLANES(mm_pmaxsw, int16_t, sw, 4, p > q ? p : q)
static inline uint64_t mm_pand(uint64_t a, uint64_t b) { return a & b; }
static inline uint64_t mm_pandn(uint64_t a, uint64_t b) { return ~a & b; }
static inline uint64_t mm_por(uint64_t a, uint64_t b) { return a | b; }
static inline uint64_t mm_pxor(uint64_t a, uint64_t b) { return a ^ b; }
static inline uint64_t mm_pmaddwd(uint64_t x, uint64_t y) {
    MMV a, b, r; a.q = x; b.q = y;
    r.sd[0] = a.sw[0] * b.sw[0] + a.sw[1] * b.sw[1];
    r.sd[1] = a.sw[2] * b.sw[2] + a.sw[3] * b.sw[3];
    return r.q;
}
static inline uint64_t mm_punpcklbw(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; for (int i = 0; i < 4; i++) { r.b[2*i] = a.b[i]; r.b[2*i+1] = b.b[i]; } return r.q; }
static inline uint64_t mm_punpckhbw(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; for (int i = 0; i < 4; i++) { r.b[2*i] = a.b[i+4]; r.b[2*i+1] = b.b[i+4]; } return r.q; }
static inline uint64_t mm_punpcklwd(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; r.w[0] = a.w[0]; r.w[1] = b.w[0]; r.w[2] = a.w[1]; r.w[3] = b.w[1]; return r.q; }
static inline uint64_t mm_punpckhwd(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; r.w[0] = a.w[2]; r.w[1] = b.w[2]; r.w[2] = a.w[3]; r.w[3] = b.w[3]; return r.q; }
static inline uint64_t mm_punpckldq(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; r.d[0] = a.d[0]; r.d[1] = b.d[0]; return r.q; }
static inline uint64_t mm_punpckhdq(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; r.d[0] = a.d[1]; r.d[1] = b.d[1]; return r.q; }
static inline uint64_t mm_packsswb(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; for (int i = 0; i < 4; i++) { r.sb[i] = mm_sat8(a.sw[i]); r.sb[i+4] = mm_sat8(b.sw[i]); } return r.q; }
static inline uint64_t mm_packuswb(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; for (int i = 0; i < 4; i++) { r.b[i] = mm_usat8(a.sw[i]); r.b[i+4] = mm_usat8(b.sw[i]); } return r.q; }
static inline uint64_t mm_packssdw(uint64_t x, uint64_t y) { MMV a, b, r; a.q = x; b.q = y; r.sw[0] = mm_sat16(a.sd[0]); r.sw[1] = mm_sat16(a.sd[1]); r.sw[2] = mm_sat16(b.sd[0]); r.sw[3] = mm_sat16(b.sd[1]); return r.q; }
/* shifts: count is the full 64-bit operand; counts >= the lane width zero (or sign-fill) the lanes */
static inline uint64_t mm_psllw(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 15) return 0; for (int i = 0; i < 4; i++) a.w[i] <<= n; return a.q; }
static inline uint64_t mm_pslld(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 31) return 0; for (int i = 0; i < 2; i++) a.d[i] <<= n; return a.q; }
static inline uint64_t mm_psllq(uint64_t x, uint64_t n) { return n > 63 ? 0 : x << n; }
static inline uint64_t mm_psrlw(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 15) return 0; for (int i = 0; i < 4; i++) a.w[i] >>= n; return a.q; }
static inline uint64_t mm_psrld(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 31) return 0; for (int i = 0; i < 2; i++) a.d[i] >>= n; return a.q; }
static inline uint64_t mm_psrlq(uint64_t x, uint64_t n) { return n > 63 ? 0 : x >> n; }
static inline uint64_t mm_psraw(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 15) n = 15; for (int i = 0; i < 4; i++) a.sw[i] >>= n; return a.q; }
static inline uint64_t mm_psrad(uint64_t x, uint64_t n) { MMV a; a.q = x; if (n > 31) n = 31; for (int i = 0; i < 2; i++) a.sd[i] >>= n; return a.q; }
static inline uint64_t mm_pshufw(uint64_t x, uint32_t imm) { MMV a, r; a.q = x; for (int i = 0; i < 4; i++) r.w[i] = a.w[(imm >> (2 * i)) & 3]; return r.q; }
static inline uint32_t mm_pmovmskb(uint64_t x) { MMV a; a.q = x; uint32_t r = 0; for (int i = 0; i < 8; i++) r |= (uint32_t)(a.b[i] >> 7) << i; return r; }

/* 80-bit extended <-> the double model */
static inline double xd_ld80(uint32_t a) {
    uint64_t mant = LD64(a);
    uint16_t se = LD16(a + 8);
    int sign = se >> 15, ex = se & 0x7FFF;
    double v;
    if (ex == 0x7FFF) {
        if ((mant << 1) == 0) v = __builtin_inf();
        else return bd(((uint64_t)sign << 63) | 0x7FF8000000000000ull | ((mant << 1) >> 12));
    } else if (ex == 0) v = ldexp((double)mant, -16382 - 63);
    else v = ldexp((double)mant, ex - 16383 - 63);
    return sign ? -v : v;
}
static inline void xd_st80(uint32_t a, double v) {
    uint64_t u = db(v), mant;
    uint16_t sign = (uint16_t)(u >> 63) << 15, ex;
    if (v != v) { ex = 0x7FFF; mant = 0x8000000000000000ull | ((u & 0xFFFFFFFFFFFFFull) << 11); }
    else if (__builtin_isinf(v)) { ex = 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v == 0) { ex = 0; mant = 0; }
    else {
        int e2;
        double m = frexp(__builtin_fabs(v), &e2);
        mant = (uint64_t)ldexp(m, 64);
        ex = (uint16_t)(e2 - 1 + 16383);
    }
    ST64(a, mant);
    ST16(a + 8, sign | ex);
}
static inline double xd_tan(double x) {
    return (__builtin_fabs(x) >= 9223372036854775808.0) ? x : xd_fix1(x, __builtin_tan(x));
}
static inline double xd_f2xm1(double x) { return xd_fix1(x, __builtin_expm1(x * 0.693147180559945309417)); }
/* Back-edge poll point: re-read guest memory each iteration, and let a waiting interrupt/timer thread in. */
extern volatile int gil_want;
void gil_yield(void);
void gil_timeslice(void);           /* let other guest threads run (called every few hundred thousand back-edges) */
extern __thread uint64_t rt_poll_count;
void rt_poll_report(CPU *c, const char *fn);
#define POLL() do { __atomic_signal_fence(__ATOMIC_SEQ_CST); if (__builtin_expect(gil_want, 0)) gil_yield(); \
                    ++rt_poll_count; \
                    if (__builtin_expect((rt_poll_count & 0x3FFFF) == 0, 0)) gil_timeslice(); \
                    if (__builtin_expect(rt_poll_count == 3000000000ull, 0)) rt_poll_report(c, __func__); } while (0)
extern int rt_trace_fn;                      /* STW_FNTRACE=1: log every guest function entry */
void rt_trace_enter(CPU *c, uint32_t fn);
void rt_dump_rings(void);
uint32_t rt_in(int size, uint16_t port);                 /* port I/O shims (PCI config, SMBus, ...) */
void rt_out(int size, uint16_t port, uint32_t value);
uint64_t rt_rdtsc(void);                       /* Xbox TSC: 733 MHz, from the host clock */
static inline double xd_atan2(double y, double x) { return xd_fix2(y, x, __builtin_atan2(y, x)); }
static inline double xd_yl2x(double y, double x) {
#ifdef QEMU_REF
    return xd_fix2(y, x, (x > 0.0) ? (log(x) / log(2.0)) * y : __builtin_nan(""));
#else
    return xd_fix2(y, x, y * __builtin_log2(x));
#endif
}
static inline double xd_scale(double a, double b) {
    if (a != a || b != b) return xd_fix2(a, b, 0.0);
    if (b >= 1e6) b = 1e6; else if (b <= -1e6) b = -1e6;
    return xd_fix1(a, ldexp(a, (int)__builtin_trunc(b)));
}
#define ST(i)    fs[(ftop + (i)) & 7]
#define FPUSH(v) do { double _v = (v); ftop = (ftop - 1) & 7; fs[ftop] = _v; } while (0)
#define FPOP()   (ftop = (ftop + 1) & 7)
#define XSPILL()  do { memcpy(c->fst, fs, sizeof fs); c->ftop = ftop; c->fcc = fcc; } while (0)
#define XRELOAD() do { memcpy(fs, c->fst, sizeof fs); ftop = c->ftop; fcc = c->fcc; } while (0)

#endif
