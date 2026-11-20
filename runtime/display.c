/* Scanout state shared by the AV kernel calls and the CRTC model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "nv2a.h"
#include "rt.h"

static struct { uint32_t mode, format, pitch, fb, start; } disp;

void display_set_mode(uint32_t mode, uint32_t format, uint32_t pitch, uint32_t fb) {
    disp.mode = mode; disp.format = format; disp.pitch = pitch; disp.fb = fb; disp.start = fb;
}

void display_set_start(uint32_t fb) {
    static int trace = -1;
    if (trace < 0) trace = getenv("STW_DISPTRACE") != NULL;
    disp.start = fb;
    if (trace) fprintf(stderr, "[disp] flip start=%#x (mode fb=%#x pitch=%#x fmt=%#x)\n", fb, disp.fb, disp.pitch, disp.format);
}

/* PVIDEO overlay (the XMV movie player uses it): a YUY2 surface scaled over the framebuffer. D3DDevice_UpdateOverlay /
 * EnableOverlay program OFFSET/SIZE_IN/POINT_IN/DS_DX/DT_DY/POINT_OUT/SIZE_OUT/FORMAT; BUFFER bit 0 selects the live buffer
 * and STOP disables the overlay. No colour key is modelled (D3D leaves FORMAT.DISPLAY clear = overlay always wins). */
static uint32_t composed[640 * 480];
static int overlay_active(void) {
    return (LD32(NV2A_BASE + 0x8700) & 1) && !(LD32(NV2A_BASE + 0x8704) & 1) && LD32(NV2A_BASE + 0x8950);
}
static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void overlay_compose(uint32_t fb, uint32_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h && y < 480; y++) memcpy(&composed[y * 640], G + fb + y * pitch, (w < 640 ? w : 640) * 4);
    uint32_t base = LD32(NV2A_BASE + 0x8900), off = LD32(NV2A_BASE + 0x8920), size_in = LD32(NV2A_BASE + 0x8928);
    uint32_t pt_in = LD32(NV2A_BASE + 0x8930), dsdx = LD32(NV2A_BASE + 0x8938), dtdy = LD32(NV2A_BASE + 0x8940);
    uint32_t pt_out = LD32(NV2A_BASE + 0x8948), size_out = LD32(NV2A_BASE + 0x8950), fmt = LD32(NV2A_BASE + 0x8958);
    uint32_t src = (base + off + (getenv("STW_OVOFF") ? (uint32_t)strtoul(getenv("STW_OVOFF"), NULL, 0) : 0)) & 0x03FFFFFFu, spitch = fmt & 0x1FFF;
    uint32_t ow = size_out & 0xFFFF, oh = size_out >> 16, ox = pt_out & 0xFFFF, oy = pt_out >> 16;
    uint32_t iw = size_in & 0xFFFF, ih = size_in >> 16, ix = pt_in & 0xFFFF, iy = pt_in >> 16;
    int uyvy = ((fmt >> 16) & 3) == 2;
    if (!ow || !oh || !iw || !ih || !spitch) return;
    for (uint32_t y = 0; y < oh && oy + y < 480; y++) {
        uint32_t sy = iy + (uint32_t)(((uint64_t)y * dtdy) >> 20);
        if (sy >= iy + ih) sy = iy + ih - 1;
        for (uint32_t x = 0; x < ow && ox + x < 640; x++) {
            uint32_t sx = ix + (uint32_t)(((uint64_t)x * dsdx) >> 20);
            if (sx >= ix + iw) sx = ix + iw - 1;
            uint32_t q = LD32(src + sy * spitch + (sx & ~1u) * 2);
            int Y = (uyvy ? ((q >> ((sx & 1) ? 24 : 8)) & 255) : ((q >> ((sx & 1) ? 16 : 0)) & 255)) - 16;
            int U = (uyvy ? (q & 255) : ((q >> 8) & 255)) - 128, V = (uyvy ? ((q >> 16) & 255) : (q >> 24)) - 128;
            int r = (298 * Y + 409 * V + 128) >> 8, g = (298 * Y - 100 * U - 208 * V + 128) >> 8, b = (298 * Y + 516 * U + 128) >> 8;
            composed[(oy + y) * 640 + ox + x] = 0xFF000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
        }
    }
}

/* Called once per vblank (from the interrupt thread, holding the guest lock): show the scanout buffer. */
int nv2a_surface_nth(int i, uint32_t *addr, uint32_t *w, uint32_t *h, uint32_t *pitch);
int nv2a_surface_at(uint32_t addr, uint32_t *w, uint32_t *h, uint32_t *pitch);
void gpu_sync(void);
void gpu_wait_idle(void);
void *gpu_scanout_texture(uint32_t addr, uint32_t w, uint32_t h, uint32_t *tw, uint32_t *th);
void *gpu_read_hi(uint32_t addr, uint32_t *tw, uint32_t *th);
void display_vblank(void) {
    gpu_wait_idle();                                         /* the scanout must see finished rendering */
    static int headless = -1;
    if (headless < 0) headless = getenv("STW_HEADLESS") != NULL;
    uint32_t fb = disp.start;
    if (fb == 0 || fb >= 0x04000000u) fb = disp.fb & 0x03FFFFFFu;     /* start not (yet) meaningful: use the mode's buffer */
    else if (fb & 0xFFF) fb = (fb + 0xFFF) & ~0xFFFu;                  /* interlace field offset: the surface is 4 KB aligned */
    uint32_t w = 640, h = 480, pitch = 640 * 4;
    nv2a_surface_at(fb, &w, &h, &pitch);
    const char *dump = getenv("STW_DUMPFRAME");              /* debugging aid: write the shown frame as a PPM about once a second */
    const char *dump_hi = getenv("STW_DUMPFRAME_HI");        /* same for the scaled (internal resolution) image */
    static int tick;
    static int every = -1, from = -1;
    if (every < 0) every = getenv("STW_DUMPEVERY") ? atoi(getenv("STW_DUMPEVERY")) : 60;   /* STW_DUMPEVERY=6: a dump every 6 frames instead of every 60 */
    if (from < 0) from = getenv("STW_DUMPFROM") ? atoi(getenv("STW_DUMPFROM")) * 60 : 0;   /* STW_DUMPFROM=<sec>: no dumps before that second */
    int dump_tick = tick >= from && (dump || dump_hi || getenv("STW_DUMPALL")) && (tick + 1) % every == 0;
    int overlay = overlay_active() && w == 640 && h == 480;
    uint32_t tw = 0, th = 0;
    void *hitex = (!overlay && !headless) ? gpu_scanout_texture(fb, w, h, &tw, &th) : NULL;   /* scaled image straight from the GPU */
    if (overlay || dump_tick || (!hitex && !headless)) gpu_sync();   /* the CPU reads the 1x surface: make it current */
    const uint8_t *shown = G + fb;                           /* what is scanned out: the framebuffer, or it with the video overlay on top */
    if (overlay) { overlay_compose(fb, pitch, w, h); shown = (const uint8_t *)composed; pitch = 640 * 4; }
    if (dump_hi && dump_tick) {
        uint32_t hw, hh;
        uint32_t *hp = gpu_read_hi(fb, &hw, &hh);
        if (hp) {
            char name[512];
            snprintf(name, sizeof name, dump_hi, (tick + 1) / 60);
            FILE *f = fopen(name, "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", hw, hh);
                for (uint32_t i = 0; i < hw * hh; i++) { fputc((hp[i] >> 16) & 255, f); fputc((hp[i] >> 8) & 255, f); fputc(hp[i] & 255, f); }
                fclose(f);
            }
            free(hp);
        }
    }
    ++tick;
    if (getenv("STW_SCALESCRIPT") && tick % 60 == 0) {        /* debugging aid: "40:0;70:3" switches the resolution preset (0 original .. 4 2160p) at those seconds (what F2 does) */
        for (const char *q = getenv("STW_SCALESCRIPT"); *q;) {
            int at = (int)strtol(q, (char **)&q, 10), sc = 1;
            if (*q == ':') sc = (int)strtol(q + 1, (char **)&q, 10);
            if (*q == ';') q++;
            if (at == tick / 60) { void gpu_set_res(int); gpu_set_res(sc); fprintf(stderr, "[res] -> preset %d\n", sc); }
        }
    }
    if (dump && tick >= from && tick % every == 0) {
        char name[512];
        snprintf(name, sizeof name, dump, every == 60 ? tick / 60 : tick / every);       /* a %d in the path numbers the frames */
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t px = *(const uint32_t *)(shown + y * pitch + x * 4);
                    fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f);
                }
            fclose(f);
        }
    }
    if (getenv("STW_DUMPALL") && tick % 60 == 0 && tick / 60 == atoi(getenv("STW_DUMPALL"))) {   /* (gpu_sync above made every 1x surface current) */   /* every known render target at second N */
        uint32_t a, sw, sh, sp;
        for (int i = 0; nv2a_surface_nth(i, &a, &sw, &sh, &sp); i++) {
            char name[256];
            snprintf(name, sizeof name, "/tmp/surf_%02d_%x_%ux%u.ppm", i, a, sw, sh);
            FILE *f = fopen(name, "wb");
            if (!f) continue;
            fprintf(f, "P6\n%u %u\n255\n", sw, sh);
            for (uint32_t y = 0; y < sh; y++)
                for (uint32_t xx = 0; xx < sw; xx++) { uint32_t px = LD32(a + y * sp + xx * 4); fputc((px >> 16) & 255, f); fputc((px >> 8) & 255, f); fputc(px & 255, f); }
            fclose(f);
        }
    }
    if (!headless) {
        extern int gpu_widescreen(void);
        display_set_aspect(!overlay && gpu_widescreen() ? 16.0 / 9.0 : 4.0 / 3.0);       /* movies (video overlay) are 4:3 pictures: pillarboxed */
        if (hitex) display_present_tex(hitex, tw, th);
        else display_present_ptr(shown, pitch, w, h);
    }
}
