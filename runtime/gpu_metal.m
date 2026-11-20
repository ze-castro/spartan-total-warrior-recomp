/* Metal renderer for the NV2A model. Render targets are textures viewing guest RAM directly (the RAM
 * is one shared mapping, wrapped as an MTLBuffer), so the CPU, the presenter and the GPU all see the
 * same pixels. Vertex programs are translated to MSL (vsh.c); the pixel stage is a fixed modulate for now. */
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu.h"
#include "rt.h"
#include "input.h"
#include "paths.h"

#define PHYS_RAM 0x10000000u

/* Internal resolution. Every colour target the GPU draws to has a "hi" texture gscale times larger in each direction; draws go
 * to the hi texture, post-process passes sample it, and the presenter shows it directly. The 1x view of guest RAM is only
 * brought up to date ("resolved", a downsample pass) when the CPU can look at it: gpu_sync, snapshots of render-target memory
 * as textures, dumps and the video overlay. STW_SCALE=1..4 sets the factor (default 2), F2 cycles it at run time. */
static int gscale = 9;                                      /* internal scale in quarters: 4 = 1x (480 lines), 6 = 720p, 9 = 1080p, 12 = 1440p, 18 = 2160p */
static const int res_quarters[5] = { 4, 6, 9, 12, 18 };    /* "Original", 720p, 1080p, 1440p, 2160p: 480 lines * quarters / 4 */
#define SCL(x) ((((x) * gscale) + 2) / 4)                   /* a guest size or coordinate at the internal scale (exact for the sizes the game uses) */

/* Quality extras (launcher "Quality"; STW_* variables override the saved settings for testing). */
static int q_bloomhq;                                       /* smooth upsampling of low-resolution render targets sampled by a larger pass (bloom) */
static int gmsaa = 1;                                       /* MSAA sample count of the main scene target: 1 (off), 2 or 4 */

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLBuffer> ram;
static NSMutableDictionary<NSNumber *, id> *pso_cache, *depth_cache, *tex_cache, *sampler_cache, *view_cache, *ds_cache;
static id<MTLTexture> white_tex;
static id<MTLRenderPipelineState> util_copy_pso, util_fill_pso, util_copy_ms_pso;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;

/* Submission state (all guarded by gpu_lock). Draws are batched into one open command buffer and, while the render
 * target stays the same, one open encoder; everything is flushed at sync points (CPU access to guest RAM, fences,
 * vblank), after enough draws, or when the vertex ring wraps. */
static id<MTLCommandBuffer> cb, last_cb;
static id<MTLRenderCommandEncoder> enc;
static id<MTLTexture> enc_color, enc_depth;
static int draws_in_cb, force_new_encoder;
static id<MTLBuffer> vring;
static size_t vring_off;

/* Fences (BACK_END_WRITE_SEMAPHORE_RELEASE and the driver's PGRAPH marker). The value is stored when the command buffer holding
 * everything submitted before it has completed, so the pusher never waits; fences are written in submission order. With
 * STW_SYNCFENCE=1 the pusher waits for the GPU and then stores the value instead. */
typedef struct { uint64_t seq; uint32_t addr, value; } Fence;
static Fence fences[4096];
static int nfences;
static pthread_mutex_t fence_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t cb_seq;                                     /* sequence number of the open command buffer */
static int cb_has_fence;
static void fence_drain(uint64_t upto) {
    pthread_mutex_lock(&fence_lock);
    int i = 0;
    for (; i < nfences && fences[i].seq <= upto; i++) __atomic_store_n((uint32_t *)(G + fences[i].addr), fences[i].value, __ATOMIC_RELEASE);
    memmove(fences, fences + i, (size_t)(nfences - i) * sizeof *fences);
    nfences -= i;
    pthread_mutex_unlock(&fence_lock);
}
#define VRING_SIZE (64u << 20)

/* Render targets seen so far (views of guest RAM), so textures can sample them directly. */
typedef struct { uint32_t addr, pitch, w, h; id<MTLTexture> view, hi, ms; int written, dirty, stale, scale, ms_stale; } Target;   /* ms: multisampled twin of hi (MSAA), ms_stale: hi changed behind its back */   /* dirty: hi is newer than the 1x view; stale: the CPU changed the 1x memory */
static Target targets[64];
static int ntargets;

static uint64_t h64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t v; memcpy(&v, b, 8); h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
    for (; n; n--, b++) h = (h ^ *b) * 1099511628211ull;
    return h;
}
/* Content hash of guest memory for texture change detection: four independent lanes so it runs at memory speed. */
static uint64_t hash_mem(const uint8_t *p, size_t n) {
    uint64_t h0 = 1469598103934665603ull, h1 = h0 ^ 0x9E3779B97F4A7C15ull, h2 = h0 ^ 0xC2B2AE3D27D4EB4Full, h3 = h0 ^ 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); memcpy(&c, p + i + 16, 8); memcpy(&d, p + i + 24, 8);
        h0 = (h0 ^ a) * 1099511628211ull; h1 = (h1 ^ b) * 1099511628211ull; h2 = (h2 ^ c) * 1099511628211ull; h3 = (h3 ^ d) * 1099511628211ull;
        h0 ^= h0 >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29;
    }
    return h64(p + i, n - i, h0 ^ (h1 * 31) ^ (h2 * 131) ^ (h3 * 1031));
}
static uint64_t h64w(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull * 0x9E3779B97F4A7C15ull + (v >> 17); }

static id<MTLLibrary> util_lib;
static int util_ms_count;
/* hi -> multisampled copy pipeline, used to start an MSAA target from the current image; rebuilt when the sample count changes */
static void make_ms_pso(void) {
    if (gmsaa <= 1 || util_ms_count == gmsaa) return;
    NSError *err = nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [util_lib newFunctionWithName:@"u_vs"];
    pd.fragmentFunction = [util_lib newFunctionWithName:@"u_copy"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.rasterSampleCount = gmsaa;
    util_copy_ms_pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!util_copy_ms_pso) { fprintf(stderr, "stw: MSAA unavailable (%s)\n", err.localizedDescription.UTF8String); gmsaa = 1; return; }
    util_ms_count = gmsaa;
}
static int gpu_ready(void) {
    if (dev) return 1;
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) return 0;
    queue = [dev newCommandQueue];
    ram = [dev newBufferWithBytesNoCopy:G length:PHYS_RAM options:MTLResourceStorageModeShared deallocator:nil];
    if (!ram) { fprintf(stderr, "stw: cannot wrap guest RAM as a Metal buffer\n"); dev = nil; return 0; }
    pso_cache = [NSMutableDictionary new]; depth_cache = [NSMutableDictionary new];
    tex_cache = [NSMutableDictionary new]; sampler_cache = [NSMutableDictionary new];
    view_cache = [NSMutableDictionary new]; ds_cache = [NSMutableDictionary new];
    vring = [dev newBufferWithLength:VRING_SIZE options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    white_tex = [dev newTextureWithDescriptor:td];
    uint32_t w = 0xFFFFFFFFu;
    [white_tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
    if (getenv("STW_RES")) { int v = atoi(getenv("STW_RES")); if (v >= 0 && v <= 4) gscale = res_quarters[v]; }              /* 0 original, 1 720p, 2 1080p, 3 1440p, 4 2160p */
    else if (getenv("STW_SCALE")) { int v = atoi(getenv("STW_SCALE")); if (v >= 1 && v <= 4) gscale = 4 * v; }           /* legacy: whole multiples of 480 lines */
    {
        StwSettings qs = stw_settings_get();
        q_bloomhq = getenv("STW_BLOOMHQ") ? atoi(getenv("STW_BLOOMHQ")) != 0 : qs.bloom_hq;
        int aa = getenv("STW_AA") ? atoi(getenv("STW_AA")) : qs.aa;
        gmsaa = aa == 2 ? 2 : aa == 3 ? 4 : 1;
        if (getenv("STW_MSAA")) gmsaa = atoi(getenv("STW_MSAA")) >= 4 ? 4 : atoi(getenv("STW_MSAA")) >= 2 ? 2 : 1;
        if (gmsaa > 1 && ![dev supportsTextureSampleCount:gmsaa]) gmsaa = 1;
    }
    static NSString *const util_src =
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct UV { float4 pos [[position]]; float2 uv; };\n"
         "vertex UV u_vs(uint vid [[vertex_id]]) { float2 p = float2((vid << 1) & 2, vid & 2); UV o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
         "fragment float4 u_copy(UV in [[stage_in]], texture2d<float> t [[texture(0)]]) { constexpr sampler s(filter::linear, address::clamp_to_edge); return t.sample(s, in.uv); }\n"
         "fragment float4 u_fill(UV in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }\n";
    NSError *err = nil;
    id<MTLLibrary> ulib = [dev newLibraryWithSource:util_src options:nil error:&err];
    if (!ulib) { fprintf(stderr, "stw: utility shaders failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
    for (int i = 0; i < 2; i++) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [ulib newFunctionWithName:@"u_vs"];
        pd.fragmentFunction = [ulib newFunctionWithName:i ? @"u_fill" : @"u_copy"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!ps) { fprintf(stderr, "stw: utility pipeline failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
        if (i) util_fill_pso = ps; else util_copy_pso = ps;
    }
    util_lib = ulib;
    make_ms_pso();
    return 1;
}

static void end_encoder(void) {
    if (enc) { [enc endEncoding]; enc = nil; enc_color = enc_depth = nil; }
}
/* Submit the open command buffer; optionally wait for completion (then nothing is pending on the GPU). */
uint64_t gpu_wait_n[8], gpu_wait_ns[8];                  /* STW_FPS statistics: count and time of CPU waits on the GPU, by reason */
static void flush_locked(int wait);
static void resolve_all(void);
static void wait_gpu_ex(int why, int resolve) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    if (resolve) resolve_all();                             /* the CPU is about to look at guest memory: bring the 1x views up to date */
    flush_locked(1);
    clock_gettime(CLOCK_MONOTONIC, &b);
    gpu_wait_n[why]++; gpu_wait_ns[why] += (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (b.tv_nsec - a.tv_nsec);
}
static void wait_gpu(int why) { wait_gpu_ex(why, why == 1 || why == 2); }
static void flush_locked(int wait) {
    end_encoder();
    if (cb) {
        if (cb_has_fence) { uint64_t seq = cb_seq; [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { fence_drain(seq); }]; }
        [cb commit]; last_cb = cb; cb = nil; draws_in_cb = 0;
    }
    if (wait) {
        if (last_cb) [last_cb waitUntilCompleted];
        fence_drain(UINT64_MAX);
        for (int i = 0; i < ntargets; i++) targets[i].written = 0;
        vring_off = 0;
    }
}
static id<MTLCommandBuffer> open_cb(void) {
    if (!cb) { cb = [queue commandBuffer]; cb_seq++; cb_has_fence = 0; }
    return cb;
}
void gpu_fence(uint32_t addr, uint32_t value) {
    static int defer = -1;
    if (defer < 0) defer = getenv("STW_SYNCFENCE") == NULL;       /* STW_SYNCFENCE=1: wait for the GPU at every fence instead (debugging) */
    pthread_mutex_lock(&gpu_lock);
    if (!defer) { wait_gpu(0); __atomic_store_n((uint32_t *)(G + addr), value, __ATOMIC_RELEASE); pthread_mutex_unlock(&gpu_lock); return; }
    if (!dev || (!cb && (!last_cb || last_cb.status >= MTLCommandBufferStatusCompleted))) {   /* nothing in flight: store now, after earlier fences */
        fence_drain(UINT64_MAX);
        pthread_mutex_lock(&fence_lock);
        __atomic_store_n((uint32_t *)(G + addr), value, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&fence_lock);
    } else {
        if (nfences >= 4096) wait_gpu(0);                       /* table full: let the GPU catch up (drains every pending fence) */
        open_cb();
        pthread_mutex_lock(&fence_lock);
        if (nfences < 4096) fences[nfences++] = (Fence){ cb_seq, addr, value };
        pthread_mutex_unlock(&fence_lock);
        cb_has_fence = 1;
        flush_locked(0);                                    /* submit now so the fence resolves as soon as the GPU gets there */
    }
    pthread_mutex_unlock(&gpu_lock);
}
/* Wait only if a fence is still outstanding (interrupt handlers may look at what fences have released). */
void gpu_wait_idle(void);
void gpu_sync_fences(void) {
    pthread_mutex_lock(&fence_lock);
    int pend = nfences > 0;
    pthread_mutex_unlock(&fence_lock);
    if (pend) gpu_wait_idle();
}
void gpu_sync(void) {                                       /* wait, and make the guest-visible 1x surfaces current */
    pthread_mutex_lock(&gpu_lock);
    wait_gpu(1);
    pthread_mutex_unlock(&gpu_lock);
}
void gpu_wait_idle(void) {                                  /* wait only (fences, presenter): the hi textures are what is consumed */
    pthread_mutex_lock(&gpu_lock);
    wait_gpu_ex(1, 0);
    pthread_mutex_unlock(&gpu_lock);
}

/* ---- render targets ---- */
static Target *find_target(uint32_t addr) {
    for (int i = 0; i < ntargets; i++) if (targets[i].addr == addr) return &targets[i];
    return NULL;
}
/* hi texture of a surface: private, the guest size at the internal scale. */
static id<MTLTexture> make_hi(uint32_t w, uint32_t h) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:SCL(w) height:SCL(h) mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    return [dev newTextureWithDescriptor:td];
}
/* Full-surface pass dst = src (linear filtered): upsample 1x -> hi, or downsample hi -> 1x. */
static void encode_copy(id<MTLTexture> src, id<MTLTexture> dst) {
    end_encoder();
    open_cb();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:util_copy_pso];
    [e setFragmentTexture:src atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    draws_in_cb++;
}
static void resolve_target(Target *tg) {
    if (!tg->dirty || !tg->hi || tg->hi == tg->view) return;
    encode_copy(tg->hi, tg->view);
    tg->dirty = 0; tg->written = 1;
}
static void resolve_all(void) { for (int i = 0; i < ntargets; i++) resolve_target(&targets[i]); }

/* The colour target of a draw: the hi texture (the 1x view itself when gscale is 1). */
static id<MTLTexture> color_target(const GpuDraw *d) {
    Target *tg = find_target(d->color_addr);
    if (tg && tg->view && tg->pitch == d->color_pitch && tg->w == d->width && tg->h == d->height && tg->scale == gscale) {
        if (tg->stale && tg->hi != tg->view) { encode_copy(tg->view, tg->hi); tg->stale = 0; tg->ms_stale = 1; }       /* the CPU wrote the 1x memory */
        tg->stale = 0;
        return tg->hi;
    }
    if (tg) resolve_target(tg);                                      /* a different surface takes over this address: keep the old content in memory */
    uint64_t k = h64w(h64w(h64w(h64w(1, d->color_addr), d->color_pitch), d->width), d->height);
    id<MTLTexture> t = view_cache[@(k)];
    if (!t) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:d->width height:d->height mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        t = [ram newTextureWithDescriptor:td offset:d->color_addr bytesPerRow:d->color_pitch];
        if (!t) return nil;
        view_cache[@(k)] = t;
    }
    id<MTLTexture> hi = t;
    if (gscale > 4) {
        hi = make_hi(d->width, d->height);
        if (!hi) return nil;
        encode_copy(t, hi);                                          /* start from whatever the 1x memory holds */
    }
    if (!tg && ntargets < 64) tg = &targets[ntargets++];
    if (tg) { tg->addr = d->color_addr; tg->pitch = d->color_pitch; tg->w = d->width; tg->h = d->height; tg->view = t; tg->hi = hi; tg->ms = nil; tg->ms_stale = 1; tg->written = 0; tg->dirty = 0; tg->stale = 0; tg->scale = gscale; }
    return hi;
}
/* MSAA applies to the full-size scene targets (640x480 and up): colour and depth/stencil of such a surface are multisampled together. */
static inline int ms_on(uint32_t w, uint32_t h) { return gmsaa > 1 && w >= 640 && h >= 480; }
static id<MTLTexture> depth_target(uint32_t addr, uint32_t w, uint32_t h, int create) {
    int ms = ms_on(w, h);
    w = SCL(w); h = SCL(h);
    NSNumber *key = @(h64w(h64w(h64w(h64w(2, addr), w), h), ms ? gmsaa : 1));
    id<MTLTexture> t = depth_cache[key];
    if (!t && create) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
        if (ms) { td.textureType = MTLTextureType2DMultisample; td.sampleCount = gmsaa; }
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        t = [dev newTextureWithDescriptor:td];
        depth_cache[key] = t;
    }
    return t;
}

/* The multisampled twin of a target's hi texture; (re)filled from hi when something wrote hi directly (clears, CPU uploads). Call with no encoder open. */
static id<MTLTexture> ensure_ms(Target *tg) {
    if (!tg->ms) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:tg->hi.width height:tg->hi.height mipmapped:NO];
        td.textureType = MTLTextureType2DMultisample; td.sampleCount = gmsaa;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget;
        tg->ms = [dev newTextureWithDescriptor:td];
        if (!tg->ms) return nil;
        if (getenv("STW_MSDEBUG")) fprintf(stderr, "[msaa] target %#x %ux%u -> %lux%lu x%d\n", tg->addr, tg->w, tg->h, (unsigned long)tg->hi.width, (unsigned long)tg->hi.height, gmsaa);
        tg->ms_stale = 1;
    }
    if (tg->ms_stale) {
        end_encoder();
        open_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tg->ms;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:util_copy_ms_pso];
        [e setFragmentTexture:tg->hi atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        draws_in_cb++;
        tg->ms_stale = 0;
    }
    return tg->ms;
}

void gpu_clear_depth(uint32_t zeta_addr, uint32_t width, uint32_t height, float depth, uint32_t stencil, int mask) {
    if (!mask || !gpu_ready()) return;
    pthread_mutex_lock(&gpu_lock);
    @autoreleasepool {
        id<MTLTexture> dt = depth_target(zeta_addr, width, height, 1);
        end_encoder();                                       /* keep the command buffer; the clear is its own pass */
        open_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = dt;
        rp.depthAttachment.loadAction = (mask & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.texture = dt;
        rp.stencilAttachment.loadAction = (mask & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = stencil;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        draws_in_cb++;
    }
    pthread_mutex_unlock(&gpu_lock);
}

/* Colour clear of a whole 32-bit surface as a render pass (no CPU fill, so no wait for the GPU). Returns 0 when it cannot
 * (caller falls back to filling guest memory). */
int gpu_clear_color(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint32_t argb) {
    if (!gpu_ready() || !width || !height || !pitch) return 0;
    pthread_mutex_lock(&gpu_lock);
    int ok = 0;
    @autoreleasepool {
        GpuDraw tmp; memset(&tmp, 0, sizeof tmp);
        tmp.color_addr = addr; tmp.color_pitch = pitch; tmp.width = width; tmp.height = height;
        id<MTLTexture> ct = color_target(&tmp);
        if (ct) {
            end_encoder();
            open_cb();
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ct;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(((argb >> 16) & 255) / 255.0, ((argb >> 8) & 255) / 255.0, (argb & 255) / 255.0, (argb >> 24) / 255.0);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            draws_in_cb++;
            Target *tg = find_target(addr);
            if (tg) { tg->written = 1; tg->dirty = 1; tg->ms_stale = 1; }
            ok = 1;
        }
    }
    pthread_mutex_unlock(&gpu_lock);
    return ok;
}

/* Clear of a sub-rectangle (inclusive sample coordinates) of a 32-bit surface: a scissored fill, so it stays on the GPU and in order. */
int gpu_clear_color_rect(uint32_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t argb) {
    if (!gpu_ready() || !width || !height || !pitch || x1 < x0 || y1 < y0) return 0;
    pthread_mutex_lock(&gpu_lock);
    int ok = 0;
    @autoreleasepool {
        GpuDraw tmp; memset(&tmp, 0, sizeof tmp);
        tmp.color_addr = addr; tmp.color_pitch = pitch; tmp.width = width; tmp.height = height;
        id<MTLTexture> ct = color_target(&tmp);
        if (ct) {
            end_encoder();
            open_cb();
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ct;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
            [e setRenderPipelineState:util_fill_pso];
            if (x1 >= width) x1 = width - 1;
            if (y1 >= height) y1 = height - 1;
            MTLScissorRect sr = { SCL(x0), SCL(y0), SCL(x1 + 1) - SCL(x0), SCL(y1 + 1) - SCL(y0) };
            [e setScissorRect:sr];
            float c[4] = { ((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f };
            [e setFragmentBytes:c length:sizeof c atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            draws_in_cb++;
            Target *tg = find_target(addr);
            if (tg) { tg->written = 1; tg->dirty = 1; tg->ms_stale = 1; }
            ok = 1;
        }
    }
    pthread_mutex_unlock(&gpu_lock);
    return ok;
}
/* The CPU wrote guest memory that may be a render target: the next draw re-reads it into the hi texture. */
void gpu_cpu_wrote(uint32_t addr, uint32_t bytes) {
    pthread_mutex_lock(&gpu_lock);
    for (int i = 0; i < ntargets; i++)
        if (addr < targets[i].addr + targets[i].pitch * targets[i].h && targets[i].addr < addr + bytes) { targets[i].stale = 1; targets[i].dirty = 0; }
    pthread_mutex_unlock(&gpu_lock);
}

/* Change the MSAA sample count at run time (debug aid STW_AASCRIPT; the launcher setting is read once at start). 0/1 = off. */
void gpu_set_msaa(int n) {
    if (!gpu_ready()) return;
    int c = n >= 4 ? 4 : n >= 2 ? 2 : 1;
    pthread_mutex_lock(&gpu_lock);
    if (c != gmsaa && (c == 1 || [dev supportsTextureSampleCount:c])) {
        wait_gpu(1);                                         /* resolve + wait: nothing multisampled is in flight any more */
        gmsaa = c;
        for (int i = 0; i < ntargets; i++) { targets[i].ms = nil; targets[i].ms_stale = 1; }
        [depth_cache removeAllObjects];
        make_ms_pso();
    }
    pthread_mutex_unlock(&gpu_lock);
}
void gpu_set_bloomhq(int on) { q_bloomhq = on; }          /* debug: STW_BLOOMSCRIPT */
int gpu_res(void) { for (int i = 0; i < 5; i++) if (res_quarters[i] == gscale) return i; return 2; }          /* index of the current resolution preset */
/* Change the internal resolution at run time (preset 0..4): everything is resolved to guest memory first, then the hi textures are rebuilt lazily. */
void gpu_set_res(int idx) {
    if (idx < 0 || idx > 4 || !gpu_ready()) return;
    int s = res_quarters[idx];
    pthread_mutex_lock(&gpu_lock);
    if (s != gscale) {
        wait_gpu(1);                                         /* resolve + wait */
        for (int i = 0; i < ntargets; i++) { targets[i].hi = nil; targets[i].ms = nil; targets[i].scale = 0; targets[i].dirty = 0; }
        [depth_cache removeAllObjects];
        view_cache = [NSMutableDictionary new];
        gscale = s;
    }
    pthread_mutex_unlock(&gpu_lock);
}

/* Presenter access: the hi texture of the surface at `addr` when it is exactly w x h and current. */
static id<MTLTexture> present_hold;
void *gpu_scanout_texture(uint32_t addr, uint32_t w, uint32_t h, uint32_t *tw, uint32_t *th) {
    void *r = NULL;
    pthread_mutex_lock(&gpu_lock);
    Target *tg = find_target(addr);
    if (tg && tg->hi && tg->hi != tg->view && tg->scale == gscale && tg->w == w && tg->h == h && !tg->stale) {
        present_hold = tg->hi;                               /* kept alive until the next call */
        r = (__bridge void *)present_hold;
        *tw = SCL(w); *th = SCL(h);
    }
    pthread_mutex_unlock(&gpu_lock);
    return r;
}
/* Copy of the hi texture (BGRA, tightly packed, malloc'd) for dumps; NULL when the surface has none. */
void *gpu_read_hi(uint32_t addr, uint32_t *tw, uint32_t *th) {
    void *out = NULL;
    pthread_mutex_lock(&gpu_lock);
    Target *tg = find_target(addr);
    if (tg && tg->hi && tg->hi != tg->view) {
        wait_gpu_ex(1, 0);
        uint32_t w = SCL(tg->w), h = SCL(tg->h);
        id<MTLBuffer> bb = [dev newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> bcb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> be = [bcb blitCommandEncoder];
        [be copyFromTexture:tg->hi sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                   toBuffer:bb destinationOffset:0 destinationBytesPerRow:(size_t)w * 4 destinationBytesPerImage:(size_t)w * h * 4];
        [be endEncoding]; [bcb commit]; [bcb waitUntilCompleted];
        out = malloc((size_t)w * h * 4);
        if (out) memcpy(out, bb.contents, (size_t)w * h * 4);
        *tw = w; *th = h;
    }
    pthread_mutex_unlock(&gpu_lock);
    return out;
}

/* ---- textures ---- */
static uint32_t deposit(uint32_t v, uint32_t mask) {
    uint32_t r = 0;
    for (uint32_t bit = 1; mask; mask &= mask - 1, bit <<= 1) if (v & bit) r |= mask & -mask;
    return r;
}
static void swizzle_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my) {
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    int done;
    do {
        done = 1;
        if (bit < w) { x |= mb; mb <<= 1; done = 0; }
        if (bit < h) { y |= mb; mb <<= 1; done = 0; }
        bit <<= 1;
    } while (!done);
    *mx = x; *my = y;
}
static uint32_t pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
static uint32_t c565(uint16_t v) { return pack(255, ((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31); }

static void dxt_color_block(const uint8_t *b, uint32_t out[16], int dxt1) {
    uint16_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    uint32_t p[4], a = c565(c0), c = c565(c1);
    p[0] = a; p[1] = c;
    int r0 = (a >> 16) & 255, g0 = (a >> 8) & 255, b0 = a & 255, r1 = (c >> 16) & 255, g1 = (c >> 8) & 255, b1 = c & 255;
    if (!dxt1 || c0 > c1) {
        p[2] = pack(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        p[3] = pack(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    } else { p[2] = pack(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2); p[3] = 0; }
    uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = p[(bits >> (2 * i)) & 3];
}

static int tex_bpp(uint32_t col) {
    switch (col) {
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B: case 0x3C: case 0x3F: case 0x40: case 0x41: return 4;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1C: case 0x1D: case 0x1A: case 0x20: case 0x17: case 0x28: case 0x29: return 2;
    case 0x00: case 0x01: case 0x13: case 0x19: case 0x1B: case 0x1F: return 1;
    default: return 0;
    }
}
/* Bytes one mip level of w x h occupies in guest memory. */
static size_t level_bytes(uint32_t col, uint32_t w, uint32_t h) {
    if (col == 0x0C || col == 0x0E || col == 0x0F) return (size_t)((w + 3) / 4) * ((h + 3) / 4) * (col == 0x0C ? 8 : 16);
    return (size_t)w * h * tex_bpp(col);
}

/* Decode one level (w x h at guest address `addr`) into BGRA8. Returns NULL for formats not handled yet. */
static uint32_t *decode_level(const GpuTex *t, uint32_t addr, uint32_t w, uint32_t h) {
    uint32_t col = t->format;
    if (!w || !h || w > 4096 || h > 4096) return NULL;
    uint32_t *out = malloc((size_t)w * h * 4);
    const uint8_t *src = G + addr;
    int dxt = col == 0x0C || col == 0x0E || col == 0x0F;
    if (dxt) {
        uint32_t bs = col == 0x0C ? 8 : 16;
        for (uint32_t by = 0; by < (h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                const uint8_t *b = src + (size_t)(by * ((w + 3) / 4) + bx) * bs;
                uint32_t px[16];
                dxt_color_block(b + (bs == 16 ? 8 : 0), px, col == 0x0C);
                for (int i = 0; i < 16; i++) {
                    uint32_t a = 255;
                    if (col == 0x0E) { uint8_t n = (b[i / 2] >> ((i & 1) * 4)) & 15; a = n * 17; }
                    else if (col == 0x0F) {
                        uint8_t a0 = b[0], a1 = b[1], al[8];
                        al[0] = a0; al[1] = a1;
                        if (a0 > a1) for (int k = 1; k < 7; k++) al[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                        else { for (int k = 1; k < 5; k++) al[k + 1] = ((5 - k) * a0 + k * a1) / 5; al[6] = 0; al[7] = 255; }
                        uint64_t bits = 0;
                        for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                        a = al[(bits >> (3 * i)) & 7];
                    }
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h) out[y * w + x] = (px[i] & 0x00FFFFFFu) | (col == 0x0C ? (px[i] & 0xFF000000u) : a << 24);
                }
            }
        return out;
    }
    int bpp;
    switch (col) {
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B: case 0x3C: case 0x3F: case 0x40: case 0x41: bpp = 4; break;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1C: case 0x1D: case 0x1A: case 0x20: case 0x17: case 0x28: case 0x29: bpp = 2; break;
    case 0x00: case 0x01: case 0x13: case 0x19: case 0x1B: case 0x1F: bpp = 1; break;
    default: { static uint32_t seen; if (!(seen & (1u << (col & 31)))) { seen |= 1u << (col & 31); fprintf(stderr, "stw: texture format %#x not decoded yet\n", col); } free(out); return NULL; }
    }
    uint32_t mx = 0, my = 0;
    if (t->swizzled) swizzle_masks(w, h, &mx, &my);
    uint32_t pitch = t->pitch ? t->pitch : w * bpp;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = t->swizzled ? src + (size_t)(deposit(x, mx) + deposit(y, my)) * bpp : src + (size_t)y * pitch + (size_t)x * bpp;
            uint32_t v = 0;
            switch (col) {
            case 0x06: case 0x12: memcpy(&v, p, 4); break;
            case 0x07: case 0x1E: memcpy(&v, p, 4); v |= 0xFF000000u; break;
            case 0x3A: case 0x3F: v = pack(p[3], p[0], p[1], p[2]); break;                     /* A8B8G8R8: bytes R,G,B,A */
            case 0x3B: case 0x40: v = pack(p[0], p[1], p[2], p[3]); break;                     /* B8G8R8A8: bytes A,R,G,B */
            case 0x3C: case 0x41: v = pack(p[0], p[3], p[2], p[1]); break;                     /* R8G8B8A8: LE word 0xRRGGBBAA, so bytes in memory are A,B,G,R */
            case 0x05: case 0x11: { uint16_t s; memcpy(&s, p, 2); v = c565(s); } break;
            case 0x02: case 0x10: { uint16_t s; memcpy(&s, p, 2); v = pack((s >> 15) * 255, ((s >> 10) & 31) * 255 / 31, ((s >> 5) & 31) * 255 / 31, (s & 31) * 255 / 31); } break;
            case 0x03: case 0x1C: { uint16_t s; memcpy(&s, p, 2); v = pack(255, ((s >> 10) & 31) * 255 / 31, ((s >> 5) & 31) * 255 / 31, (s & 31) * 255 / 31); } break;
            case 0x04: case 0x1D: { uint16_t s; memcpy(&s, p, 2); v = pack(((s >> 12) & 15) * 17, ((s >> 8) & 15) * 17, ((s >> 4) & 15) * 17, (s & 15) * 17); } break;
            case 0x00: case 0x13: v = pack(255, p[0], p[0], p[0]); break;
            case 0x19: case 0x1F: v = pack(p[0], 255, 255, 255); break;
            case 0x01: case 0x1B: v = pack(p[0], p[0], p[0], p[0]); break;
            case 0x1A: case 0x20: v = pack(p[1], p[0], p[0], p[0]); break;
            case 0x17: case 0x28: v = pack(p[1], p[0], p[1], p[0]); break;                     /* G8B8: (r,g,b,a) = (byte0, byte1, byte0, byte1) */
            case 0x29: v = pack(p[1], p[1], p[0], p[0]); break;                               /* R8B8: (r,g,b,a) = (byte1, byte0, byte0, byte1) */
            }
            out[y * w + x] = v;
        }
    return out;
}
static uint32_t *decode_texture(const GpuTex *t) { return decode_level(t, t->addr, t->width, t->height); }

/* Aliasing: a texture that is exactly a render target we draw to is sampled through that target's view (no CPU copy,
 * and the GPU orders the work). X8R8G8B8 textures get their alpha forced to 1 through a swizzled view. */
static id<MTLTexture> cur_ct;                               /* colour target of the draw whose textures are being resolved */
static id<MTLTexture> target_texture(const GpuTex *t, int *needs_barrier) {
    int linear32 = t->format == 0x12 || t->format == 0x1E || t->format == 0x41;
    if (!linear32) return nil;
    Target *tg = find_target(t->addr);
    if (!tg || !tg->hi || tg->pitch != t->pitch || tg->w != t->width || tg->h != t->height || tg->scale != gscale) return nil;
    if (tg->hi == cur_ct) return nil;                       /* feedback loop (sampling the surface this draw renders to): fall back to a CPU snapshot */
    if (tg->written || tg->dirty) *needs_barrier = 1;
    if (t->format == 0x12) return tg->hi;
    NSNumber *key = @(h64w(h64w(h64w(t->format == 0x41 ? 5 : 3, t->addr), t->pitch), (uint64_t)(uintptr_t)(__bridge void *)tg->hi));
    id<MTLTexture> v = view_cache[key];
    if (!v) {
        MTLTextureSwizzleChannels sw = { MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne };
        if (t->format == 0x41) { sw = (MTLTextureSwizzleChannels){ MTLTextureSwizzleAlpha, MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue }; }   /* see decode_level: r,g,b,a = byte3,2,1,0 */
        v = [tg->hi newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm textureType:MTLTextureType2D
                                           levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw];
        view_cache[key] = v;
    }
    return v;
}

/* Bytes of guest memory the decoder reads for this texture: the mip chain for swizzled/DXT, pitch * height for linear. */
static size_t tex_src_bytes(const GpuTex *t, uint32_t nlev) {
    uint32_t w = t->width, h = t->height;
    int dxt = t->format == 0x0C || t->format == 0x0E || t->format == 0x0F;
    size_t bytes = 0;
    if (!t->swizzled && !dxt) bytes = t->pitch ? (size_t)t->pitch * h : level_bytes(t->format, w, h);
    else for (uint32_t l = 0, lw = w, lh = h; l < nlev; l++, lw = lw > 1 ? lw >> 1 : 1, lh = lh > 1 ? lh >> 1 : 1) bytes += level_bytes(t->format, lw, lh);
    if (!bytes) bytes = (size_t)w * h * 4;
    return bytes > (1u << 24) ? (1u << 24) : bytes;
}

static int tex_was_alias;                                   /* the last get_texture() returned a view of a render target */
static const GpuDraw *dbg_d;
static id<MTLTexture> get_texture(const GpuTex *t, int *needs_barrier) {
    tex_was_alias = 0;
    if (!t->enabled) return white_tex;
    id<MTLTexture> alias = target_texture(t, needs_barrier);
    if (alias) { tex_was_alias = 1; return alias; }
    uint32_t w = t->width, h = t->height;
    int snapshot = 0;                                      /* the texture is (part of) a render target's memory: a per-frame image, not an asset */
    for (int i = 0; i < ntargets; i++)
        if (t->addr < targets[i].addr + targets[i].pitch * targets[i].h && targets[i].addr < t->addr + w * h * 4) snapshot = 1;
    /* A snapshot of RAM that pending GPU work may still be writing must wait for it. */
    for (int i = 0; i < ntargets; i++)
        if ((targets[i].written || targets[i].dirty) && t->addr < targets[i].addr + targets[i].pitch * targets[i].h && targets[i].addr < t->addr + w * h * 4) {
            static int tw = -1; if (tw < 0) tw = getenv("STW_TEXWAIT") != NULL;
            if (tw) fprintf(stderr, "[texwait] prog=%#x color=%#x blend=%d depth=%d verts=%u tex fmt=%#x %ux%u pitch=%#x @%#x vs target @%#x %ux%u pitch=%#x%s\n", dbg_d->psh.stage_program, dbg_d->color_addr, dbg_d->blend_enable, dbg_d->depth_enable, dbg_d->nverts, t->format, w, h, t->pitch, t->addr, targets[i].addr, targets[i].w, targets[i].h, targets[i].pitch, targets[i].hi == enc_color ? " (current)" : "");
            wait_gpu(2);
            break;
        }
    uint32_t nlev = 1;                                     /* mip chain: levels follow each other, each half the size of the last */
    if ((t->swizzled || t->format == 0x0C || t->format == 0x0E || t->format == 0x0F) && t->mip_levels > 1 && !t->cube) {
        uint32_t maxl = 1, m = w > h ? w : h;
        while (m > 1) { m >>= 1; maxl++; }
        nlev = t->mip_levels < maxl ? t->mip_levels : maxl;
    }
    uint64_t hash = hash_mem(G + t->addr, tex_src_bytes(t, nlev));
    NSNumber *key = @(h64w(h64w(h64w(h64w(4, t->addr), t->format), (uint64_t)w << 16 | h), t->pitch));
    NSArray *ent = tex_cache[key];
    if (ent && [ent[1] unsignedLongLongValue] == hash) return ent[0];
    uint32_t *px = decode_texture(t);
    if (!px) return white_tex;
    if (getenv("STW_DUMPTEX") && t->format == strtoul(getenv("STW_DUMPTEX"), NULL, 16)) {       /* write each new texture of this format as /tmp/tex_<fmt>_<addr>.ppm */
        char name[128];
        snprintf(name, sizeof name, "/tmp/tex_%x_%x.ppm", t->format, t->addr);
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) { fputc((px[i] >> 16) & 255, f); fputc((px[i] >> 8) & 255, f); fputc(px[i] & 255, f); }
            fclose(f);
        }
    }
    if (getenv("STW_TEXTRACE")) {
        fprintf(stderr, "[tex] decode fmt=%#x %ux%u pitch=%#x @%#x: src bytes %02x %02x %02x %02x -> %08x\n", t->format, w, h, t->pitch, t->addr,
                G[t->addr], G[t->addr + 1], G[t->addr + 2], G[t->addr + 3], px[0]);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.mipmapLevelCount = nlev;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    uint32_t lw = w, lh = h; uint32_t addr = t->addr + (uint32_t)level_bytes(t->format, w, h);
    for (uint32_t l = 1; l < nlev; l++) {
        lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
        uint32_t *lp = decode_level(t, addr, lw, lh);
        if (lp) { [tex replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:l withBytes:lp bytesPerRow:lw * 4]; free(lp); }
        addr += (uint32_t)level_bytes(t->format, lw, lh);
    }
    tex_cache[key] = @[tex, @(hash)];
    return tex;
}

static MTLSamplerAddressMode addr_mode(uint32_t m) {
    switch (m) { case 2: return MTLSamplerAddressModeMirrorRepeat; case 3: case 5: return MTLSamplerAddressModeClampToEdge;
                 case 4: return MTLSamplerAddressModeClampToBorderColor; default: return MTLSamplerAddressModeRepeat; }
}
static id<MTLSamplerState> get_sampler(const GpuTex *t) {
    uint32_t mn = (t->filter >> 16) & 0xFF, mg = (t->filter >> 24) & 0xF, u = t->address & 0xF, v = (t->address >> 8) & 0xF;
    uint32_t mips = t->swizzled || t->format == 0x0C || t->format == 0x0E || t->format == 0x0F ? t->mip_levels : 1;
    NSNumber *key = @((uint64_t)mn << 48 | (uint64_t)mg << 32 | (uint64_t)u << 16 | v | (uint64_t)(mips > 1) << 40);
    id<MTLSamplerState> s = sampler_cache[key];
    if (s) return s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    /* MIN: 1 box lod0, 2 tent lod0, 3 box nearest-lod, 4 tent nearest-lod, 5 box tent-lod, 6 tent tent-lod */
    d.minFilter = (mn == 1 || mn == 3 || mn == 5) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.mipFilter = (mips > 1 && (mn == 3 || mn == 4)) ? MTLSamplerMipFilterNearest : (mips > 1 && (mn == 5 || mn == 6)) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    d.magFilter = (mg == 1) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.sAddressMode = addr_mode(u); d.tAddressMode = addr_mode(v);
    static int aniso = -1;                                  /* STW_ANISO=1..16: anisotropic filtering for mip-mapped trilinear/bilinear textures (default 8) */
    if (aniso < 0) aniso = getenv("STW_ANISO") ? atoi(getenv("STW_ANISO")) : 8;
    if (aniso > 1 && mips > 1 && d.minFilter == MTLSamplerMinMagFilterLinear && d.magFilter == MTLSamplerMinMagFilterLinear && d.mipFilter != MTLSamplerMipFilterNotMipmapped)
        d.maxAnisotropy = aniso > 16 ? 16 : aniso;
    s = [dev newSamplerStateWithDescriptor:d];
    sampler_cache[key] = s;
    return s;
}

/* ---- pipeline state ---- */
static MTLBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 0: return MTLBlendFactorZero; case 1: return MTLBlendFactorOne;
    case 0x300: return MTLBlendFactorSourceColor; case 0x301: return MTLBlendFactorOneMinusSourceColor;
    case 0x302: return MTLBlendFactorSourceAlpha; case 0x303: return MTLBlendFactorOneMinusSourceAlpha;
    case 0x304: return MTLBlendFactorDestinationAlpha; case 0x305: return MTLBlendFactorOneMinusDestinationAlpha;
    case 0x306: return MTLBlendFactorDestinationColor; case 0x307: return MTLBlendFactorOneMinusDestinationColor;
    case 0x308: return MTLBlendFactorSourceAlphaSaturated;
    case 0x8001: return MTLBlendFactorBlendColor; case 0x8002: return MTLBlendFactorOneMinusBlendColor;
    case 0x8003: return MTLBlendFactorBlendAlpha; case 0x8004: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}
static MTLCompareFunction cmp_func(uint32_t f) {
    switch (f) { case 0x200: return MTLCompareFunctionNever; case 0x201: return MTLCompareFunctionLess; case 0x202: return MTLCompareFunctionEqual;
                 case 0x203: return MTLCompareFunctionLessEqual; case 0x204: return MTLCompareFunctionGreater; case 0x205: return MTLCompareFunctionNotEqual;
                 case 0x206: return MTLCompareFunctionGreaterEqual; default: return MTLCompareFunctionAlways; }
}

static MTLStencilOperation stencil_op(uint32_t op) {
    switch (op) { case 0x0000: return MTLStencilOperationZero; case 0x1E01: return MTLStencilOperationReplace; case 0x1E02: return MTLStencilOperationIncrementClamp;
                  case 0x1E03: return MTLStencilOperationDecrementClamp; case 0x150A: return MTLStencilOperationInvert; case 0x8507: return MTLStencilOperationIncrementWrap;
                  case 0x8508: return MTLStencilOperationDecrementWrap; default: return MTLStencilOperationKeep; }
}

static id<MTLRenderPipelineState> get_pso(const GpuDraw *d, int ns) {
    /* Key from the state that changes the generated code, before building any shader text. */
    uint64_t h = h64(d->vsh, 136 * 4 * sizeof(uint32_t), 1469598103934665603ull);
    h = h64w(h, d->vsh_start);
    h = h64(&d->psh.control, sizeof d->psh.control, h);
    h = h64(d->psh.rgb_icw, sizeof d->psh.rgb_icw, h); h = h64(d->psh.rgb_ocw, sizeof d->psh.rgb_ocw, h);
    h = h64(d->psh.alpha_icw, sizeof d->psh.alpha_icw, h); h = h64(d->psh.alpha_ocw, sizeof d->psh.alpha_ocw, h);
    h = h64w(h64w(h64w(h, d->psh.final0), d->psh.final1), d->psh.stage_program);
    h = h64w(h64w(h, d->psh.alpha_test), d->psh.alpha_func);
    h = h64w(h, ns);
    h = h64w(h64w(h64w(h64w(h64w(h, d->blend_enable), d->blend_src), d->blend_dst), d->blend_eq), d->color_mask);
    NSNumber *key = @(h);
    id<MTLRenderPipelineState> p = pso_cache[key];
    if (p) return p;
    char *vs = vsh_to_msl(d->vsh, d->vsh_start, 136), *ps = psh_to_msl(&d->psh);
    if (getenv("STW_SHADERLOG") && d->psh.stage_program == strtoul(getenv("STW_SHADERLOG"), NULL, 16)) fprintf(stderr, "---- fragment ----\n%s\n", ps);
    NSString *src = [NSString stringWithFormat:@"%s%s", vs, ps];
    free(vs); free(ps);
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "stw: shader compile failed: %s\n", err.localizedDescription.UTF8String);
        if (getenv("STW_DUMPSHADER")) fprintf(stderr, "%s\n", src.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLColorWriteMask m = 0;
    if (d->color_mask & (1u << 16)) m |= MTLColorWriteMaskRed;
    if (d->color_mask & (1u << 8)) m |= MTLColorWriteMaskGreen;
    if (d->color_mask & 1u) m |= MTLColorWriteMaskBlue;
    if (d->color_mask & (1u << 24)) m |= MTLColorWriteMaskAlpha;
    ca.writeMask = m;                                      /* a zero mask really means "write no colour" (depth-only passes) */
    if (d->blend_enable) {
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = blend_factor(d->blend_src);
        ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = blend_factor(d->blend_dst);
        switch (d->blend_eq) {
        case 0x800A: ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationSubtract; break;
        case 0x800B: ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationReverseSubtract; break;
        case 0x8007: ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationMin; break;
        case 0x8008: ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationMax; break;
        default: ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationAdd; break;
        }
    }
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.rasterSampleCount = ns;
    p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!p) { fprintf(stderr, "stw: pipeline failed: %s\n", err.localizedDescription.UTF8String); return nil; }
    pso_cache[key] = p;
    return p;
}

/* ---- widescreen ----
 * In 16:9 mode the game builds its 3D view for a 16:9 aspect (runtime/data/patches.txt) and renders it into the same 640x480 buffer; the
 * presenter stretches that to 16:9. The picture is therefore anamorphic: the 3D scene looks right when stretched, but the game's 2D elements
 * (HUD, text, menu items, logos) are laid out for 4:3, so they are squeezed to 3/4 about the screen centre to look right once stretched.
 * The vertex shader does the squeeze for vertices it outputs with w = 1 (orthographic: perspective 3D never has w = 1); this function only
 * switches it off for the game's full-screen passes (letterbox bars, fades, bloom, backgrounds), which must keep covering the whole width: a
 * short pass-through vertex program whose vertices are window coordinates spanning the screen. */
int gpu_widescreen(void) {
    static int w = -1;
    if (w < 0) w = getenv("STW_WIDE") ? atoi(getenv("STW_WIDE")) != 0 : stw_settings_get().widescreen;
    return w;
}
static int vsh_length(const GpuDraw *d) {
    int n = 0;
    for (int i = d->vsh_start; i < 136; i++) { n++; if (d->vsh[i * 4 + 3] & 1) break; }
    return n;
}
static float ui_squeeze(const GpuDraw *d) {
    if (!gpu_widescreen() || d->width != 640 || d->height != 480) return 1.f;           /* off-screen passes (bloom chain) are not scaled */
    if (vsh_length(d) <= 11) {                                                           /* pass-through programs: look at the window coordinates they carry */
        float mnx = 1e30f, mxx = -1e30f;
        for (uint32_t i = 0; i < d->nverts; i++) {
            const float *vp = d->verts + (size_t)d->indices[i] * d->vstride * 4;
            const float *p0 = (d->attr_mask & 1) ? vp : d->cur[0];
            if (p0[0] < mnx) mnx = p0[0];
            if (p0[0] > mxx) mxx = p0[0];
        }
        if (mnx <= 1.f && mxx >= 639.f) return 1.f;                                      /* a pass over the whole width is not an element */
    }
    return 0.75f;
}

void gpu_draw(const GpuDraw *d) {
    static int wtrace = -1;
    if (wtrace < 0) wtrace = getenv("STW_WIDETRACE") != NULL;
    if (wtrace && d->width == 640 && d->height == 480 && d->nverts >= 3) {     /* debugging aid: what kinds of draws make up the main scene (3D world, 2D HUD, full-screen passes) */
        uint64_t h = 1469598103934665603ull;
        int plen = 0;
        for (int i = d->vsh_start; i < 136; i++) { for (int k = 0; k < 4; k++) { h ^= d->vsh[i * 4 + k]; h *= 1099511628211ull; } plen++; if (d->vsh[i * 4 + 3] & 1) break; }
        float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f, mnw = 1e30f, mxw = -1e30f;
        for (uint32_t i = 0; i < d->nverts; i++) {
            const float *vp = d->verts + (size_t)d->indices[i] * d->vstride * 4;
            const float *p0 = (d->attr_mask & 1) ? vp : d->cur[0];
            if (p0[0] < mnx) mnx = p0[0]; if (p0[0] > mxx) mxx = p0[0];
            if (p0[1] < mny) mny = p0[1]; if (p0[1] > mxy) mxy = p0[1];
            if (p0[3] < mnw) mnw = p0[3]; if (p0[3] > mxw) mxw = p0[3];
        }
        { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); fprintf(stderr, "[t=%.3f] ", ts.tv_sec % 1000 + ts.tv_nsec / 1e9); }
        fprintf(stderr, "[wide] tgt=%#x vsh=%016llx@%d len=%d nv=%u z=%u/%u bl=%u v0x=[%g,%g] y=[%g,%g] w=[%g,%g] c-38=(%g %g %g %g) c-37=(%g %g %g %g) tex0=%d cull=%u st=%u\n", d->color_addr, (unsigned long long)h, d->vsh_start, plen, d->nverts,
                d->depth_enable, d->depth_write, d->blend_enable, mnx, mxx, mny, mxy, mnw, mxw,
                d->consts[(96 - 38) * 4], d->consts[(96 - 38) * 4 + 1], d->consts[(96 - 38) * 4 + 2], d->consts[(96 - 38) * 4 + 3],
                d->consts[(96 - 37) * 4], d->consts[(96 - 37) * 4 + 1], d->consts[(96 - 37) * 4 + 2], d->consts[(96 - 37) * 4 + 3], d->tex[0].enabled, d->cull_enable, d->stencil_enable);
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_DRAWTRACE") ? atoi(getenv("STW_DRAWTRACE")) : 0;
    if (trace > 1) {
        fprintf(stderr, "[draw+] verts=%u stages=%u ctl=%#x prog=%#x tex:", d->nverts, d->psh.control & 0xFF, d->psh.control, d->psh.stage_program);
        for (int i = 0; i < 4; i++) fprintf(stderr, " [%d en=%d f=%#x %ux%u @%#x]", i, d->tex[i].enabled, d->tex[i].format, d->tex[i].width, d->tex[i].height, d->tex[i].addr);
        fprintf(stderr, " cull=%u/%#x/%#x st=%u/%#x/%#x/%#x/%#x/%#x/%#x/%#x depth=%u/%#x blend=%u/%#x/%#x atest=%u cmask=%#x dwrite=%u eq=%#x\n", d->cull_enable, d->cull_face, d->front_face, d->stencil_enable, d->stencil_func, d->stencil_ref, d->stencil_rmask, d->stencil_wmask, d->stencil_fail, d->stencil_zfail, d->stencil_zpass, d->depth_enable, d->depth_func, d->blend_enable, d->blend_src, d->blend_dst, d->psh.alpha_test, d->color_mask, d->depth_write, d->blend_eq);
    }
    if (trace > 3 && (d->tex[0].format == 0x41 || d->psh.stage_program == 0x421)) {
        static int shown;
        if (shown++ < 4) {
            for (int i = 0; i < 4; i++) fprintf(stderr, "  tex%d en=%d fmt=%#x %ux%u @%#x pitch=%#x addr=%#x filter=%#x norm=%d\n", i, d->tex[i].enabled, d->tex[i].format, d->tex[i].width, d->tex[i].height, d->tex[i].addr, d->tex[i].pitch, d->tex[i].address, d->tex[i].filter, d->tex[i].normalized);
            fprintf(stderr, "  other=%#x bumpmat1=%08x %08x %08x %08x scale1=%08x off1=%08x bumpmat3=%08x %08x %08x %08x\n", d->psh.other_stage_input, d->psh.bump_mat[1][0], d->psh.bump_mat[1][1], d->psh.bump_mat[1][2], d->psh.bump_mat[1][3], d->psh.bump_scale[1], d->psh.bump_offset[1], d->psh.bump_mat[3][0], d->psh.bump_mat[3][1], d->psh.bump_mat[3][2], d->psh.bump_mat[3][3]);
            fprintf(stderr, "[psh] ctl=%#x prog=%#x final=%#x/%#x finalk=%#x/%#x\n", d->psh.control, d->psh.stage_program, d->psh.final0, d->psh.final1, d->psh.final_factor0, d->psh.final_factor1);
            for (int i = 0; i < (int)(d->psh.control & 0xFF); i++) fprintf(stderr, "  stage%d rgb_i=%08x rgb_o=%08x a_i=%08x a_o=%08x k0=%08x k1=%08x\n", i, d->psh.rgb_icw[i], d->psh.rgb_ocw[i], d->psh.alpha_icw[i], d->psh.alpha_ocw[i], d->psh.factor0[i], d->psh.factor1[i]);
        }
    }
    if (trace > 2 && d->tex[0].enabled && d->nverts >= 3) {
        for (int v = 0; v < 3; v++) {
            float a[64];                                       /* expand vertex v of the first triangle to all 16 attributes */
            const float *vp = d->verts + (size_t)d->indices[v] * d->vstride * 4;
            for (int i = 0; i < 16; i++) {
                if (d->attr_mask >> i & 1) memcpy(a + i * 4, vp + __builtin_popcount(d->attr_mask & ((1u << i) - 1)) * 4, 16);
                else memcpy(a + i * 4, d->cur[i], 16);
            }
            fprintf(stderr, "[vtx%d] v0=(%g %g %g %g) v3=(%g %g %g %g) v9=(%g %g %g %g) v10=(%g %g)\n", v, a[0], a[1], a[2], a[3],
                    a[12], a[13], a[14], a[15], a[36], a[37], a[38], a[39], a[40], a[41]);
        }
        fprintf(stderr, "[tex0] addr=%#x addrreg=%#x filter=%#x pitch=%#x\n", d->tex[0].addr, d->tex[0].address, d->tex[0].filter, d->tex[0].pitch);
    }
    if (trace == 1 || trace > 1) fprintf(stderr, "[draw] fmt=%#x clip=(%u,%u) color=%#x pitch=%#x %ux%u zeta=%#x verts=%u tex0=%d(%#x %ux%u @%#x) depth=%u blend=%u\n", d->surface_format, d->clip_x, d->clip_y, d->color_addr, d->color_pitch, d->width, d->height, d->zeta_addr, d->nverts, d->tex[0].enabled, d->tex[0].format, d->tex[0].width, d->tex[0].height, d->tex[0].addr, d->depth_enable, d->blend_enable);
    if (getenv("STW_MODES")) {                              /* debugging aid: tally the state combinations the game uses, printed every 5000 draws */
        static struct { char key[400]; unsigned n; } tab[512]; static int nt; static unsigned total;
        char key[400];
        int pos = snprintf(key, sizeof key, "prog=%x", d->psh.stage_program);
        for (int i = 0; i < 4; i++) if (d->tex[i].enabled) pos += snprintf(key + pos, sizeof key - pos, " t%d[f%x fl%x ad%x]", i, d->tex[i].format, d->tex[i].filter >> 16, d->tex[i].address);
        pos += snprintf(key + pos, sizeof key - pos, " cull%u/%x/%x st%u(%x,%x,%x/%x/%x/%x/%x) fog%u sh%x po%u(%g,%g) z%u/%x/%u b%u/%x/%x/%x at%u/%x cm%x", d->cull_enable, d->cull_face, d->front_face,
                         d->stencil_enable, d->stencil_func, d->stencil_ref, d->stencil_fail, d->stencil_zfail, d->stencil_zpass, d->stencil_wmask, d->stencil_rmask,
                         d->psh.fog_enable, d->shade_mode, d->poly_off_fill, d->poly_off_scale, d->poly_off_bias, d->depth_enable, d->depth_func, d->depth_write,
                         d->blend_enable, d->blend_src, d->blend_dst, d->blend_eq, d->psh.alpha_test, d->psh.alpha_func, d->color_mask);
        int k = 0;
        for (; k < nt; k++) if (!strcmp(tab[k].key, key)) break;
        if (k == nt && nt < 512) { strcpy(tab[nt].key, key); tab[nt++].n = 0; }
        if (k < 512) tab[k].n++;
        if (++total % 5000 == 0) { for (int i = 0; i < nt; i++) fprintf(stderr, "[modes] %u %s\n", tab[i].n, tab[i].key); fprintf(stderr, "[modes] ----\n"); }
    }
    if (!gpu_ready() || !d->width || !d->height || !d->color_pitch) return;
    static int nobloom = -1;                                /* STW_NOBLOOM=1: debugging aid, skip additive depth-less full-screen passes */
    if (nobloom < 0) nobloom = getenv("STW_NOBLOOM") != NULL;
    if (nobloom && !d->depth_enable && d->blend_enable && d->blend_src == 1 && d->blend_dst == 1) return;
    pthread_mutex_lock(&gpu_lock);
    @autoreleasepool {
        id<MTLRenderPipelineState> pso = get_pso(d, ms_on(d->width, d->height) ? gmsaa : 1);
        id<MTLTexture> ct = color_target(d);
        id<MTLTexture> dt = depth_target(d->zeta_addr, d->width, d->height, 1);
        size_t vlen = ((size_t)d->nuniq * d->vstride * 16 + 15) & ~(size_t)15, ilen = (size_t)d->nverts * 4;
        if (!pso || !ct || vlen + ilen > VRING_SIZE) { pthread_mutex_unlock(&gpu_lock); return; }

        static int stat_stencil = -1;                         /* STW_STENCILSTAT=1: histogram of stencil values before each stencil-tested colour draw */
        if (stat_stencil < 0) stat_stencil = getenv("STW_STENCILSTAT") != NULL;
        if (stat_stencil && d->stencil_enable && d->color_mask && dt) {
            flush_locked(1);
            size_t bpr = SCL(d->width), n = bpr * SCL(d->height);
            id<MTLBuffer> sb = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> bcb = [queue commandBuffer];
            id<MTLBlitCommandEncoder> be = [bcb blitCommandEncoder];
            [be copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(SCL(d->width), SCL(d->height), 1)
                       toBuffer:sb destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:n options:MTLBlitOptionStencilFromDepthStencil];
            [be endEncoding]; [bcb commit]; [bcb waitUntilCompleted];
            unsigned hist[256] = {0}; const uint8_t *sp = sb.contents;
            for (size_t i = 0; i < n; i++) hist[sp[i]]++;
            fprintf(stderr, "[stencil] func=%#x ref=%u verts=%u:", d->stencil_func, d->stencil_ref, d->nverts);
            for (int i = 0; i < 256; i++) if (hist[i]) fprintf(stderr, " %d:%u", i, hist[i]);
            fprintf(stderr, "\n");
        }
        /* resolve textures first: aliasing a target written in this command buffer needs a pass boundary */
        id<MTLTexture> texs[4];
        PshConsts k;
        memset(&k, 0, sizeof k);
        int barrier = 0;
        dbg_d = d; cur_ct = ct;
        for (int i = 0; i < 4; i++) {
            int mode = (d->psh.stage_program >> (i * 5)) & 0x1F;
            int use = d->tex[i].enabled && mode != 0 && mode != 4;
            k.tex_size[i][0] = (float)d->tex[i].width; k.tex_size[i][1] = (float)d->tex[i].height;
            k.tex_flags[i][0] = d->tex[i].normalized; k.tex_flags[i][1] = use;
            texs[i] = use ? get_texture(&d->tex[i], &barrier) : white_tex;
            /* a low-resolution render target (bloom chain) sampled by a larger pass: smooth bicubic upsampling instead of blocky bilinear */
            k.tex_flags[i][2] = (use && tex_was_alias && q_bloomhq && gscale > 4 && d->width > d->tex[i].width && d->tex[i].width <= 320) ? 1 : 0;
        }
        if (vring_off + vlen + ilen > VRING_SIZE) wait_gpu(3);   /* GPU may still be reading the old ring contents */
        if (barrier) force_new_encoder = 1;

        if (!enc || enc_color != ct || enc_depth != dt || force_new_encoder) {
            end_encoder();
            open_cb();
            id<MTLTexture> ctex = ct, rtex = nil;
            if (ms_on(d->width, d->height)) {                    /* MSAA: draw into the multisampled twin and resolve into hi when the pass ends */
                Target *tgm = find_target(d->color_addr);
                id<MTLTexture> m = tgm && tgm->hi == ct ? ensure_ms(tgm) : nil;
                if (!m) { fprintf(stderr, "stw: MSAA target unavailable, turning MSAA off\n"); gmsaa = 1; [depth_cache removeAllObjects]; pthread_mutex_unlock(&gpu_lock); return; }
                ctex = m; rtex = ct;
                open_cb();                                       /* ensure_ms may have ended the encoder and submitted work */
            }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ctex;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            if (rtex) { rp.colorAttachments[0].resolveTexture = rtex; rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
            rp.depthAttachment.texture = dt;
            rp.depthAttachment.loadAction = MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = dt;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            enc = [cb renderCommandEncoderWithDescriptor:rp];
            static int noclamp = -1;                              /* STW_NOCLAMP=1: debugging aid, clip at the near/far planes again */
            if (noclamp < 0) noclamp = getenv("STW_NOCLAMP") != NULL;
            /* NV2A does not clip primitives against the near/far planes, it clamps depth (xemu enables GL_DEPTH_CLAMP). Zfail shadow
             * volumes depend on it: clipped at the near plane they lose their caps and the stencil parity comes out wrong. */
            if (!noclamp) [enc setDepthClipMode:MTLDepthClipModeClamp];
            enc_color = ct; enc_depth = dt;
            force_new_encoder = 0;
        }
        Target *tg = find_target(d->color_addr);
        if (tg) { tg->written = 1; tg->dirty = 1; }

        [enc setRenderPipelineState:pso];
        static int nodepth = -1;                              /* STW_NODEPTH=1: debugging aid, ignore the depth test */
        if (nodepth < 0) nodepth = getenv("STW_NODEPTH") != NULL;
        GpuDraw dcopy = *d;
        if (nodepth) dcopy.depth_enable = 0;
        d = &dcopy;
        uint64_t dk = h64w(h64w(1, d->depth_enable ? d->depth_func : 0x207), d->depth_enable && d->depth_write);
        if (d->stencil_enable) {
            dk = h64w(h64w(h64w(h64w(dk, d->stencil_func), d->stencil_rmask), d->stencil_wmask), d->stencil_fail);
            dk = h64w(h64w(dk, d->stencil_zfail), d->stencil_zpass);
        }
        NSNumber *dkey = @(dk);
        id<MTLDepthStencilState> ds = ds_cache[dkey];
        if (!ds) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = d->depth_enable ? cmp_func(d->depth_func) : MTLCompareFunctionAlways;
            dd.depthWriteEnabled = d->depth_enable && d->depth_write;
            if (d->stencil_enable) {
                MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
                sd.stencilCompareFunction = cmp_func(d->stencil_func);
                sd.stencilFailureOperation = stencil_op(d->stencil_fail);
                sd.depthFailureOperation = stencil_op(d->stencil_zfail);
                sd.depthStencilPassOperation = stencil_op(d->stencil_zpass);
                sd.readMask = d->stencil_rmask; sd.writeMask = d->stencil_wmask;
                dd.frontFaceStencil = dd.backFaceStencil = sd;
            }
            ds = [dev newDepthStencilStateWithDescriptor:dd];
            ds_cache[dkey] = ds;
        }
        [enc setDepthStencilState:ds];
        [enc setStencilReferenceValue:d->stencil_ref];
        [enc setFrontFacingWinding:d->front_face == 0x901 ? MTLWindingCounterClockwise : MTLWindingClockwise];
        if (d->poly_off_fill) [enc setDepthBias:d->poly_off_bias slopeScale:d->poly_off_scale clamp:0];   /* SET_POLYGON_OFFSET_*: GL-style units/slope */
        else [enc setDepthBias:0 slopeScale:0 clamp:0];
        [enc setCullMode:!d->cull_enable ? MTLCullModeNone : d->cull_face == 0x404 ? MTLCullModeFront : MTLCullModeBack];
        if (d->cull_enable && d->cull_face == 0x408) { pthread_mutex_unlock(&gpu_lock); return; }   /* FRONT_AND_BACK: nothing is drawn */
        {   /* window coordinates are absolute; only the clip rectangle may be written */
            uint32_t cw = d->clip_w ? d->clip_w : d->width, ch = d->clip_h ? d->clip_h : d->height;
            MTLScissorRect sr = { d->clip_x, d->clip_y, cw, ch };
            if (sr.x + sr.width > d->width) sr.width = d->width - sr.x;
            if (sr.y + sr.height > d->height) sr.height = d->height - sr.y;
            { uint32_t ex = SCL(sr.x + sr.width), ey = SCL(sr.y + sr.height); sr.x = SCL(sr.x); sr.y = SCL(sr.y); sr.width = ex - sr.x; sr.height = ey - sr.y; }
            [enc setScissorRect:sr];
        }
        /* Vertex programs emit logical window coordinates; with SET_SURFACE_FORMAT antialiasing (1 = 2x1, 2 = 2x2) the buffer holds
         * that many samples per pixel and the hardware scales the coordinates. The game draws its stencil shadows in 2x2 mode and its
         * main scene in 1x, so without this the shadows land at a fraction of their size in the top-left corner. */
        uint32_t aamode = (d->surface_format >> 12) & 0xF;
        float surf[8] = { (float)d->width, (float)d->height, d->clip_min, d->clip_max, aamode >= 1 ? 2.f : 1.f, aamode >= 2 ? 2.f : 1.f, ui_squeeze(d), 0.f };
        size_t voff = vring_off, ioff = vring_off + vlen;
        memcpy((uint8_t *)vring.contents + voff, d->verts, (size_t)d->nuniq * d->vstride * 16);
        memcpy((uint8_t *)vring.contents + ioff, d->indices, ilen);
        vring_off = (ioff + ilen + 255) & ~(size_t)255;
        [enc setVertexBuffer:vring offset:voff atIndex:0];
        struct { uint32_t mask, stride, pad[2]; float cur[16][4]; } vk = { d->attr_mask, d->vstride };
        memcpy(vk.cur, d->cur, sizeof vk.cur);
        [enc setVertexBytes:&vk length:sizeof vk atIndex:3];
        [enc setVertexBytes:d->consts length:192 * 16 atIndex:1];
        [enc setVertexBytes:surf length:sizeof surf atIndex:2];
        for (int i = 0; i < 9; i++) {                       /* ARGB words -> RGBA floats */
            uint32_t w0 = i < 8 ? d->psh.factor0[i] : d->psh.final_factor0, w1 = i < 8 ? d->psh.factor1[i] : d->psh.final_factor1;
            k.c0[i][0] = ((w0 >> 16) & 255) / 255.f; k.c0[i][1] = ((w0 >> 8) & 255) / 255.f; k.c0[i][2] = (w0 & 255) / 255.f; k.c0[i][3] = (w0 >> 24) / 255.f;
            k.c1[i][0] = ((w1 >> 16) & 255) / 255.f; k.c1[i][1] = ((w1 >> 8) & 255) / 255.f; k.c1[i][2] = (w1 & 255) / 255.f; k.c1[i][3] = (w1 >> 24) / 255.f;
        }
        { uint32_t f = d->psh.fog_color;                   /* SET_FOG_COLOR: R in the low byte */
          k.fog[0] = (f & 255) / 255.f; k.fog[1] = ((f >> 8) & 255) / 255.f; k.fog[2] = ((f >> 16) & 255) / 255.f; k.fog[3] = (f >> 24) / 255.f; }
        k.alpha_ref = d->psh.alpha_ref;
        for (int i = 0; i < 4; i++) {
            const uint32_t *w = d->psh.bump_mat[i];            /* words arrive as m00, m01, m11, m10 */
            uint32_t m[4] = { w[0], w[1], w[3], w[2] };       /* -> m00, m01, m10, m11 */
            memcpy(k.bump_mat[i], m, 16);
            memcpy(&k.bump_scale[i][0], &d->psh.bump_scale[i], 4);
            memcpy(&k.bump_offset[i][0], &d->psh.bump_offset[i], 4);
        }
        for (int i = 0; i < 4; i++) {
            [enc setFragmentTexture:texs[i] atIndex:i];
            [enc setFragmentSamplerState:get_sampler(&d->tex[i]) atIndex:i];
        }
        [enc setFragmentBytes:&k length:sizeof k atIndex:0];
        if (d->blend_enable) {
            float a = (d->blend_color >> 24) / 255.f, r = ((d->blend_color >> 16) & 255) / 255.f, g = ((d->blend_color >> 8) & 255) / 255.f, b = (d->blend_color & 255) / 255.f;
            [enc setBlendColorRed:r green:g blue:b alpha:a];
        }
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:d->nverts indexType:MTLIndexTypeUInt32 indexBuffer:vring indexBufferOffset:ioff];
        if (++draws_in_cb >= 400) flush_locked(0);          /* keep the GPU fed without waiting */
    }
    pthread_mutex_unlock(&gpu_lock);
}
