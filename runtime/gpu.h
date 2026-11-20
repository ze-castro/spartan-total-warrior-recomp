/* Interface between the NV2A model (nv2a.c) and the host renderer (gpu_metal.m). */
#ifndef STW_GPU_H
#define STW_GPU_H
#include <stdint.h>

typedef struct {
    int enabled;
    uint32_t addr;                 /* physical address of the base level */
    uint32_t format;               /* NV097_SET_TEXTURE_FORMAT_COLOR_* */
    uint32_t width, height;
    uint32_t pitch;                /* linear formats only */
    int swizzled;                  /* Morton-order storage (SZ_ formats) */
    int normalized;                /* texcoords are 0..1 (swizzled/DXT) vs texel units (linear "image" formats) */
    uint32_t mip_levels;           /* levels stored after the base level (SET_TEXTURE_FORMAT bits 16..19), swizzled/DXT only */
    int cube;                      /* SET_TEXTURE_FORMAT bit 2 */
    uint32_t filter;               /* raw SET_TEXTURE_FILTER */
    uint32_t address;              /* raw SET_TEXTURE_ADDRESS (wrap modes) */
} GpuTex;

/* Register-combiner state (NV097_SET_COMBINER_*, SET_SHADER_STAGE_PROGRAM ...), raw hardware words. */
typedef struct {
    uint32_t control;                       /* low byte = number of stages, bits 8.. = flags (mux msb, unique c0/c1) */
    uint32_t rgb_icw[8], rgb_ocw[8], alpha_icw[8], alpha_ocw[8];
    uint32_t factor0[8], factor1[8];        /* per-stage constants c0 / c1 (ARGB) */
    uint32_t final0, final1;                /* SET_COMBINER_SPECULAR_FOG_CW0/1 */
    uint32_t final_factor0, final_factor1;  /* constants of the final combiner */
    uint32_t stage_program;                 /* 5 bits per texture stage: PS_TEXTUREMODES_* */
    uint32_t fog_color;                     /* ARGB-ish packed as written to SET_FOG_COLOR */
    uint32_t fog_enable;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t other_stage_input;             /* SET_SHADER_OTHER_STAGE_INPUT: dot-product mapping + which stage feeds stages 2/3 */
    uint32_t bump_mat[4][4];                /* per stage, method words in order (m00, m01, m11, m10), floats as bits */
    uint32_t bump_scale[4], bump_offset[4];
} PshState;

typedef struct {
    /* render target (physical addresses, 32-bit colour) */
    uint32_t color_addr, color_pitch, width, height, zeta_addr;
    uint32_t surface_format, clip_x, clip_y, clip_w, clip_h;      /* clip = drawable window, in samples */
    /* geometry: a triangle list of `nverts` indices into `nuniq` vertices of `vstride` float4 each. Vertex attribute i is
     * stored in the vertex when bit i of attr_mask is set (at the slot given by the number of lower set bits); otherwise
     * it is the draw-wide constant cur[i]. */
    const float *verts;
    const uint32_t *indices;
    uint32_t nverts, nuniq, vstride, attr_mask;
    float cur[16][4];
    /* transform program and constants */
    const uint32_t *vsh;           /* 136 slots * 4 words */
    int vsh_start;
    const float *consts;           /* 192 float4 */
    float clip_min, clip_max;
    /* fixed-function-ish state */
    uint32_t depth_enable, depth_func, depth_write;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_rmask, stencil_wmask;   /* wmask already folded with CONTROL0.STENCIL_WRITE_ENABLE */
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;                                 /* GL-style op enums as the NV2A methods carry them */
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t color_mask;
    uint32_t shade_mode, poly_off_fill;
    float poly_off_scale, poly_off_bias;
    GpuTex tex[4];
    PshState psh;
} GpuDraw;

void gpu_draw(const GpuDraw *d);
void gpu_clear_depth(uint32_t zeta_addr, uint32_t width, uint32_t height, float depth, uint32_t stencil, int mask);   /* mask: bit 0 depth, bit 1 stencil */
void gpu_fence(uint32_t addr, uint32_t value);   /* store `value` at guest `addr` once all GPU work submitted so far has finished */
void gpu_sync_fences(void);        /* gpu_sync, but only when a fence is still outstanding */
int gpu_clear_color(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint32_t argb);   /* whole-surface clear on the GPU; 0 = not possible */
void gpu_sync(void);               /* wait until all submitted GPU work has finished writing guest memory (and make the 1x surfaces current) */
void gpu_wait_idle(void);          /* wait for the GPU only; the scaled textures are consumed directly */
int gpu_clear_color_rect(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t argb);
void gpu_cpu_wrote(uint32_t addr, uint32_t bytes);   /* the CPU changed guest memory that may be a render target */
void gpu_set_bloomhq(int on);
void gpu_set_msaa(int n);          /* debug: MSAA sample count of the scene target at run time (1 = off) */
int gpu_res(void);                 /* current internal resolution preset: 0 original (480 lines), 1 720p, 2 1080p, 3 1440p, 4 2160p */
void gpu_set_res(int idx);
void *gpu_scanout_texture(uint32_t addr, uint32_t w, uint32_t h, uint32_t *tw, uint32_t *th);   /* hi texture of a surface (id<MTLTexture> as void*), NULL if none */
void *gpu_read_hi(uint32_t addr, uint32_t *tw, uint32_t *th);                                    /* malloc'd BGRA copy of the hi texture, for dumps */

char *vsh_to_msl(const uint32_t *prog, int start, int nslots);
char *psh_to_msl(const PshState *s);
/* Constant buffer read by the generated fragment function (layout mirrored in the MSL source). */
typedef struct {
    float c0[9][4], c1[9][4];               /* per-stage constants, [8] = final combiner */
    float fog[4];
    float tex_size[4][4];
    uint32_t tex_flags[4][4];               /* x = texcoords are normalised, y = sample this stage */
    float bump_mat[4][4];                   /* m00, m01, m10, m11 per stage */
    float bump_scale[4][4], bump_offset[4][4];   /* [stage][0] used */
    uint32_t alpha_ref, pad[3];
} PshConsts;
#endif
