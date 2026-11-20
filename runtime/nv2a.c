/* NV2A device thread. */
#include <sys/resource.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "nv2a.h"
#include "display.h"
#include "gpu.h"
#include <math.h>
#include "rt.h"

#define REG(off) (NV2A_BASE + (off))

static int gpu_trace = -1;
static volatile uint32_t stat_frames, stat_draws;       /* STW_FPS=1: printed once a second */

/* Flip pacing. FLIP_INCREMENT_WRITE (0x12c) counts flips the GPU has been asked for; FLIP_STALL (0x130) makes the pusher wait
 * until the driver's vblank DPC has performed them, which it signals with PGRAPH_INCREMENT (bit 1 = READ_3D++). Without this
 * the GPU renders several frames per vblank and the scanout buffer is the one being cleared and redrawn (visible flicker). */
static volatile uint32_t flip_write, flip_read;
static volatile int flip_stall;
/* Menus and movies draw a few dozen primitives per frame, scenes thousands; input uses this to pick what Esc means. */
static volatile int scene3d;
static uint32_t scene_mark, scene_low;
static void scene_frame(void) {
    uint32_t n = stat_draws - scene_mark;
    scene_mark = stat_draws;
    if (n >= 100) { scene3d = 1; scene_low = 0; }
    else if (++scene_low > 90) scene3d = 0;                           /* ~1.5 s of light frames */
}
int nv2a_scene_3d(void) { return scene3d; }
static inline int flip_blocked(void) {
    static struct timespec since;                                  /* when the current stall began (tv_sec 0 = not stalled) */
    if (!flip_stall) return 0;
    if ((int32_t)(flip_write - __atomic_load_n(&flip_read, __ATOMIC_ACQUIRE)) > 0) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (!since.tv_sec) since = t;
        if (t.tv_sec - since.tv_sec < 2) return 1;
        /* The driver's flip handler never ran (a lost flip would otherwise freeze the GPU for good): give up waiting. */
        fprintf(stderr, "stw: FLIP_STALL timed out (write %u, read %u); releasing\n", flip_write, flip_read);
        __atomic_store_n(&flip_read, flip_write, __ATOMIC_RELEASE);
    }
    since.tv_sec = 0;
    flip_stall = 0;                                                /* one-shot */
    return 0;
}
static struct { uint32_t get, data; uint32_t kind; } swm_ring[32];   /* STW_STALLDUMP: last software-method events (kind 1 raised, 2 acknowledged) */
static volatile uint32_t swm_n;
static void swm_note(uint32_t get, uint32_t data, uint32_t kind) { uint32_t i = __atomic_fetch_add(&swm_n, 1, __ATOMIC_RELAXED) & 31; swm_ring[i].get = get; swm_ring[i].data = data; swm_ring[i].kind = kind; }
static volatile int pusher_stalled;                  /* a software method (NO_OPERATION with data) is waiting for the driver's ISR */

/* ---- instance memory: RAMHT (handle -> object instance) and DMA objects ---- */
#define PRAMIN(off) (NV2A_PRAMIN + (off))

/* PFIFO_RAMHT (0x2210): bits 4..8 = base (4 KB units), bits 16..17 = log2(size / 4 KB). Entries are
 * {handle, context}; context bit 31 = valid, bits 0..15 = instance address >> 4. */
static int ramht_find(uint32_t handle, uint32_t *inst) {
    uint32_t r = LD32(REG(0x2210));
    uint32_t base = ((r >> 4) & 0x1F) << 12, size = 1u << (((r >> 16) & 3) + 12);
    for (uint32_t o = 0; o < size; o += 8) {
        uint32_t ctx = LD32(PRAMIN(base + o + 4));
        if ((ctx >> 31) && LD32(PRAMIN(base + o)) == handle) { *inst = (ctx & 0xFFFF) << 4; return 1; }
    }
    return 0;
}

typedef struct { uint32_t base, limit; int valid; } Dma;
/* DMA object: word0 flags (class, adjust in bits 20..31), word1 limit, word2 page frame | flags. */
static Dma dma_from_handle(uint32_t handle) {
    Dma d = { 0, 0, 0 };
    uint32_t inst;
    if (!handle || !ramht_find(handle, &inst)) return d;
    uint32_t w0 = LD32(PRAMIN(inst)), w1 = LD32(PRAMIN(inst + 4)), w2 = LD32(PRAMIN(inst + 8));
    d.base = (w2 & 0xFFFFF000u) + (w0 >> 20);
    d.limit = w1;
    d.valid = 1;
    return d;
}
static uint32_t object_class(uint32_t handle) {
    uint32_t inst;
    return ramht_find(handle, &inst) ? LD32(PRAMIN(inst)) & 0xFF : 0;
}

/* ---- PGRAPH / kelvin (class 0x97) state ---- */
enum { CLS_KELVIN = 0x97 };
typedef struct {
    uint32_t subch_class[8];
    uint32_t dma_color, dma_zeta, dma_semaphore, dma_notify, dma_state, dma_a, dma_b, dma_vtx_a, dma_vtx_b, dma_report;
    uint32_t semaphore_offset;
    uint32_t color_offset, zeta_offset, surface_pitch_color, surface_pitch_zeta, surface_format;
    uint32_t clear_color, clear_zs;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t clear_rect_h, clear_rect_v;                 /* SET_CLEAR_RECT_HORIZONTAL/VERTICAL: min | max << 16 */
    uint32_t bound[8];
    uint32_t cur_sub;                                      /* subchannel of the method being dispatched */

    /* vertex arrays / immediate data */
    struct { uint32_t type, count, stride, offset; int dma_b; } va[16];
    float cur[16][4];                                      /* current vertex attribute values (SET_VERTEX_DATA*) */
    uint32_t prim;                                         /* 0 outside begin/end */
    uint32_t *inl;                                         /* INLINE_ARRAY words */
    uint32_t n_inl, cap_inl;
    uint32_t *elems;                                       /* ARRAY_ELEMENT indices */
    uint32_t n_elems, cap_elems;
    uint32_t da_start[256], da_count[256], n_da;           /* DRAW_ARRAYS ranges */
    float *imm;                                            /* immediate-mode vertices, 16 float4 each */
    uint32_t n_imm, cap_imm;
    /* transform program / constants */
    uint32_t prog[136 * 4], prog_ld, prog_start;
    float consts[192 * 4];
    uint32_t const_ld;
    float clip_min, clip_max;
    /* state */
    uint32_t depth_enable, depth_func, depth_write, cull_enable, cull_face, front_face;
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_rmask, stencil_wmask, stencil_fail, stencil_zfail, stencil_zpass, stencil_write;
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color, color_mask;
    uint32_t poly_front, poly_back, shade_mode, poly_off_fill, poly_off_line, poly_off_point;
    float poly_off_scale, poly_off_bias, point_size;
    struct { uint32_t offset, format, control0, control1, filter, rect, address; } tex[4];
    PshState psh;
} Pgraph;
static Pgraph pg;

/* Bytes per pixel of the colour / depth surface from SET_SURFACE_FORMAT (colour in bits 0..3, depth in 4..7). */
static int color_bpp(void) {
    switch (pg.surface_format & 0xF) {
    case 1: case 2: case 3: return 2;                    /* X1R5G5B5, R5G6B5 */
    case 0xB: return 1;                                  /* B8 */
    default: return 4;
    }
}

/* Surface geometry. The render window (SET_SURFACE_CLIP) can be a sub-rectangle of the buffer (letterboxing), and with
 * antialiasing the buffer holds clip * scale samples. Vertex programs emit absolute sample-space window coordinates, so
 * the target is the whole buffer; only the clip rectangle is drawable. The buffer's full height is not recorded
 * anywhere for pitch surfaces; a symmetric border around the window is assumed. */
typedef struct { uint32_t full_w, full_h, clip_x, clip_y, clip_w, clip_h; } SurfGeom;
static SurfGeom surface_geom(void) {
    uint32_t aa = (pg.surface_format >> 12) & 0xF, sx = aa >= 1 ? 2 : 1, sy = aa >= 2 ? 2 : 1;
    SurfGeom g;
    g.clip_w = (pg.clip_w ? pg.clip_w : 640) * sx;
    g.clip_h = (pg.clip_h ? pg.clip_h : 480) * sy;
    g.clip_x = pg.clip_x * sx;
    g.clip_y = pg.clip_y * sy;
    g.full_w = g.clip_x * 2 + g.clip_w;
    g.full_h = g.clip_y * 2 + g.clip_h;
    return g;
}

/* CLEAR_SURFACE: mask bit 0 = Z, bit 1 = stencil, bits 4..7 = colour channels. Done directly in guest memory. */
static void clear_surface(uint32_t mask) {
    SurfGeom sg = surface_geom();
    uint32_t sw = sg.full_w, sh = sg.full_h;
    if (gpu_trace) fprintf(stderr, "[clear] mask=%#x color=%#x base=%#x pitch=%#x fmt=%#x rect=%08x/%08x geom=%ux%u\n", mask, pg.clear_color,
                           dma_from_handle(pg.dma_color).base + pg.color_offset, pg.surface_pitch_color, pg.surface_format, pg.clear_rect_h, pg.clear_rect_v, sw, sh);
    uint32_t x0 = pg.clear_rect_h & 0xFFFF, x1 = pg.clear_rect_h >> 16, y0 = pg.clear_rect_v & 0xFFFF, y1 = pg.clear_rect_v >> 16;
    if (!pg.clear_rect_h && !pg.clear_rect_v) { x0 = sg.clip_x; x1 = sg.clip_x + sg.clip_w - 1; y0 = sg.clip_y; y1 = sg.clip_y + sg.clip_h - 1; }
    if (x1 > 4096 || y1 > 4096) return;
    Dma dc = dma_from_handle(pg.dma_color), dz = dma_from_handle(pg.dma_zeta);
    if ((mask & 0xF0) && dc.valid) {
        int bpp = color_bpp();
        uint32_t base = dc.base + pg.color_offset;
        uint32_t v = pg.clear_color;
        if (bpp == 4 && (mask & 0xF0) == 0xF0 && x0 == 0 && y0 == 0 && x1 + 1 >= sw && y1 + 1 >= sh && gpu_clear_color(base, pg.surface_pitch_color, sw, sh, v))
            goto color_done;                                               /* cleared by the GPU, in order with the draws */
        if (bpp == 4 && (mask & 0xF0) == 0xF0 && gpu_clear_color_rect(base, pg.surface_pitch_color, sw, sh, x0, y0, x1, y1, v))
            goto color_done;                                               /* sub-rectangle cleared by the GPU as well */
        gpu_sync();                                                        /* CPU fill: the renderer must be done with this memory */
        for (uint32_t y = y0; y <= y1; y++)
            for (uint32_t x = x0; x <= x1; x++) {
                uint32_t a = base + y * pg.surface_pitch_color + x * bpp;
                if (bpp == 4) ST32RAW(a, v); else if (bpp == 2) ST16RAW(a, (uint16_t)v); else ST8RAW(a, (uint8_t)v);
            }
        gpu_cpu_wrote(base, pg.surface_pitch_color * (y1 + 1));
    }
color_done:
    if ((mask & 3) && dz.valid) {
        /* depth lives in a private Metal texture (the guest never reads its zeta buffer), keyed by address + size */
        gpu_clear_depth(dz.base + pg.zeta_offset, sw, sh, (float)(pg.clear_zs >> 8) / 16777215.f, pg.clear_zs & 0xFF, mask & 3);
    }
}

static void note_surface(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch);
/* ---- vertex fetch and primitive assembly (included into nv2a.c) ---- */
static void push_u32(uint32_t **v, uint32_t *n, uint32_t *cap, uint32_t x) {
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 1024; *v = realloc(*v, *cap * 4); }
    (*v)[(*n)++] = x;
}
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* Decode one attribute value from guest memory at `p` into out[4] (missing components: 0,0,0,1). */
static void decode_attr(uint32_t type, uint32_t count, const uint8_t *p, float out[4]) {
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    switch (type) {
    case 2:  { uint32_t w[4]; memcpy(w, p, 4 * (count < 4 ? count : 4)); for (uint32_t i = 0; i < count && i < 4; i++) out[i] = f32(w[i]); } break;
    case 0:                                                  /* UB_D3D: bytes are B,G,R,A, normalised, delivered as R,G,B,A */
        out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f; break;
    case 4:  for (uint32_t i = 0; i < count && i < 4; i++) out[i] = p[i] / 255.f; break;    /* UB_OGL */
    case 1:  for (uint32_t i = 0; i < count && i < 4; i++) { int16_t x; memcpy(&x, p + 2 * i, 2); out[i] = x / 32767.f; } break;   /* S1 */
    case 5:  for (uint32_t i = 0; i < count && i < 4; i++) { int16_t x; memcpy(&x, p + 2 * i, 2); out[i] = (float)x; } break;      /* S32K */
    case 6: {                                                /* CMP: 11/11/10 signed normalised */
        uint32_t v; memcpy(&v, p, 4);
        int32_t x = (int32_t)(v << 21) >> 21, y = (int32_t)(v << 10) >> 21, z = (int32_t)v >> 22;
        out[0] = x / 1023.f; out[1] = y / 1023.f; out[2] = z / 511.f; break;
    }
    default: break;
    }
}
static uint32_t type_size(uint32_t type) { return type == 2 ? 4 : (type == 1 || type == 5) ? 2 : type == 6 ? 4 : 1; }

/* One bound vertex array, resolved once per draw (the DMA object lookup is far too slow to repeat per vertex). */
typedef struct { uint32_t base, stride, type, count; } ArrAttr;

/* Growable scratch buffers that live across draws (flush_draw only runs on the device thread). */
static void *scratch(void **p, size_t *cap, size_t need) {
    if (need > *cap) { size_t n = *cap ? *cap : 4096; while (n < need) n *= 2; *p = realloc(*p, n); *cap = n; }
    return *p;
}
static float *vtx_buf;      static size_t vtx_cap;     /* compact vertices */
static uint32_t *idx_buf;   static size_t idx_cap;     /* triangle-list indices */
static uint32_t *slot_buf;  static size_t slot_cap;    /* unique-vertex slot of every submitted vertex */
static uint32_t *uid_buf;   static size_t uid_cap;     /* guest vertex id of every unique slot */
static uint32_t *remap, *remap_stamp, remap_n, remap_gen;   /* guest vertex id -> slot, valid when stamp == generation */
#define REMAP_MAX (1u << 22)

/* Appends the triangles of one primitive run as indices into the unique vertices (s = slot per submitted vertex). */
static uint32_t assemble(uint32_t prim, const uint32_t *s, uint32_t n, uint32_t *o) {
    uint32_t k = 0;
#define T(a, b, c) (o[k++] = s[a], o[k++] = s[b], o[k++] = s[c])
    switch (prim) {
    case 5: for (uint32_t i = 0; i + 2 < n; i += 3) T(i, i + 1, i + 2); break;
    case 6: for (uint32_t i = 0; i + 2 < n; i++) { if (i & 1) T(i + 1, i, i + 2); else T(i, i + 1, i + 2); } break;
    case 7: case 10: for (uint32_t i = 1; i + 1 < n; i++) T(0, i, i + 1); break;
    case 8: for (uint32_t i = 0; i + 3 < n; i += 4) { T(i, i + 1, i + 2); T(i, i + 2, i + 3); } break;
    case 9: for (uint32_t i = 0; i + 3 < n; i += 2) { T(i, i + 1, i + 3); T(i, i + 3, i + 2); } break;
    default: { static int warned; if (!warned++) fprintf(stderr, "stw: primitive type %u not rendered yet\n", prim); }
    }
#undef T
    return k;
}

static void fill_draw(GpuDraw *g) {
    memset(g, 0, sizeof *g);
    Dma dc = dma_from_handle(pg.dma_color), dz = dma_from_handle(pg.dma_zeta);
    g->color_addr = dc.base + pg.color_offset;
    g->color_pitch = pg.surface_pitch_color;
    {
        SurfGeom sg = surface_geom();
        g->width = sg.full_w; g->height = sg.full_h;
        g->clip_x = sg.clip_x; g->clip_y = sg.clip_y; g->clip_w = sg.clip_w; g->clip_h = sg.clip_h;
    }
    note_surface(g->color_addr, g->width, g->height, pg.surface_pitch_color);

    g->zeta_addr = dz.base + pg.zeta_offset;
    g->surface_format = pg.surface_format;
    g->vsh = pg.prog; g->vsh_start = pg.prog_start; g->consts = pg.consts;
    g->clip_min = pg.clip_min; g->clip_max = pg.clip_max ? pg.clip_max : 16777215.f;
    g->depth_enable = pg.depth_enable; g->depth_func = pg.depth_func; g->depth_write = pg.depth_write;
    g->cull_enable = pg.cull_enable; g->cull_face = pg.cull_face; g->front_face = pg.front_face;
    g->stencil_enable = pg.stencil_enable; g->stencil_func = pg.stencil_func; g->stencil_ref = pg.stencil_ref; g->stencil_rmask = pg.stencil_rmask;
    g->stencil_wmask = pg.stencil_write ? pg.stencil_wmask : 0;
    g->stencil_fail = pg.stencil_fail; g->stencil_zfail = pg.stencil_zfail; g->stencil_zpass = pg.stencil_zpass;
    g->blend_enable = pg.blend_enable; g->blend_src = pg.blend_src; g->blend_dst = pg.blend_dst;
    g->blend_eq = pg.blend_eq; g->blend_color = pg.blend_color; g->color_mask = pg.color_mask;
    g->shade_mode = pg.shade_mode; g->poly_off_fill = pg.poly_off_fill; g->poly_off_scale = pg.poly_off_scale; g->poly_off_bias = pg.poly_off_bias;
    g->psh = pg.psh;
    for (int i = 0; i < 4; i++) {
        uint32_t f = pg.tex[i].format, col = (f >> 8) & 0xFF;
        GpuTex *t = &g->tex[i];
        t->enabled = (pg.tex[i].control0 >> 30) & 1;
        Dma d = dma_from_handle((f & 3) == 2 ? pg.dma_b : pg.dma_a);
        t->addr = d.base + pg.tex[i].offset;
        t->format = col;
        int dxt = col >= 0x0C && col <= 0x0F && col != 0x0D;
        /* SZ_* formats are swizzled; LU_* ("image") formats are linear; DXT is block-linear */
        t->swizzled = (col <= 0x0B) || col == 0x19 || col == 0x1A || (col >= 0x27 && col <= 0x2C) || col == 0x32 || (col >= 0x3A && col <= 0x3C);
        t->normalized = t->swizzled || dxt;
        t->mip_levels = (t->swizzled || dxt) ? (f >> 16) & 0xF : 1;
        t->cube = (f >> 2) & 1;
        if (t->swizzled || dxt) { t->width = 1u << ((f >> 20) & 15); t->height = 1u << ((f >> 24) & 15); }
        else { t->width = pg.tex[i].rect >> 16; t->height = pg.tex[i].rect & 0xFFFF; }
        t->pitch = pg.tex[i].control1 >> 16;
        t->filter = pg.tex[i].filter; t->address = pg.tex[i].address;
    }
}

static void flush_draw(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_DRAWTRACE") != NULL;
    if (trace && getenv("STW_ATTRS")) {
        fprintf(stderr, "[attrs]");
        for (int i = 0; i < 16; i++) if (pg.va[i].count) fprintf(stderr, " a%d(t%u c%u s%u o%#x)", i, pg.va[i].type, pg.va[i].count, pg.va[i].stride, pg.va[i].offset);
        fprintf(stderr, "\n");
    }
    if (trace) fprintf(stderr, "[flush] prim=%u inl=%u elems=%u da=%u imm=%u va0=(t%u c%u s%u)\n", pg.prim, pg.n_inl, pg.n_elems, pg.n_da, pg.n_imm, pg.va[0].type, pg.va[0].count, pg.va[0].stride);
    uint32_t nsub = 0, nuniq = 0, nact = 0, mask = 0, act[16];
    const float *vsrc = NULL; uint32_t vstride = 0;
    for (int i = 0; i < 16; i++) if (pg.va[i].count) { act[nact++] = i; mask |= 1u << i; }
    if (pg.n_inl) {                                         /* inline array: attributes with count != 0, in slot order */
        uint32_t words = 0, off[16];
        for (int i = 0; i < 16; i++) { off[i] = words; if (pg.va[i].count) words += (pg.va[i].count * type_size(pg.va[i].type) + 3) / 4; }
        if (words) {
            nsub = nuniq = pg.n_inl / words;
            vstride = nact ? nact : 1;
            float *vb = scratch((void **)&vtx_buf, &vtx_cap, (size_t)nuniq * vstride * 16);
            for (uint32_t k = 0; k < nuniq; k++) {
                float *v = vb + (size_t)k * vstride * 4;
                for (uint32_t a = 0; a < nact; a++) {
                    int i = act[a];
                    const uint32_t *w = pg.inl + (size_t)k * words + off[i];
                    decode_attr(pg.va[i].type, pg.va[i].count, (const uint8_t *)w, v + a * 4);
                }
            }
            vsrc = vb;
            uint32_t *sl = scratch((void **)&slot_buf, &slot_cap, (size_t)nsub * 4);
            for (uint32_t k = 0; k < nsub; k++) sl[k] = k;
        }
    } else if (pg.n_elems || pg.n_da) {                     /* indexed or ranged draws from vertex arrays */
        ArrAttr arr[16];
        Dma dmas[2] = { dma_from_handle(pg.dma_vtx_a), dma_from_handle(pg.dma_vtx_b) };
        for (uint32_t a = 0; a < nact; a++) {
            int i = act[a];
            arr[a] = (ArrAttr){ dmas[pg.va[i].dma_b ? 1 : 0].base + pg.va[i].offset, pg.va[i].stride, pg.va[i].type, pg.va[i].count };
        }
        uint32_t total = pg.n_elems;
        for (uint32_t r = 0; r < pg.n_da; r++) total += pg.da_count[r];
        uint32_t *sl = scratch((void **)&slot_buf, &slot_cap, (size_t)total * 4);
        uint32_t *uid = scratch((void **)&uid_buf, &uid_cap, (size_t)total * 4);
        if (pg.n_da == 1 && !pg.n_elems) {                  /* one contiguous range: every vertex is its own slot */
            for (uint32_t i = 0; i < total; i++) { sl[i] = i; uid[i] = pg.da_start[0] + i; }
            nuniq = total;
        } else {                                            /* share vertices that are referenced more than once */
            uint32_t gen = ++remap_gen, k = 0;               /* ranges first, then the element list, as submitted */
            for (uint32_t r = 0; r <= pg.n_da; r++) {
                uint32_t n = r < pg.n_da ? pg.da_count[r] : pg.n_elems;
                for (uint32_t i = 0; i < n; i++, k++) {
                    uint32_t id = r < pg.n_da ? pg.da_start[r] + i : pg.elems[i];
                    if (id < REMAP_MAX) {
                        if (id >= remap_n) {
                            uint32_t nn = remap_n ? remap_n : 4096; while (nn <= id) nn *= 2;
                            remap = realloc(remap, nn * 4); remap_stamp = realloc(remap_stamp, nn * 4);
                            memset(remap_stamp + remap_n, 0, (nn - remap_n) * 4); remap_n = nn;
                        }
                        if (remap_stamp[id] != gen) { remap_stamp[id] = gen; remap[id] = nuniq; uid[nuniq++] = id; }
                        sl[k] = remap[id];
                    } else { sl[k] = nuniq; uid[nuniq++] = id; }
                }
            }
        }
        nsub = total;
        vstride = nact ? nact : 1;
        float *vb = scratch((void **)&vtx_buf, &vtx_cap, (size_t)nuniq * vstride * 16);
        for (uint32_t u = 0; u < nuniq; u++) {
            float *v = vb + (size_t)u * vstride * 4;
            for (uint32_t a = 0; a < nact; a++)
                decode_attr(arr[a].type, arr[a].count, G + (uint32_t)(arr[a].base + uid[u] * arr[a].stride), v + a * 4);
        }
        vsrc = vb;
    } else if (pg.n_imm) {                                  /* immediate mode: every vertex carries all 16 attributes */
        nsub = nuniq = pg.n_imm; vstride = 16; mask = 0xFFFF; vsrc = pg.imm;
        uint32_t *sl = scratch((void **)&slot_buf, &slot_cap, (size_t)nsub * 4);
        for (uint32_t k = 0; k < nsub; k++) sl[k] = k;
    }
    if (vsrc && nsub) {
        uint32_t *ib = scratch((void **)&idx_buf, &idx_cap, (size_t)nsub * 3 * 4);
        uint32_t nidx = assemble(pg.prim, slot_buf, nsub, ib);
        if (nidx) {
            GpuDraw g;
            fill_draw(&g);
            g.verts = vsrc; g.indices = ib; g.nverts = nidx; g.nuniq = nuniq; g.vstride = vstride; g.attr_mask = mask;
            memcpy(g.cur, pg.cur, sizeof g.cur);
            stat_draws++;
            gpu_draw(&g);
        }
    }
    pg.n_inl = pg.n_elems = pg.n_da = pg.n_imm = 0;
}

static void immediate_vertex(void) {                         /* attribute 0 was just written: latch a vertex */
    if (pg.n_imm == pg.cap_imm) { pg.cap_imm = pg.cap_imm ? pg.cap_imm * 2 : 256; pg.imm = realloc(pg.imm, (size_t)pg.cap_imm * 256); }
    memcpy(pg.imm + (size_t)pg.n_imm++ * 64, pg.cur, 256);
}

static uint32_t meth_cnt[0x800], meth_last[0x800];       /* STW_METHODS=1: histogram of kelvin methods, printed every 30 s */
static void kelvin_method(uint32_t method, uint32_t data) {
    if ((method >> 2) < 0x800) { meth_cnt[method >> 2]++; meth_last[method >> 2] = data; }
    switch (method) {
    case 0x180: pg.dma_notify = data; break;
    case 0x184: pg.dma_a = data; break;
    case 0x188: pg.dma_b = data; break;
    case 0x190: pg.dma_state = data; break;
    case 0x194: pg.dma_color = data; break;
    case 0x198: pg.dma_zeta = data; break;
    case 0x19c: pg.dma_vtx_a = data; break;
    case 0x1a0: pg.dma_vtx_b = data; break;
    case 0x1a4: pg.dma_semaphore = data; break;
    case 0x1a8: pg.dma_report = data; break;
    case 0x12c: stat_frames++; scene_frame(); __atomic_add_fetch(&flip_write, 1, __ATOMIC_RELEASE); break;
    case 0x130: flip_stall = 1; break;                                      /* FLIP_INCREMENT_WRITE: one presented frame */
    case 0x100:                                                            /* NO_OPERATION: non-zero data = software method call */
        if (data) {
            gpu_sync_fences();                                             /* the ISR this raises may look at fences released before it */
            if (gpu_trace) fprintf(stderr, "[gpu] software method: sub%u NOP data=%#x\n", pg.cur_sub, data);
            ST32RAW(REG(0x400704), (0u << 20) | (pg.cur_sub << 16) | method);    /* TRAPPED_ADDR: chid, subchannel, method */
            ST32RAW(REG(0x400708), data);                                  /* TRAPPED_DATA_LOW */
            ST32RAW(REG(0x400108), 1);                                     /* NSOURCE: notification */
            ST32RAW(REG(0x400100), LD32(REG(0x400100)) | (1u << 20));      /* INTR: error */
            swm_note(LD32(NV2A_USER + 0x44), data, 1);
            pusher_stalled = 1;
            nv2a_update_pmc();
        }
        break;
    case 0x1d6c: pg.semaphore_offset = data; break;                       /* SET_SEMAPHORE_OFFSET */
    case 0x1d70: {                                                        /* BACK_END_WRITE_SEMAPHORE_RELEASE */
        Dma d = dma_from_handle(pg.dma_semaphore);
        if (d.valid) gpu_fence(d.base + pg.semaphore_offset, data);        /* completes once the GPU has finished what precedes it, without stalling the pusher */
        else gpu_sync();
        if (gpu_trace) fprintf(stderr, "[gpu] semaphore dma=%#x base=%#x off=%#x <- %#x\n", pg.dma_semaphore, d.base, pg.semaphore_offset, data);
        else if (gpu_trace) fprintf(stderr, "[gpu] semaphore release with no DMA object\n");
        break;
    }
    case 0x200: pg.clip_x = data & 0xFFFF; pg.clip_w = data >> 16; break;           /* SET_SURFACE_CLIP_HORIZONTAL */
    case 0x204: pg.clip_y = data & 0xFFFF; pg.clip_h = data >> 16; break;           /* SET_SURFACE_CLIP_VERTICAL */
    case 0x208: pg.surface_format = data; break;
    case 0x20c: pg.surface_pitch_color = data & 0xFFFF; pg.surface_pitch_zeta = data >> 16; break;
    case 0x210: pg.color_offset = data; break;
    case 0x214: pg.zeta_offset = data; break;
    case 0x300: pg.psh.alpha_test = data; break;
    case 0x33c: pg.psh.alpha_func = data; break;
    case 0x340: pg.psh.alpha_ref = data & 0xFF; break;
    case 0x288: pg.psh.final0 = data; break;
    case 0x28c: pg.psh.final1 = data; break;
    case 0x1e60: pg.psh.control = data; break;
    case 0x1e70: pg.psh.stage_program = data; break;
    case 0x1e78: pg.psh.other_stage_input = data; break;
    case 0x2a4: pg.psh.fog_enable = data; break;
    case 0x2a8: pg.psh.fog_color = data; break;
    case 0x1e20: pg.psh.final_factor0 = data; break;
    case 0x1e24: pg.psh.final_factor1 = data; break;
    case 0x17fc:                                                           /* SET_BEGIN_END */
        if (data) { pg.prim = data; pg.n_inl = pg.n_elems = pg.n_da = pg.n_imm = 0; }
        else { if (pg.prim) flush_draw(); pg.prim = 0; }
        break;
    case 0x1818: push_u32(&pg.inl, &pg.n_inl, &pg.cap_inl, data); break;             /* INLINE_ARRAY */
    case 0x1800: push_u32(&pg.elems, &pg.n_elems, &pg.cap_elems, data & 0xFFFF); push_u32(&pg.elems, &pg.n_elems, &pg.cap_elems, data >> 16); break;
    case 0x1808: push_u32(&pg.elems, &pg.n_elems, &pg.cap_elems, data); break;       /* ARRAY_ELEMENT32 */
    case 0x1810:                                                                      /* DRAW_ARRAYS */
        if (pg.n_da < 256) { pg.da_start[pg.n_da] = data & 0xFFFFFF; pg.da_count[pg.n_da] = (data >> 24) + 1; pg.n_da++; }
        break;
    case 0x394: pg.clip_min = f32(data); break;
    case 0x398: pg.clip_max = f32(data); break;
    case 0x30c: pg.depth_enable = data; break;
    case 0x304: pg.blend_enable = data; break;
    case 0x308: pg.cull_enable = data; break;
    case 0x344: pg.blend_src = data; break;
    case 0x348: pg.blend_dst = data; break;
    case 0x34c: pg.blend_color = data; break;
    case 0x350: pg.blend_eq = data; break;
    case 0x354: pg.depth_func = data; break;
    case 0x358: pg.color_mask = data; break;
    case 0x35c: pg.depth_write = data; break;
    case 0x290: pg.stencil_write = data & 1; break;                                   /* SET_CONTROL0: STENCIL_WRITE_ENABLE */
    case 0x32c: pg.stencil_enable = data; break;
    case 0x360: pg.stencil_wmask = data & 0xFF; break;
    case 0x364: pg.stencil_func = data; break;
    case 0x368: pg.stencil_ref = data & 0xFF; break;
    case 0x36c: pg.stencil_rmask = data & 0xFF; break;
    case 0x370: pg.stencil_fail = data; break;
    case 0x374: pg.stencil_zfail = data; break;
    case 0x378: pg.stencil_zpass = data; break;
    case 0x39c: pg.cull_face = data; break;
    case 0x37c: pg.shade_mode = data; break;
    case 0x338: pg.poly_off_fill = data; break;                             /* SET_POLY_OFFSET_FILL_ENABLE */
    case 0x384: pg.poly_off_scale = f32(data); break;
    case 0x388: pg.poly_off_bias = f32(data); break;
    case 0x38c: pg.poly_front = data; break;
    case 0x390: pg.poly_back = data; break;
    case 0x3a0: pg.front_face = data; break;
    case 0x1e9c: pg.prog_ld = data; break;
    case 0x1ea0: pg.prog_start = data; break;
    case 0x1ea4: pg.const_ld = data; break;
    case 0x1d8c: pg.clear_zs = data; break;
    case 0x1d90: pg.clear_color = data; break;
    case 0x1d98: pg.clear_rect_h = data; break;
    case 0x1d9c: pg.clear_rect_v = data; break;
    case 0x1d94: clear_surface(data); break;
    default:
        if (method >= 0xb00 && method < 0xb80) {                           /* SET_TRANSFORM_PROGRAM: 4 words per slot */
            uint32_t slot = (method - 0xb00) / 4;
            if (pg.prog_ld < 136) pg.prog[pg.prog_ld * 4 + slot % 4] = data;
            if (slot % 4 == 3) pg.prog_ld++;
        } else if (method >= 0xb80 && method < 0xc00) {                    /* SET_TRANSFORM_CONSTANT */
            uint32_t slot = (method - 0xb80) / 4;
            if (pg.const_ld < 192) pg.consts[pg.const_ld * 4 + slot % 4] = f32(data);
            if (slot % 4 == 3) pg.const_ld++;
        } else if (method >= 0x260 && method < 0x280) { pg.psh.alpha_icw[(method - 0x260) / 4] = data;
        } else if (method >= 0xa60 && method < 0xa80) { pg.psh.factor0[(method - 0xa60) / 4] = data;
        } else if (method >= 0xa80 && method < 0xaa0) { pg.psh.factor1[(method - 0xa80) / 4] = data;
        } else if (method >= 0xaa0 && method < 0xac0) { pg.psh.alpha_ocw[(method - 0xaa0) / 4] = data;
        } else if (method >= 0xac0 && method < 0xae0) { pg.psh.rgb_icw[(method - 0xac0) / 4] = data;
        } else if (method >= 0x1e40 && method < 0x1e60) { pg.psh.rgb_ocw[(method - 0x1e40) / 4] = data;
        } else if (method >= 0x1720 && method < 0x1760) {                  /* VERTEX_DATA_ARRAY_OFFSET */
            int i = (method - 0x1720) / 4;
            pg.va[i].dma_b = (data >> 31) & 1; pg.va[i].offset = data & 0x7FFFFFFF;
        } else if (method >= 0x1760 && method < 0x17a0) {                  /* VERTEX_DATA_ARRAY_FORMAT */
            int i = (method - 0x1760) / 4;
            pg.va[i].type = data & 0xF; pg.va[i].count = (data >> 4) & 0xF; pg.va[i].stride = data >> 8;
        } else if (method >= 0x1880 && method < 0x1900) {                  /* SET_VERTEX_DATA2F_M: 2 words per attribute */
            int i = (method - 0x1880) / 8, p = ((method - 0x1880) / 4) & 1;
            pg.cur[i][p] = f32(data); pg.cur[i][2] = 0; pg.cur[i][3] = 1;
            if (i == 0 && p == 1) immediate_vertex();
        } else if (method >= 0x1a00 && method < 0x1b00) {                  /* SET_VERTEX_DATA4F_M */
            int i = (method - 0x1a00) / 16, p = ((method - 0x1a00) / 4) & 3;
            pg.cur[i][p] = f32(data);
            if (i == 0 && p == 3) immediate_vertex();
        } else if (method >= 0x1940 && method < 0x1980) {                  /* SET_VERTEX_DATA4UB: BGRA bytes -> RGBA floats */
            int i = (method - 0x1940) / 4;
            pg.cur[i][0] = ((data >> 16) & 255) / 255.f; pg.cur[i][1] = ((data >> 8) & 255) / 255.f;
            pg.cur[i][2] = (data & 255) / 255.f; pg.cur[i][3] = (data >> 24) / 255.f;
            if (i == 0) immediate_vertex();
        } else if (method >= 0x1b00 && method < 0x1c00) {                  /* textures: 64-byte stride per stage */
            int i = (method - 0x1b00) / 64;
            if (i < 4) switch (method & 63) {
            case 0x00: pg.tex[i].offset = data; break;
            case 0x04: pg.tex[i].format = data; break;
            case 0x08: pg.tex[i].address = data; break;
            case 0x0c: pg.tex[i].control0 = data; break;
            case 0x10: pg.tex[i].control1 = data; break;
            case 0x14: pg.tex[i].filter = data; break;
            case 0x1c: pg.tex[i].rect = data; break;
            case 0x28: case 0x2c: case 0x30: case 0x34: pg.psh.bump_mat[i][((method & 63) - 0x28) / 4] = data; break;   /* BUMP_ENV_MAT */
            case 0x38: pg.psh.bump_scale[i] = data; break;
            case 0x3c: pg.psh.bump_offset[i] = data; break;
            default: break;
            }
        }
        break;

    }
}

/* Render targets seen so far, so the presenter can find the geometry of the surface it is asked to show. */
static struct { uint32_t addr, w, h, pitch; } seen_surfaces[32];
static void note_surface(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch) {
    for (int i = 0; i < 32; i++)
        if (seen_surfaces[i].addr == addr || !seen_surfaces[i].addr) { seen_surfaces[i].addr = addr; seen_surfaces[i].w = w; seen_surfaces[i].h = h; seen_surfaces[i].pitch = pitch; return; }
}
int nv2a_surface_nth(int i, uint32_t *addr, uint32_t *w, uint32_t *h, uint32_t *pitch) {
    if (i < 0 || i >= 32 || !seen_surfaces[i].addr) return 0;
    *addr = seen_surfaces[i].addr; *w = seen_surfaces[i].w; *h = seen_surfaces[i].h; *pitch = seen_surfaces[i].pitch;
    return 1;
}
int nv2a_surface_at(uint32_t addr, uint32_t *w, uint32_t *h, uint32_t *pitch) {
    for (int i = 0; i < 32; i++)
        if (seen_surfaces[i].addr == addr) { *w = seen_surfaces[i].w; *h = seen_surfaces[i].h; *pitch = seen_surfaces[i].pitch; return 1; }
    return 0;
}

/* Methods are delivered per subchannel; method 0 binds an object (handle) to the subchannel. */
static void nv_method(uint32_t subch, uint32_t method, uint32_t data) {
    if (method == 0) {
        pg.subch_class[subch] = object_class(data);
        pg.bound[subch] = data;
        if (gpu_trace) fprintf(stderr, "[gpu] sub%u = object %#x class %#x\n", subch, data, pg.subch_class[subch]);
        return;
    }
    if (gpu_trace > 1) fprintf(stderr, "[gpu] sub%u(c%02x) m%04x = %08x\n", subch, pg.subch_class[subch], method, data);
    pg.cur_sub = subch;
    if (pg.subch_class[subch] == CLS_KELVIN) kelvin_method(method, data);
    else if (pg.subch_class[subch] == 0x44) {                /* pattern object: SET_COLOR0/1 land in PGRAPH_PATT_COLOR0/1,
                                                              * which the driver uses as a GPU-progress marker it polls */
        if (method == 0x310) gpu_fence(REG(0x400B10), data);                     /* the driver's fence counter: it is released once the GPU has finished what precedes it */
        else if (method == 0x314) ST32RAW(REG(0x400B14), data);
    }
}

/* Consume push buffer words in [get, put). Returns the new get. Headers (NV_PFIFO_CACHE1 pusher):
 *   0x20000000|addr      old-style jump          addr|1   jump      addr|2   call      0x00020000  return
 *   (count<<18)|(sub<<13)|method           increasing run
 *   0x40000000|(count<<18)|(sub<<13)|method  non-increasing run */
static struct { uint32_t rem, method, sub; int inc; } run;           /* method run in progress */
void kobj_dump_waits(void);
static unsigned mid_put, mid_stall;                                  /* STW_FPS: calls that ended in the middle of a run */
/* Wait slots. After every fence (marker + semaphore release) the driver leaves a reserved pair of dummy commands. To wait for a fence
 * with an interrupt instead of polling, it overwrites that pair with WAIT_FOR_IDLE and a software-method NOP (0x40100, n), but only when
 * its view of GET says the GPU is still well before the slot. The real GPU is far slower than this pusher, so a pusher that has already
 * run past the slot while the guest thread was descheduled never executes the patch and the interrupt the driver sleeps on is lost.
 * The watcher notices a patch in a slot that was already consumed and delivers the interrupt the GPU would have raised (the fence the
 * driver waits for is complete: the pusher is past it). STW_NOSLOTWATCH=1 disables it. */
#define NSLOT 128
static struct { uint32_t addr, fp[3]; uint64_t t_rec, t_seen; int state; } wslot[NSLOT];   /* fp: the fence words in front of the slot, to tell the same lap from a reused ring; state 0 free, 1 watching, 2 patch seen */
static uint32_t wslot_n;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void slot_record(uint32_t r) {
    if (LD32(r) != 0x00041d90u || LD32(r + 8) != 0x00041d90u) return;      /* not an untouched reserved pair */
    uint32_t i = wslot_n++ % NSLOT;
    wslot[i].addr = r; wslot[i].t_rec = mono_ns(); wslot[i].t_seen = 0; wslot[i].state = 1;
    wslot[i].fp[0] = LD32(r - 4); wslot[i].fp[1] = LD32(r - 12); wslot[i].fp[2] = LD32(r - 16);
}
static void slot_watch(void) {
    static int off = -1;
    if (off < 0) off = getenv("STW_NOSLOTWATCH") != NULL;
    if (off) return;
    uint64_t now = 0;
    uint32_t get = LD32(NV2A_USER + 0x44);
    for (int i = 0; i < NSLOT; i++) {
        if (!wslot[i].state) continue;
        uint32_t r = wslot[i].addr;
        uint32_t hdr = LD32(r + 8), data = LD32(r + 12);
        if (!now) now = mono_ns();
        if (now - wslot[i].t_rec > 50000000ull || LD32(r - 4) != wslot[i].fp[0] || LD32(r - 12) != wslot[i].fp[1] || LD32(r - 16) != wslot[i].fp[2]) { wslot[i].state = 0; continue; }   /* the driver wrapped and wrote new commands over this fence: a patch now is for the next lap, ahead of the pusher */
        if (hdr != 0x00040100u || data != 5) { wslot[i].state = 1; continue; }   /* untouched, taken back by the driver, or some other software method */
        if (now - wslot[i].t_rec < 1000000ull || (get >= r && get < r + 0x10)) continue;   /* the pusher may not have consumed it yet */
        if (wslot[i].state == 1) { wslot[i].state = 2; wslot[i].t_seen = now; continue; }
        if (now - wslot[i].t_seen < 300000ull) continue;                       /* the driver undoes a patch it no longer needs within microseconds */
        if (pusher_stalled || (LD32(REG(0x400100)) & (1u << 20))) continue;    /* the interrupt registers are busy: try again */
        gpu_sync_fences();
        fprintf(stderr, "stw: wait slot %#x patched after the pusher passed it (data %#x): delivering the missed interrupt\n", r, data);
        ST32RAW(REG(0x400704), 0x100);                                         /* TRAPPED_ADDR: subchannel 0, NO_OPERATION */
        ST32RAW(REG(0x400708), data);
        ST32RAW(REG(0x400108), 1);
        ST32RAW(REG(0x400100), LD32(REG(0x400100)) | (1u << 20));
        swm_note(get, data, 3);
        pusher_stalled = 1;
        nv2a_update_pmc();
        wslot[i].state = 0;
    }
}

static uint32_t pusher_run(uint32_t get, uint32_t put) {
    static uint32_t ret_addr;
    int guard = 0;
    while (get != put && !pusher_stalled && !flip_blocked() && guard++ < (1 << 22)) {
        if (get >= 0x04000000u) {                                       /* a jump left the 64 MB of RAM: garbage in the push buffer; drop what is queued */
            fprintf(stderr, "stw: push buffer jump to %#x (put %#x); discarding\n", get, put);
            return put;
        }
        if (run.rem) {                                                  /* the rest of a method run that an earlier call had to leave (PUT or a stall hit mid-run) */
            while (run.rem && get != put && !pusher_stalled) {
                nv_method(run.sub, run.method, LD32(get));
                get += 4;
                if (run.method == 0x1d70) slot_record(get);                       /* the reserved pair that follows a fence */
                if (run.inc) run.method += 4;
                run.rem--;
            }
            continue;
        }
        /* Publish DMA_GET at every command boundary, like the hardware's. The driver patches reserved NOP slots ahead of the GPU
         * (turning them into WAIT_FOR_IDLE + software-method interrupts) when its view of GET says the GPU is still far behind; a
         * GET that only moves when this call returns lets a fast pusher run past slots the driver still thinks are ahead of it,
         * and the interrupt a waiting thread needs is then never raised. */
        __atomic_store_n((uint32_t *)(G + NV2A_USER + 0x44), get, __ATOMIC_RELEASE);
        uint32_t w = LD32(get);
        get += 4;
        if ((w & 0xE0000003u) == 0x20000000u) { get = w & 0x1FFFFFFFu; continue; }
        if ((w & 3) == 1) { get = w & ~3u; continue; }
        if ((w & 3) == 2) { ret_addr = get; get = w & ~3u; continue; }
        if (w == 0x00020000u) { get = ret_addr; continue; }
        uint32_t method = w & 0x1FFC, sub = (w >> 13) & 7, count = (w >> 18) & 0x7FF;
        int inc = (w & 0xE0030003u) == 0;
        if (!inc && (w & 0xE0030003u) != 0x40000000u) {
            if (gpu_trace) fprintf(stderr, "[gpu] bad header %08x at %08x\n", w, get - 4);
            continue;
        }
        run.rem = count; run.method = method; run.sub = sub; run.inc = inc;     /* consumed at the top of the loop, and resumable across calls like the hardware's */
        if (!count) continue;
    }
    if (run.rem) { if (get == put) mid_put++; else mid_stall++; }
    return get;
}

static void *device_thread(void *arg) {
    (void)arg;
    gpu_trace = getenv("STW_GPUTRACE") ? atoi(getenv("STW_GPUTRACE")) : 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        uint64_t ns = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000ull + (t.tv_nsec - t0.tv_nsec);
        /* PTIMER_TIME_0/1: free-running counter, 27 MHz-ish, low bits are in 0x9400 shifted by 5 */
        uint64_t tm = (ns * 27 / 1000) << 5;
        ST32RAW(REG(0x9400), (uint32_t)tm);
        ST32RAW(REG(0x9410), (uint32_t)(tm >> 32));
        /* PFB write-buffer flush: the driver sets bit 16 and waits for the hardware to clear it */
        /* channel 0 DMA: the driver advances DMA_PUT; the "GPU" consumes up to it and publishes DMA_GET */
        uint32_t put = LD32(NV2A_USER + 0x40), get = LD32(NV2A_USER + 0x44);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);                /* see the command words written before PUT */
        if (put != get) {
            static int first = 1;
            if (first) {
                first = 0;
                if (gpu_trace) fprintf(stderr, "[gpu] first kick: put=%08x get=%08x  CACHE1 put=%08x get=%08x inst=%08x\n",
                                       put, get, LD32(REG(0x3240)), LD32(REG(0x3244)), LD32(REG(0x3248)));
            }
            ST32RAW(NV2A_USER + 0x44, pusher_run(get, put));
        }
        slot_watch();
        {   /* debugging aid: STW_WATCH=<guest address> reports when a guest word changes, with every thread's recent calls */
            static uint32_t watch, last; static int init;
            if (!init) { init = 1; watch = getenv("STW_WATCH") ? (uint32_t)strtoul(getenv("STW_WATCH"), NULL, 16) : 0; last = watch ? LD32(watch) : 0; }
            if (watch && LD32(watch) != last) {
                fprintf(stderr, "stw: watched word %#x changed %#x -> %#x\n", watch, last, LD32(watch));
                last = LD32(watch);
                rt_dump_rings();
            }
        }
        if (getenv("STW_METHODS")) {
            static struct timespec lm; if (!lm.tv_sec) lm = t;
            if (t.tv_sec - lm.tv_sec >= 30) {
                lm = t;
                fprintf(stderr, "[methods]");
                for (int i = 0; i < 0x800; i++) if (meth_cnt[i]) fprintf(stderr, " %x:%u:%x", i * 4, meth_cnt[i], meth_last[i]);
                fprintf(stderr, "\n");
            }
        }
        if (getenv("STW_FPS")) {
            static struct timespec last; static uint32_t f0, d0;
            if (!last.tv_sec) last = t;
            if (t.tv_sec - last.tv_sec >= 1) {
                struct rusage ru; getrusage(RUSAGE_SELF, &ru);
                double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
                static double cpu0; static struct timespec t0w;
                double wall = (t.tv_sec - last.tv_sec) + (t.tv_nsec - last.tv_nsec) * 1e-9; (void)t0w;
                extern uint64_t gpu_wait_n[8], gpu_wait_ns[8];
                static uint64_t wn0[8], wt0[8];
                char wb[160]; int wp = 0;
                for (int i = 0; i < 4; i++) { wp += snprintf(wb + wp, sizeof wb - wp, " w%d=%llux/%.0fms", i, (unsigned long long)(gpu_wait_n[i] - wn0[i]), (gpu_wait_ns[i] - wt0[i]) / 1e6); wn0[i] = gpu_wait_n[i]; wt0[i] = gpu_wait_ns[i]; }
                fprintf(stderr, "[fps] %u frames, %u draws /s scene3d=%d cpu=%.0f%%%s mid=%u/%u\n", stat_frames - f0, stat_draws - d0, scene3d, (cpu - cpu0) / wall * 100, wb, mid_put, mid_stall);
                cpu0 = cpu;
                if (getenv("STW_HEAPLOG")) {
                    extern void guest_heap_stats(unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *);
                    unsigned long long u, pm, pk, na, nf;
                    guest_heap_stats(&u, &pm, &pk, &na, &nf);
                    fprintf(stderr, "[heap] in use %llu KB + permanent %llu KB (peak %llu KB), %llu allocs / %llu frees\n", u >> 10, pm >> 10, pk >> 10, na, nf);
                }
                if (getenv("STW_STALLDUMP") && stat_frames - f0 <= 1) { fprintf(stderr, "[stall] recent guest calls per thread:\n"); rt_dump_rings(); kobj_dump_waits(); }
                f0 = stat_frames; d0 = stat_draws; last = t;
            }
        }
        usleep(20);
    }
    return NULL;
}

/* PMC_INTR_0 is the OR of the engines' pending status: PBUS 28, PCRTC 24, PTIMER 20, PGRAPH 12, PFIFO 8. */
void kobj_poke_interrupt(void);
void nv2a_update_pmc(void) {
    static const struct { uint32_t reg; int bit; } src[] = {
        { 0x001100, 28 }, { 0x600100, 24 }, { 0x009100, 20 }, { 0x400100, 12 }, { 0x002100, 8 } };
    uint32_t v = LD32(REG(0x100)) & (1u << 31);
    for (int i = 0; i < 5; i++) if (LD32(REG(src[i].reg))) v |= 1u << src[i].bit;
    ST32RAW(REG(0x100), v);
    if (v) kobj_poke_interrupt();                      /* something is pending: let the ISR run without delay */
}

/* Register writes with side effects. Offsets are relative to NV2A_BASE. */
void mmio_write(uint32_t addr, int size, uint64_t value) {
    uint32_t off = addr - NV2A_BASE, v = (uint32_t)value;
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_MMIOTRACE") != NULL;
    if (trace && off != 0x800040) fprintf(stderr, "[mmio] w%d %06x <- %08x\n", size * 8, off, v);
    if (size != 4) {                                   /* sub-word writes: merge into the existing word */
        uint32_t cur = LD32(addr & ~3u), sh = (addr & 3) * 8, m = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        v = (cur & ~m) | ((v << sh) & m);
        addr &= ~3u;
        off = addr - NV2A_BASE;
    }
    if (off == 0x800040) __atomic_thread_fence(__ATOMIC_RELEASE);   /* command words must be visible before PUT */
    switch (off) {
    case 0x000100:                                     /* PMC_INTR_0: read-only summary of the engines' status */
        return;
    case 0x001100: case 0x002100: case 0x009100: case 0x400100: case 0x600100:
        /* PBUS / PFIFO / PTIMER / PGRAPH / PCRTC INTR_0: write 1 to clear */
        ST32RAW(addr, LD32(addr) & ~v);
        if (off == 0x400100 && !(LD32(addr) & (1u << 20))) { if (pusher_stalled) swm_note(LD32(NV2A_USER + 0x44), 0, 2); pusher_stalled = 0; }      /* the ISR acknowledged the software method */
        nv2a_update_pmc();
        return;
    case 0x003214: case 0x002400:                      /* PFIFO CACHE1 / RUNOUT status: hardware-owned */
        return;
    case 0x003220:                                     /* CACHE1_DMA_PUSH: bit 4 (STATE) is hardware status; the pusher is never busy for the CPU */
        v &= ~0x10u;
        break;
    case 0x40071c:                                     /* PGRAPH_INCREMENT: bit 1 = INCREMENT_READ_3D (the flip DPC consumed a flip) */
        if (v & 2) __atomic_add_fetch(&flip_read, 1, __ATOMIC_RELEASE);
        break;
    case 0x600800:                                     /* PCRTC_START: the scanout address changed (a flip) */
        ST32RAW(addr, v);
        display_set_start(v);
        return;
    case 0x100410:                                     /* PFB write-buffer flush completes instantly */
        v &= ~0x10000u;
        break;
    default: break;
    }
    ST32RAW(addr, v);
}

void nv2a_start(void) {
    pg.color_mask = 0x01010101;                         /* reset state: all colour channels writable */
    pg.depth_write = 1;
    pg.stencil_func = 0x207; pg.stencil_rmask = pg.stencil_wmask = 0xFF; pg.stencil_fail = pg.stencil_zfail = pg.stencil_zpass = 0x1E00; pg.stencil_write = 1;
    ST32RAW(REG(0x000000), 0x02A000A1);                 /* PMC_BOOT_0: NV2A, revision A1 */
    ST32RAW(REG(0x10020C), 0x04000000);                 /* PFB_CSTATUS: 64 MB of RAM */
    ST32RAW(REG(0x003214), 0x10);                       /* CACHE1 empty */
    ST32RAW(REG(0x002400), 0x10);                       /* runout empty */
    pthread_t th;
    pthread_create(&th, NULL, device_thread, NULL);
    pthread_detach(th);
}
