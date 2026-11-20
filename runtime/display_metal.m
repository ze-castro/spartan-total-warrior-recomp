/* Host display: a Cocoa window with a CAMetalLayer. The main thread runs the Cocoa event loop; the
 * guest runs on its own thread. Each vblank the scanout buffer is copied from guest RAM into a
 * texture and drawn scaled to the window. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/stat.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdio.h>
#include "display.h"
#include "rt.h"
#include "input.h"
#include "paths.h"
#include "xiso.h"
#include "prepare.h"
#import "launcher_pad.h"
#include "gpu.h"

static NSWindow *window;
static CAMetalLayer *layer;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pso;
static id<MTLTexture> tex;
static uint32_t tex_w, tex_h;
static void (*guest_start)(void);
static int hud_on;

static NSString *const kShader =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut vs(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VOut o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
     "fragment float4 fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  return float4(t.sample(s, in.uv).rgb, 1.0); }\n";

/* Quality filters on the final image (launcher: Anti-aliasing = FXAA, Sharpening), applied to the internal-resolution texture before it is scaled to the window. */
static NSString *const kFilterShader =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "vertex VOut fvs(uint vid [[vertex_id]]) {\n"
     "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "  VOut o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = float2(p.x, 1.0 - p.y); return o; }\n"
     "float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
     /* FXAA (Lottes' compact edge-directed blur): follows the local edge direction and averages along it, keeping flat areas untouched */
     "fragment float4 fs_fxaa(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
     "  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
     "  float2 px = 1.0 / float2(t.get_width(), t.get_height());\n"
     "  float2 uv = in.uv;\n"
     "  float3 rgbNW = t.sample(s, uv + float2(-1, -1) * px).rgb, rgbNE = t.sample(s, uv + float2(1, -1) * px).rgb;\n"
     "  float3 rgbSW = t.sample(s, uv + float2(-1, 1) * px).rgb, rgbSE = t.sample(s, uv + float2(1, 1) * px).rgb;\n"
     "  float3 rgbM = t.sample(s, uv).rgb;\n"
     "  float lNW = lum(rgbNW), lNE = lum(rgbNE), lSW = lum(rgbSW), lSE = lum(rgbSE), lM = lum(rgbM);\n"
     "  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE))), lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));\n"
     "  if (lMax - lMin < max(0.03, lMax * 0.125)) return float4(rgbM, 1.0);\n"
     "  float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));\n"
     "  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);\n"
     "  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);\n"
     "  dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;\n"
     "  float3 rgbA = 0.5 * (t.sample(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb + t.sample(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
     "  float3 rgbB = rgbA * 0.5 + 0.25 * (t.sample(s, uv + dir * -0.5).rgb + t.sample(s, uv + dir * 0.5).rgb);\n"
     "  float lB = lum(rgbB);\n"
     "  return float4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0); }\n"
     /* Contrast-adaptive sharpening (after AMD FidelityFX CAS): per-channel sharpen weight shrinks where the local contrast is already high */
     "fragment float4 fs_cas(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], constant float &strength [[buffer(0)]]) {\n"
     "  constexpr sampler s(filter::nearest, address::clamp_to_edge);\n"
     "  float2 sz = float2(t.get_width(), t.get_height());\n"
     "  int2 p = int2(in.uv * sz);\n"
     "  int2 mxp = int2(sz) - 1;\n"
     "  float3 e = t.read(uint2(p)).rgb;\n"
     "  float3 b = t.read(uint2(clamp(p + int2(0, -1), int2(0), mxp))).rgb, d = t.read(uint2(clamp(p + int2(-1, 0), int2(0), mxp))).rgb;\n"
     "  float3 f = t.read(uint2(clamp(p + int2(1, 0), int2(0), mxp))).rgb, h = t.read(uint2(clamp(p + int2(0, 1), int2(0), mxp))).rgb;\n"
     "  float3 mn = min(min(min(d, e), min(f, b)), h), mx = max(max(max(d, e), max(f, b)), h);\n"
     "  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-4)));\n"
     "  float3 w = amp * (-1.0 / mix(8.0, 5.0, strength));\n"
     "  float3 rcpW = 1.0 / (1.0 + 4.0 * w);\n"
     "  return float4(saturate(((b + d + f + h) * w + e) * rcpW), 1.0); }\n";
static id<MTLRenderPipelineState> pso_fxaa, pso_cas;
static id<MTLTexture> filt_tex[2];                           /* scratch targets the size of the source */
static int q_fxaa, q_sharpen;                               /* quality settings used by the presenter */

static void setup_metal(void) {
    device = MTLCreateSystemDefaultDevice();
    queue = [device newCommandQueue];
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:kShader options:nil error:&err];
    if (!lib) { fprintf(stderr, "stw: shader compile failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) { fprintf(stderr, "stw: pipeline failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
    id<MTLLibrary> flib = [device newLibraryWithSource:kFilterShader options:nil error:&err];
    if (!flib) { fprintf(stderr, "stw: filter shaders failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
    for (int i = 0; i < 2; i++) {
        MTLRenderPipelineDescriptor *fd = [MTLRenderPipelineDescriptor new];
        fd.vertexFunction = [flib newFunctionWithName:@"fvs"];
        fd.fragmentFunction = [flib newFunctionWithName:i ? @"fs_cas" : @"fs_fxaa"];
        fd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> fp = [device newRenderPipelineStateWithDescriptor:fd error:&err];
        if (!fp) { fprintf(stderr, "stw: filter pipeline failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
        if (i) pso_cas = fp; else pso_fxaa = fp;
    }
}
/* Quality choices: read when the first frame is presented, i.e. after the launcher; STW_AA=0..3 and STW_SHARPEN=0..100 override the saved settings. */
static void load_quality(void) {
    static int done;
    if (done) return;
    done = 1;
    StwSettings qs = stw_settings_get();
    int aa = getenv("STW_AA") ? atoi(getenv("STW_AA")) : qs.aa;
    q_fxaa = aa == 1;
    q_sharpen = getenv("STW_SHARPEN") ? atoi(getenv("STW_SHARPEN")) : qs.sharpen;
    if (q_sharpen < 0) q_sharpen = 0;
    if (q_sharpen > 100) q_sharpen = 100;
}



/* The resolution presets: the console's own 480 lines, then the common 720p, 1080p, 1440p and 2160p. The game renders 4:3 pictures; in widescreen the same lines are
 * shown 16:9 (the picture is anamorphic and stretched by the presenter). */
static NSString *res_text(int idx, int wide) {
    static const int lines[5] = { 480, 720, 1080, 1440, 2160 };
    static const char *names[5] = { "Original", "720p", "1080p", "1440p", "2160p" };
    int h = lines[idx];
    int w = wide ? (h * 16 + 4) / 9 : h * 4 / 3;
    return [NSString stringWithFormat:@"%s   %d x %d", names[idx], w, h];
}
static int stw_wide_now(void) { return stw_settings_get().widescreen; }

/* Actions on the F-keys: F1 / F2 previous / next internal resolution, F3 Metal HUD, F10 full screen. */
static NSString *base_title(void) { return @"Spartan: Total Warrior (native recomp)"; }
static void flash_title(NSString *t) {
    window.title = t;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ if ([window.title isEqualToString:t]) window.title = base_title(); });
}
static void act_scale(int dir) {
    int s = gpu_res() + dir;
    if (s < 0) s = 0;
    if (s > 4) s = 4;
    gpu_set_res(s);
    StwSettings c = stw_settings_get(); c.scale = s; stw_settings_put(&c);
    flash_title([NSString stringWithFormat:@"Internal resolution %@", res_text(s, stw_wide_now())]);
}
/* The Metal HUD is loaded by the system when MTL_HUD_ENABLED=1 is in the environment at launch (the launcher re-launches the game with it set);
 * it then adds an "Enable HUD" item to the app's menu, which is what F3 presses. */
static NSMenuItem *find_hud_item(NSMenu *m) {
    for (NSMenuItem *i in m.itemArray) {
        if (!i.submenu && i.action && [NSStringFromSelector(i.action) isEqualToString:@"toggleEnabled:"]) return i;   /* the system's "Enable HUD" item */
        NSMenuItem *r = i.submenu ? find_hud_item(i.submenu) : nil;
        if (r) return r;
    }
    return nil;
}
static void act_hud(void) {
    NSMenuItem *it = find_hud_item(NSApp.mainMenu);
    if (it) {
        [NSApp sendAction:it.action to:it.target from:it];
        hud_on = !hud_on;
        StwSettings c = stw_settings_get(); c.hud = hud_on; stw_settings_put(&c);
        flash_title(hud_on ? @"Metal HUD on" : @"Metal HUD off");
    } else {
        flash_title(@"Metal HUD is not available");
    }
}
static void act_full(void) {
    int was = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    [window toggleFullScreen:nil];
    StwSettings c = stw_settings_get(); c.fullscreen = !was; stw_settings_put(&c);
}

@interface StwWindow : NSWindow
@end
@implementation StwWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (void)keyDown:(NSEvent *)e {
    if (getenv("STW_INPUTLOG")) fprintf(stderr, "[key] down %d repeat=%d\n", e.keyCode, e.isARepeat);
    if (!e.isARepeat) input_log("key down %d", e.keyCode);
    if (!e.isARepeat) {
        if (e.keyCode == 122) { act_scale(-1); return; }                                 /* F1: previous resolution */
        if (e.keyCode == 120) { act_scale(1); return; }                                  /* F2: next resolution */
        if (e.keyCode == 99) { act_hud(); return; }                                      /* F3: Metal HUD */
        if (e.keyCode == 109) { act_full(); return; }                                    /* F10: full screen */
    } else return;
    input_key(e.keyCode, 1);
}
- (void)keyUp:(NSEvent *)e { input_log("key up %d", e.keyCode); input_key(e.keyCode, 0); }
- (void)flagsChanged:(NSEvent *)e {
    NSEventModifierFlags f = e.modifierFlags;
    input_mods(((f & NSEventModifierFlagShift) ? MOD_SHIFT : 0) | ((f & NSEventModifierFlagCommand) ? MOD_CMD : 0) |
               ((f & NSEventModifierFlagControl) ? MOD_CTRL : 0) | ((f & NSEventModifierFlagOption) ? MOD_OPT : 0));
}
- (void)mouseDown:(NSEvent *)e { (void)e; if (!input_mouselook()) input_set_mouselook(1); input_mouse_button(0, 1); }          /* the mouse is always captured while the window has focus */
- (void)mouseUp:(NSEvent *)e { (void)e; input_mouse_button(0, 0); }
- (void)rightMouseDown:(NSEvent *)e { (void)e; input_mouse_button(1, 1); }
- (void)rightMouseUp:(NSEvent *)e { (void)e; input_mouse_button(1, 0); }
- (void)otherMouseDown:(NSEvent *)e { input_mouse_button((int)e.buttonNumber, 1); }
- (void)otherMouseUp:(NSEvent *)e { input_mouse_button((int)e.buttonNumber, 0); }
- (void)otherMouseDragged:(NSEvent *)e { input_mouse_move(e.deltaX, e.deltaY); }
- (void)scrollWheel:(NSEvent *)e {
    double dy = e.scrollingDeltaY;
    if (e.hasPreciseScrollingDeltas) dy /= 12;                                         /* trackpad pixels -> wheel-notch units */
    input_scroll(dy);
}
- (void)mouseMoved:(NSEvent *)e { input_mouse_move(e.deltaX, e.deltaY); }
- (void)mouseDragged:(NSEvent *)e { input_mouse_move(e.deltaX, e.deltaY); }
- (void)rightMouseDragged:(NSEvent *)e { input_mouse_move(e.deltaX, e.deltaY); }
- (void)becomeKeyWindow { [super becomeKeyWindow]; input_set_mouselook(1); }
- (void)resignKeyWindow { input_set_mouselook(0); input_release_all(); [super resignKeyWindow]; }                     /* Cmd+Tab away frees the cursor */
@end


/* ---- start-up launcher: resolution, mouse-look sensitivity, Metal HUD, full screen ----
 * Shown before the game window opens and the guest starts. Choices are saved in stw_settings.txt. Skipped when "Show at startup" is
 * unticked (hold Option while the game starts to bring it back) or STW_NOLAUNCHER is set. STW_RES (if set) preselects the resolution. */
@interface StwLauncher : NSObject
@property (strong) NSPanel *panel;
@property (strong) NSPopUpButton *res, *aspect;
@property (strong) NSSlider *sens;
@property (strong) NSTextField *sensLabel;
@property (strong) NSButton *hud, *full, *show;
@property (strong) NSMutableArray<NSPopUpButton *> *rowButtons;
@property (strong) id keyMonitor;
@property (assign) int recording;
@property (strong) NSPopUpButton *aaPop;
@property (strong) NSSlider *sharpSlider;
@property (strong) NSTextField *sharpLabel;
@property (strong) NSButton *bloomChk;
@property (strong) NSTextField *gameStatus;
@property (strong) StwPrepareView *prep;
@property (strong) NSButton *chooseBtn, *playBtn, *cancelBtn;
@property (strong) NSProgressIndicator *progress;
@property (assign) BOOL extracting;
@property (assign) volatile BOOL cancelExtract;
@end
@implementation StwLauncher
static double sens_from_slider(double t) { return 0.0333 * pow(60.0, t); }              /* 0..1 -> 25%..1500%; 100% = the default 0.1333 */
static double slider_from_sens(double v) { return log(v / 0.0333) / log(60.0); }
- (void)aspectChanged:(id)sender {
    (void)sender;
    int wide = (int)self.aspect.indexOfSelectedItem == 1;
    for (int s = 0; s <= 4; s++) [self.res itemAtIndex:s].title = res_text(s, wide);
    [self.res selectItemAtIndex:self.res.indexOfSelectedItem];
}
- (void)sliderChanged:(id)sender { (void)sender; self.sensLabel.stringValue = [NSString stringWithFormat:@"%.0f%%", sens_from_slider(self.sens.doubleValue) / 0.13333 * 100]; }
- (void)play:(id)sender { (void)sender; if (!self.extracting && ([self gameReady] || getenv("STW_LAUNCHERSHOT"))) [NSApp stopModal]; }
- (void)quit:(id)sender { (void)sender; _exit(0); }
- (NSTextField *)label:(NSString *)t at:(NSRect)r bold:(BOOL)b in:(NSView *)v {
    NSTextField *l = [NSTextField labelWithString:t];
    l.frame = r;
    if (b) l.font = [NSFont boldSystemFontOfSize:13];
    [v addSubview:l];
    return l;
}


/* ---- Game disc: pick the disc image (or an extracted folder) before the game starts ---- */
static int extract_cb(uint64_t done, uint64_t total, const char *name, void *ctx) {      /* runs on the extraction thread */
    StwLauncher *l = (__bridge StwLauncher *)ctx;
    static uint64_t last;
    if (done - last > (4u << 20) || done == total) {                                      /* update the window about every 4 MB */
        last = done;
        NSString *n = @(name);
        dispatch_async(dispatch_get_main_queue(), ^{
            l.progress.doubleValue = total ? (double)done / (double)total * 100.0 : 0;
            l.gameStatus.stringValue = [NSString stringWithFormat:@"Copying game files: %.0f%%   %@", total ? (double)done / (double)total * 100.0 : 0.0, n];
        });
    }
    return l.cancelExtract;
}
- (BOOL)gameReady { char err[256]; return stw_game_valid(stw_game_root(), err, sizeof err) && stw_lifted_ready(); }
- (void)refreshGame {
    if (self.extracting || self.prep.busy) return;
    char verr[256];
    if (stw_game_valid(stw_game_root(), verr, sizeof verr) && !stw_lifted_ready()) {      /* the app has to translate this disc for this Mac first */
        self.gameStatus.stringValue = [NSString stringWithFormat:@"Game found: %s", stw_game_root()];
        self.gameStatus.textColor = NSColor.labelColor;
        self.playBtn.enabled = NO;
        [self.prep startWithGameRoot:stw_game_root() completion:^(BOOL ok) { if (ok) [self refreshGame]; }];
        return;
    }
    BOOL ok = [self gameReady];
    if (ok && getenv("STW_LAUNCHERAUTOPLAY")) dispatch_async(dispatch_get_main_queue(), ^{ [self play:nil]; });
    self.gameStatus.stringValue = ok ? [NSString stringWithFormat:@"Ready: %s", stw_game_root()]
                                      : @"No game found. Choose your Spartan: Total Warrior (PAL) disc image (.iso), or a folder extracted from it.";
    self.gameStatus.textColor = ok ? [NSColor colorWithCalibratedRed:0.1 green:0.5 blue:0.2 alpha:1] : [NSColor colorWithCalibratedRed:0.75 green:0.2 blue:0.1 alpha:1];
    self.playBtn.enabled = ok;
    self.chooseBtn.enabled = YES; self.chooseBtn.hidden = NO;
    self.progress.hidden = YES; self.cancelBtn.hidden = YES;
}
- (void)alert:(NSString *)msg {
    NSAlert *a = [NSAlert new];
    a.messageText = msg;
    a.alertStyle = NSAlertStyleWarning;
    [a runModal];
}
- (void)useGameRoot:(const char *)root iso:(const char *)iso {
    StwSettings c = stw_settings_get();
    snprintf(c.game_root, sizeof c.game_root, "%s", root);
    snprintf(c.iso, sizeof c.iso, "%s", iso);
    stw_settings_put(&c);
    stw_set_game_root(root);
    [self refreshGame];
}
- (void)cancelCopy:(id)sender { (void)sender; self.cancelExtract = YES; }
- (void)chooseGame:(id)sender {
    (void)sender;
    NSOpenPanel *op = [NSOpenPanel openPanel];
    {   /* start in the folder the disc image was chosen from last time (the image stays where the user keeps it) */
        StwSettings cur = stw_settings_get();
        if (cur.iso[0]) op.directoryURL = [NSURL fileURLWithPath:@(cur.iso).stringByDeletingLastPathComponent isDirectory:YES];
        else if (cur.game_root[0]) op.directoryURL = [NSURL fileURLWithPath:@(cur.game_root).stringByDeletingLastPathComponent isDirectory:YES];
    }
    op.title = @"Choose the game disc image or an extracted game folder";
    op.message = @"A Spartan: Total Warrior (Xbox, PAL) .iso, or the folder that holds its default.xbe";
    op.canChooseFiles = YES; op.canChooseDirectories = YES; op.allowsMultipleSelection = NO;
    if ([op runModal] != NSModalResponseOK || !op.URL) return;
    [self handlePath:op.URL.path.UTF8String];
}
- (void)handlePath:(const char *)path {
    struct stat st;
    char err[256];
    if (stat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) {                                    /* an extracted folder: used in place */
        if (!stw_game_valid(path, err, sizeof err)) { [self alert:@(err)]; return; }
        [self useGameRoot:path iso:""];
        return;
    }
    XIso *x = xiso_open(path);
    if (!x) { [self alert:@"That file is not an Xbox disc image (XDVDFS .iso)."]; return; }
    uint32_t n = 0;
    uint8_t *xbe = xiso_read_file(x, "default.xbe", &n);
    if (!xbe) { xiso_close(x); [self alert:@"That disc image has no default.xbe."]; return; }
    BOOL match = stw_bytes_match_build(xbe, n);
    free(xbe);
    if (!match) { xiso_close(x); [self alert:@"This disc is not the version this build was made from (Spartan: Total Warrior, PAL). To use another version, rebuild with scripts/prepare.sh and that disc."]; return; }
    char dest[1100], tmp[1100];
    stw_path(dest, sizeof dest, "game_disc");
    snprintf(tmp, sizeof tmp, "%s.partial", dest);
    if (stw_game_valid(dest, err, sizeof err)) {                  /* copied before: just point at it */
        xiso_close(x);
        [self useGameRoot:dest iso:path];
        return;
    }
    self.extracting = YES; self.cancelExtract = NO;
    self.chooseBtn.enabled = NO; self.chooseBtn.hidden = YES; self.playBtn.enabled = NO;
    self.progress.hidden = NO; self.progress.doubleValue = 0; self.cancelBtn.hidden = NO;
    self.gameStatus.textColor = NSColor.labelColor;
    self.gameStatus.stringValue = @"Copying the game files from the disc image (about 2 GB, once)...";
    NSString *isoS = @(path), *destS = @(dest), *tmpS = @(tmp);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        char e[300] = "";
        NSFileManager *fm = NSFileManager.defaultManager;
        [fm removeItemAtPath:tmpS error:nil];
        int rc = xiso_extract(x, tmpS.UTF8String, extract_cb, (__bridge void *)self, e, sizeof e);
        xiso_close(x);
        if (rc == 0) { [fm removeItemAtPath:destS error:nil]; if (rename(tmpS.UTF8String, destS.UTF8String)) { rc = -1; snprintf(e, sizeof e, "could not move the copied files into place"); } }
        else [fm removeItemAtPath:tmpS error:nil];
        NSString *eS = @(e);
        dispatch_async(dispatch_get_main_queue(), ^{
            self.extracting = NO;
            self.progress.hidden = YES; self.cancelBtn.hidden = YES; self.chooseBtn.hidden = NO;       /* the copy is over (preparation has its own panel below) */
            if (rc == 0) [self useGameRoot:destS.UTF8String iso:isoS.UTF8String];
            else { [self refreshGame]; if (!self.cancelExtract) [self alert:[NSString stringWithFormat:@"Could not copy the game files: %@", eS]]; }
        });
    });
}

/* ---- Quality tab (anti-aliasing, sharpening, bloom, texture hooks) ---- */
- (void)sharpChanged:(id)sender { (void)sender; int v = (int)llround(self.sharpSlider.doubleValue); self.sharpLabel.stringValue = v ? [NSString stringWithFormat:@"%d%%", v] : @"off"; }
- (void)note:(NSString *)t at:(NSRect)r in:(NSView *)v {                 /* small wrapping caption */
    NSTextField *l = [self label:t at:r bold:NO in:v];
    l.font = [NSFont systemFontOfSize:11];
    l.lineBreakMode = NSLineBreakByWordWrapping;
    l.maximumNumberOfLines = 3;
    l.preferredMaxLayoutWidth = r.size.width;
}
- (void)buildQuality:(NSView *)v settings:(StwSettings)c {
    CGFloat y = 450;
    [self label:@"Anti-aliasing" at:NSMakeRect(16, y, 120, 20) bold:YES in:v];
    self.aaPop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, y - 4, 340, 26) pullsDown:NO];
    [self.aaPop addItemsWithTitles:@[@"Off", @"FXAA (post filter, cheap)", @"MSAA 2x (3D scene)", @"MSAA 4x (3D scene)"]];
    [self.aaPop selectItemAtIndex:c.aa];
    [v addSubview:self.aaPop];
    [self note:@"FXAA smooths every edge of the final image; MSAA smooths the 3D geometry (costs GPU time)." at:NSMakeRect(150, y - 44, 350, 36) in:v];
    [self label:@"Sharpening" at:NSMakeRect(16, y - 80, 120, 20) bold:YES in:v];
    self.sharpSlider = [NSSlider sliderWithValue:c.sharpen minValue:0 maxValue:100 target:self action:@selector(sharpChanged:)];
    self.sharpSlider.frame = NSMakeRect(150, y - 82, 280, 24);
    [v addSubview:self.sharpSlider];
    self.sharpLabel = [self label:@"" at:NSMakeRect(440, y - 80, 60, 20) bold:NO in:v];
    [self sharpChanged:nil];
    [self label:@"Contrast-adaptive sharpening of the final image (off = 0)." at:NSMakeRect(150, y - 104, 350, 16) bold:NO in:v].font = [NSFont systemFontOfSize:11];
    self.bloomChk = [NSButton checkboxWithTitle:@"Smooth bloom at high internal resolution" target:nil action:nil];
    self.bloomChk.frame = NSMakeRect(16, y - 146, 480, 22); self.bloomChk.state = c.bloom_hq ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:self.bloomChk];
    [self note:@"Anti-aliasing and bloom options apply at the next start of the game." at:NSMakeRect(16, y - 200, 484, 36) in:v];
}
- (void)readQuality:(StwSettings *)c {
    c->aa = (int)self.aaPop.indexOfSelectedItem;
    c->sharpen = (int)llround(self.sharpSlider.doubleValue);
    c->bloom_hq = self.bloomChk.state == NSControlStateValueOn;
}

/* ---- Controls tab ---- */
static NSString *describe(StwBinding b) {
    if (!b.kind) return @"(not set)";
    NSMutableString *s = [NSMutableString new];
    if (b.mods & MOD_CTRL) [s appendString:@"Ctrl + "];
    if (b.mods & MOD_OPT) [s appendString:@"Option + "];
    if (b.mods & MOD_SHIFT) [s appendString:@"Shift + "];
    if (b.mods & MOD_CMD) [s appendString:@"Cmd + "];
    if (b.kind == 1) [s appendString:@(stw_key_name(b.code))];
    else [s appendString:b.code == 0 ? @"Left click" : b.code == 1 ? @"Right click" : b.code == 2 ? @"Middle click" : [NSString stringWithFormat:@"Mouse button %d", b.code + 1]];
    return s;
}
- (void)refreshRow:(int)act {
    StwBinding b = stw_binding_get(act);
    NSPopUpButton *pb = self.rowButtons[act];
    [pb.itemArray[0] setTitle:describe(b)];
    for (NSMenuItem *mi in pb.menu.itemArray) {
        int op = (int)mi.tag % 100;
        if (op >= 10 && op <= 13) mi.state = (b.mods & (1 << (op - 10))) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [pb selectItemAtIndex:0];
}
- (void)stopRecording {
    if (self.keyMonitor) { [NSEvent removeMonitor:self.keyMonitor]; self.keyMonitor = nil; }
    if (self.recording >= 0) { int a = self.recording; self.recording = -1; [self refreshRow:a]; }
}
- (void)rowMenu:(NSMenuItem *)mi {
    int act = (int)mi.tag / 100, op = (int)mi.tag % 100;
    [self stopRecording];
    StwBinding b = stw_binding_get(act);
    if (op == 1) {                                         /* press a key (or a modifier key) to bind it; Esc cancels */
        self.recording = act;
        [self.rowButtons[act].itemArray[0] setTitle:@"press a key..."];
        __weak StwLauncher *me = self;
        self.keyMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *e) {
            StwLauncher *s = me;
            if (!s || s.recording != act) return e;
            StwBinding nb = stw_binding_get(act);
            if (e.type == NSEventTypeKeyDown) {
                if (e.keyCode == 53) { [s stopRecording]; return nil; }       /* Esc: cancel */
                NSEventModifierFlags f = e.modifierFlags;
                nb.kind = 1; nb.code = e.keyCode;
                nb.mods = ((f & NSEventModifierFlagShift) ? MOD_SHIFT : 0) | ((f & NSEventModifierFlagCommand) ? MOD_CMD : 0) |
                          ((f & NSEventModifierFlagControl) ? MOD_CTRL : 0) | ((f & NSEventModifierFlagOption) ? MOD_OPT : 0);
            } else {                                                          /* a modifier key pressed on its own */
                int c = e.keyCode;
                int down = (c == 56 || c == 60) ? (e.modifierFlags & NSEventModifierFlagShift) : (c == 55 || c == 54) ? (e.modifierFlags & NSEventModifierFlagCommand) :
                           (c == 59 || c == 62) ? (e.modifierFlags & NSEventModifierFlagControl) : (c == 58 || c == 61) ? (e.modifierFlags & NSEventModifierFlagOption) : 0;
                if (!down) return nil;
                nb.kind = 1; nb.code = c; nb.mods = 0;
            }
            stw_binding_put(act, nb);
            [s stopRecording];
            return nil;
        }];
        return;
    }
    if (op >= 2 && op <= 6) { b.kind = 2; b.code = op == 2 ? 0 : op == 3 ? 1 : op == 4 ? 2 : op == 5 ? 3 : 4; }
    else if (op >= 10 && op <= 13) b.mods ^= 1 << (op - 10);
    else if (op == 20) b = (StwBinding){ 0, 0, 0 };
    else if (op == 21) b = stw_binding_default(act);
    stw_binding_put(act, b);
    [self refreshRow:act];
}
- (void)resetAll:(id)sender {
    (void)sender;
    [self stopRecording];
    stw_bindings_reset();
    for (int i = 0; i < STW_NACTIONS; i++) [self refreshRow:i];
}
- (void)buildControls:(NSView *)v {
    self.recording = -1;
    self.rowButtons = [NSMutableArray new];
    CGFloat rowH = 30, h = STW_NACTIONS * rowH + 8;
    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(8, 40, v.bounds.size.width - 16, v.bounds.size.height - 48)];
    sv.hasVerticalScroller = YES; sv.borderType = NSBezelBorder;
    NSView *doc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, h)];
    for (int i = 0; i < STW_NACTIONS; i++) {
        CGFloat y = h - 4 - (i + 1) * rowH;
        [self label:@(stw_action_name(i)) at:NSMakeRect(10, y + 6, 230, 18) bold:NO in:doc];
        NSPopUpButton *pb = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(250, y + 2, sv.contentSize.width - 262, 26) pullsDown:YES];
        [pb addItemWithTitle:@""];
        struct { NSString *t; int op; } items[] = { { @"Press a key...", 1 }, { @"Left click", 2 }, { @"Right click", 3 }, { @"Middle click", 4 }, { @"Mouse button 4", 5 }, { @"Mouse button 5", 6 },
            { nil, 0 }, { @"Require Shift", 10 }, { @"Require Cmd", 11 }, { @"Require Ctrl", 12 }, { @"Require Option", 13 }, { nil, 0 }, { @"Clear", 20 }, { @"Default", 21 } };
        for (unsigned k = 0; k < sizeof items / sizeof items[0]; k++) {
            if (!items[k].t) { [pb.menu addItem:[NSMenuItem separatorItem]]; continue; }
            NSMenuItem *mi = [pb.menu addItemWithTitle:items[k].t action:@selector(rowMenu:) keyEquivalent:@""];
            mi.target = self; mi.tag = i * 100 + items[k].op;
        }
        [doc addSubview:pb];
        [self.rowButtons addObject:pb];
        [self refreshRow:i];
    }
    sv.documentView = doc;
    [doc scrollPoint:NSMakePoint(0, h)];
    [v addSubview:sv];
    NSButton *reset = [NSButton buttonWithTitle:@"Reset all to defaults" target:self action:@selector(resetAll:)];
    reset.frame = NSMakeRect(8, 6, 170, 28);
    [v addSubview:reset];
    [self label:@"Mouse movement turns the camera and the wheel zooms (not rebindable). Combos: pick Require Shift/Cmd/..." at:NSMakeRect(186, 10, v.bounds.size.width - 194, 28) bold:NO in:v].font = [NSFont systemFontOfSize:10];
}

- (void)run {
    StwSettings c = stw_settings_get();
    if (getenv("STW_RES")) { int v = atoi(getenv("STW_RES")); if (v >= 0 && v <= 4) c.scale = v; }
    self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 620) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                              backing:NSBackingStoreBuffered defer:NO];
    self.panel.title = @"Spartan: Total Warrior - launcher";
    self.panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    NSView *root = self.panel.contentView;
    NSTabView *tabs = [[NSTabView alloc] initWithFrame:NSMakeRect(12, 56, 536, 552)];
    NSTabViewItem *t1 = [[NSTabViewItem alloc] initWithIdentifier:@"display"]; t1.label = @"Display and mouse";
    NSTabViewItem *t2 = [[NSTabViewItem alloc] initWithIdentifier:@"controls"]; t2.label = @"Controls";
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 510)];
    NSView *v2 = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 510)];
    t1.view = v; t2.view = v2;
    [tabs addTabViewItem:t1]; [tabs addTabViewItem:t2];
    StwPadTab *padTab = [[StwPadTab alloc] initWithSize:NSMakeSize(520, 510)];     /* controller options and remapping (launcher_pad.m) */
    NSTabViewItem *t3 = [[NSTabViewItem alloc] initWithIdentifier:@"controller"]; t3.label = @"Controller";
    t3.view = padTab.view;
    [tabs addTabViewItem:t3];
    NSView *v4 = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 510)];       /* Quality: anti-aliasing, sharpening, bloom, texture hooks */
    NSTabViewItem *t4 = [[NSTabViewItem alloc] initWithIdentifier:@"quality"]; t4.label = @"Quality";
    t4.view = v4;
    [tabs addTabViewItem:t4];
    [self buildQuality:v4 settings:c];
    [root addSubview:tabs];
    CGFloat top = 372;
    [self label:@"Game" at:NSMakeRect(16, 480, 120, 20) bold:YES in:v];
    self.gameStatus = [self label:@"" at:NSMakeRect(150, 446, 350, 52) bold:NO in:v];
    self.gameStatus.lineBreakMode = NSLineBreakByWordWrapping;
    self.gameStatus.maximumNumberOfLines = 3;
    self.gameStatus.preferredMaxLayoutWidth = 350;
    self.chooseBtn = [NSButton buttonWithTitle:@"Choose disc image or folder..." target:self action:@selector(chooseGame:)];
    self.chooseBtn.frame = NSMakeRect(146, 408, 230, 28);
    [v addSubview:self.chooseBtn];
    self.progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(150, 418, 280, 12)];
    self.progress.style = NSProgressIndicatorStyleBar; self.progress.indeterminate = NO; self.progress.minValue = 0; self.progress.maxValue = 100; self.progress.hidden = YES;
    [v addSubview:self.progress];
    self.cancelBtn = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancelCopy:)];
    self.cancelBtn.frame = NSMakeRect(436, 410, 70, 26); self.cancelBtn.hidden = YES;
    [v addSubview:self.cancelBtn];
    [self label:@"Resolution" at:NSMakeRect(16, top, 120, 20) bold:YES in:v];
    self.res = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, top - 4, 340, 26) pullsDown:NO];
    for (int s = 0; s <= 4; s++) [self.res addItemWithTitle:res_text(s, c.widescreen)];
    [self.res selectItemAtIndex:c.scale];
    if (getenv("STW_LAUNCHERSHOT")) fprintf(stderr, "[launcher] scale=%d selected=%d\n", c.scale, (int)self.res.indexOfSelectedItem);
    [v addSubview:self.res];
    [self label:@"in game: F1 / F2 = lower / higher" at:NSMakeRect(150, top - 24, 340, 16) bold:NO in:v].font = [NSFont systemFontOfSize:11];
    [self label:@"Aspect ratio" at:NSMakeRect(16, top - 50, 120, 20) bold:YES in:v];          /* widescreen: a self-contained section */
    self.aspect = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, top - 54, 340, 26) pullsDown:NO];
    [self.aspect addItemsWithTitles:@[@"4:3   original picture", @"16:9   widescreen (wider view, HUD kept proportional)"]];
    [self.aspect selectItemAtIndex:c.widescreen ? 1 : 0];
    self.aspect.target = self; self.aspect.action = @selector(aspectChanged:);          /* the resolution list shows sizes for the chosen aspect */
    [v addSubview:self.aspect];
    [self label:@"Mouse look" at:NSMakeRect(16, top - 88, 120, 20) bold:YES in:v];
    self.sens = [NSSlider sliderWithValue:slider_from_sens(c.sens) minValue:0 maxValue:1 target:self action:@selector(sliderChanged:)];
    self.sens.frame = NSMakeRect(150, top - 90, 280, 24);
    [v addSubview:self.sens];
    self.sensLabel = [self label:@"" at:NSMakeRect(440, top - 88, 56, 20) bold:NO in:v];
    [self sliderChanged:nil];
    [self label:@"camera sensitivity (slow ... fast)" at:NSMakeRect(150, top - 108, 340, 16) bold:NO in:v].font = [NSFont systemFontOfSize:11];
    self.hud = [NSButton checkboxWithTitle:@"Metal performance HUD (F3 shows / hides it in game)" target:nil action:nil];
    self.hud.frame = NSMakeRect(16, top - 140, 480, 22); self.hud.state = c.hud ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:self.hud];
    self.full = [NSButton checkboxWithTitle:@"Full screen (F10 in game)" target:nil action:nil];
    self.full.frame = NSMakeRect(16, top - 166, 480, 22); self.full.state = c.fullscreen ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:self.full];
    self.show = [NSButton checkboxWithTitle:@"Show this window at startup (hold Option to bring it back)" target:nil action:nil];
    self.show.frame = NSMakeRect(16, top - 192, 480, 22); self.show.state = c.show_launcher ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:self.show];
    self.prep = [[StwPrepareView alloc] initWithParent:v frame:NSMakeRect(16, 14, 488, 150)];
    [self buildControls:v2];
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)];
    quit.frame = NSMakeRect(24, 14, 90, 32);
    [root addSubview:quit];
    NSButton *play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)];
    play.frame = NSMakeRect(446, 14, 90, 32);
    play.keyEquivalent = @"\r";
    [root addSubview:play];
    self.playBtn = play;
    [self refreshGame];
    [self.panel center];
    [NSApp activateIgnoringOtherApps:YES];
    [self.panel makeKeyAndOrderFront:nil];
    if (getenv("STW_LAUNCHERSHOT"))                       /* debug: save pictures of both tabs ("<path>.png" and "<path>_controls.png"), then press Play */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (getenv("STW_LAUNCHERPICK") ? 12 : 1) * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            for (int pass = 0; pass < 4; pass++) {
                [tabs selectTabViewItemAtIndex:pass];
                [self.panel displayIfNeeded];
                NSView *cv = self.panel.contentView;
                NSBitmapImageRep *rep = [cv bitmapImageRepForCachingDisplayInRect:cv.bounds];
                [cv cacheDisplayInRect:cv.bounds toBitmapImageRep:rep];
                NSString *path = @(getenv("STW_LAUNCHERSHOT"));
                if (pass) path = [[path stringByDeletingPathExtension] stringByAppendingString:pass == 1 ? @"_controls.png" : pass == 2 ? @"_controller.png" : @"_quality.png"];
                [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
            }
            [self play:nil];
        });
    if (getenv("STW_LAUNCHERPICK"))                        /* debug: feed a path to the same code the file picker uses */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ [self handlePath:getenv("STW_LAUNCHERPICK")]; });
    [NSApp runModalForWindow:self.panel];                 /* returns when Play stops the modal session (closing the panel also starts the game) */
    [self stopRecording];
    [padTab stop];
    if (getenv("STW_LAUNCHERSHOT")) fprintf(stderr, "[launcher] at close selected=%d\n", (int)self.res.indexOfSelectedItem);
    [self.panel orderOut:nil];
    c = stw_settings_get();                               /* bindings were saved as they were edited */
    c.scale = (int)self.res.indexOfSelectedItem;
    c.widescreen = (int)self.aspect.indexOfSelectedItem == 1;
    c.sens = sens_from_slider(self.sens.doubleValue);
    c.hud = self.hud.state == NSControlStateValueOn;
    c.fullscreen = self.full.state == NSControlStateValueOn;
    c.show_launcher = self.show.state == NSControlStateValueOn;
    [self readQuality:&c];
    stw_settings_put(&c);
}
@end
static int launcher_wanted(void) {
    char err[256];
    if (getenv("STW_HEADLESS")) return 0;
    if ((!stw_game_valid(stw_game_root(), err, sizeof err) || !stw_lifted_ready()) && !getenv("STW_NOLAUNCHER")) return 1;     /* nothing to play yet: the launcher is where the disc gets chosen */
    if (getenv("STW_NOLAUNCHER")) return 0;
    if ([NSEvent modifierFlags] & NSEventModifierFlagOption) return 1;
    return stw_settings_get().show_launcher;
}

@interface StwDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation StwDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    stw_pad_start();                                 /* pick up controllers (and their hot-plug notifications) from the start */
    if (launcher_wanted()) { StwLauncher *l = [StwLauncher new]; [l run]; }
    StwSettings cfg = stw_settings_get();
    if (!getenv("MTL_HUD_ENABLED")) {                    /* the Metal HUD is only loaded when the variable is set at launch: start over with it (hidden again below when it is off), without the launcher */
        setenv("MTL_HUD_ENABLED", "1", 1);
        setenv("STW_NOLAUNCHER", "1", 1);
        NSArray<NSString *> *args = NSProcessInfo.processInfo.arguments;
        char *argv[64]; int n = 0;
        for (NSString *a in args) if (n < 63) argv[n++] = (char *)a.UTF8String;
        argv[n] = NULL;
        char path[4096]; uint32_t sz = sizeof path;
        if (!_NSGetExecutablePath(path, &sz)) { fflush(NULL); execv(path, argv); }
        fprintf(stderr, "stw: could not restart with the Metal HUD enabled\n");
    }
    hud_on = getenv("MTL_HUD_ENABLED") != NULL;
    if (hud_on && !cfg.hud)                              /* loaded for F3, but the saved choice is "off": hide it once its menu item exists */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSMenuItem *it = find_hud_item(NSApp.mainMenu);
            if (it) { [NSApp sendAction:it.action to:it.target from:it]; hud_on = 0; }
        });
    if ((!getenv("STW_RES") && !getenv("STW_SCALE")) || launcher_wanted()) { char sc[8]; snprintf(sc, sizeof sc, "%d", cfg.scale); setenv("STW_RES", sc, 1); unsetenv("STW_SCALE"); }   /* the renderer reads it when the guest starts */
    int wide = stw_settings_get().widescreen || (getenv("STW_WIDE") && atoi(getenv("STW_WIDE")));
    NSRect r = NSMakeRect(0, 0, wide ? 1280 : 960, 720);
    window = [[StwWindow alloc] initWithContentRect:r
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                           backing:NSBackingStoreBuffered defer:NO];
    window.title = @"Spartan: Total Warrior (native recomp)";
    window.delegate = self;
    window.acceptsMouseMovedEvents = YES;
    window.contentAspectRatio = wide ? NSMakeSize(16, 9) : NSMakeSize(4, 3);
    if (getenv("STW_WINSIZE")) {                      /* debug: "WxH" window with no aspect lock, to check the letterbox / pillarbox bars */
        int ww = 1000, wh = 720; sscanf(getenv("STW_WINSIZE"), "%dx%d", &ww, &wh);
        window.contentResizeIncrements = NSMakeSize(1, 1);
        [window setContentSize:NSMakeSize(ww, wh)];
    }
    layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = getenv("STW_PRESENTDUMP") == NULL;   /* STW_PRESENTDUMP=<n>: read back the n-th presented frame to /tmp/stw_present.ppm (debugging) */
    layer.maximumDrawableCount = 2;                  /* default 3 lets the presenter queue two frames ahead of the display: ~33 ms more input latency */
    window.contentView.wantsLayer = YES;
    window.contentView.layer = layer;
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
    NSMenu *bar = [NSMenu new];                      /* Cmd+Q quits (the cursor is hidden while the mouse is captured, so there is no close button to click) */
    NSMenuItem *appItem = [NSMenuItem new];
    [bar addItem:appItem];
    NSMenu *appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit Spartan: Total Warrior" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    NSApp.mainMenu = bar;
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (cfg.fullscreen) [window toggleFullScreen:nil];
    guest_start();                                   /* the window exists: start the guest */
    if (getenv("STW_HUDTEST"))                       /* debug: press the HUD toggle at that second */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(getenv("STW_HUDTEST")) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ act_hud(); });
    if (getenv("STW_QUITAFTER"))                     /* debug: exercise the Cmd+Q path */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(getenv("STW_QUITAFTER")) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
/* Leave immediately: the guest threads and the audio mixer are still running and must not be waited for. */
static void quit_now(void) { input_set_mouselook(0); fflush(NULL); _exit(0); }
- (void)applicationWillTerminate:(NSNotification *)n { (void)n; quit_now(); }
- (void)windowWillClose:(NSNotification *)n { (void)n; quit_now(); }
@end

void display_main_loop(void (*start_guest)(void)) {
    @autoreleasepool {
        guest_start = start_guest;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        setup_metal();
        StwDelegate *del = [StwDelegate new];
        NSApp.delegate = del;
        [NSApp run];
    }
}

/* Called from the vblank thread: copy the scanout surface into a texture and draw it. */
void display_present(uint32_t fb, uint32_t pitch, uint32_t width, uint32_t height, uint32_t format) {
    (void)format;                                    /* 32-bit X8R8G8B8 / A8R8G8B8 only for now */
    display_present_ptr(G + fb, pitch, width, height);
}
static double present_aspect = 4.0 / 3.0;
void display_set_aspect(double a) { present_aspect = a; }
/* One full-screen pass src -> dst with a filter pipeline. */
static void filter_pass(id<MTLCommandBuffer> cb, id<MTLRenderPipelineState> ps, id<MTLTexture> src, id<MTLTexture> dst, float strength, int has_param) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps];
    [e setFragmentTexture:src atIndex:0];
    if (has_param) [e setFragmentBytes:&strength length:sizeof strength atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}
static id<MTLTexture> scratch(int slot, NSUInteger w, NSUInteger h) {
    if (!filt_tex[slot] || filt_tex[slot].width != w || filt_tex[slot].height != h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        filt_tex[slot] = [device newTextureWithDescriptor:td];
    }
    return filt_tex[slot];
}
/* Debug: write a texture (BGRA8) as a PPM. */
static void dump_texture(id<MTLTexture> t, const char *name) {
    id<MTLBuffer> bb = [device newBufferWithLength:t.width * t.height * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> dcb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> be = [dcb blitCommandEncoder];
    [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:bb destinationOffset:0 destinationBytesPerRow:t.width * 4 destinationBytesPerImage:t.width * t.height * 4];
    [be endEncoding]; [dcb commit]; [dcb waitUntilCompleted];
    const uint8_t *px = bb.contents;
    FILE *f = fopen(name, "wb");
    if (f) { fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)t.width, (unsigned long)t.height); for (size_t i = 0; i < t.width * t.height; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); } fclose(f); }
}
static int present_count;                                   /* frames presented from a scaled texture (the counter STW_FILTERDUMP and STW_AASCRIPT use) */
static void draw_texture(id<MTLTexture> t, int filters) {
    load_quality();
    if (filters) {
        ++present_count;
        if (getenv("STW_BLOOMSCRIPT")) {                      /* debugging aid: "5398:0;5402:1" turns the smooth bloom upsampling off / on before presenting those frames */
            for (const char *q = getenv("STW_BLOOMSCRIPT"); *q;) {
                char *e; long at = strtol(q, &e, 10), v = 0;
                if (*e == ':') v = strtol(e + 1, &e, 10);
                if (*e == ';') e++;
                if (e == q) break;
                q = e;
                if (at == present_count) gpu_set_bloomhq((int)v);
            }
        }
        if (getenv("STW_AASCRIPT")) {                         /* debugging aid: "5399:2;5404:0" switches the quality mode before presenting those frames (0 off, 1 FXAA, 2 MSAA 2x, 3 MSAA 4x; +10 = also sharpen 50) */
            for (const char *q = getenv("STW_AASCRIPT"); *q;) {
                char *e; long at = strtol(q, &e, 10), mode = 0;
                if (*e == ':') mode = strtol(e + 1, &e, 10);
                if (*e == ';') e++;
                if (e == q) break;
                q = e;
                if (at == present_count) { int sh = mode >= 10; mode %= 10; q_fxaa = mode == 1; q_sharpen = sh ? 50 : 0; gpu_set_msaa(mode == 2 ? 2 : mode == 3 ? 4 : 1); }
            }
        }
    }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) { static int missed; if (getenv("STW_PRESENTLOG") && (++missed % 30) == 1) fprintf(stderr, "[present] no drawable (%d missed)\n", missed); return; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (filters && (q_fxaa || q_sharpen)) {                       /* FXAA, then sharpen, both at the internal resolution */
        if (q_fxaa) { id<MTLTexture> o = scratch(0, t.width, t.height); filter_pass(cb, pso_fxaa, t, o, 0, 0); t = o; }
        if (q_sharpen) { id<MTLTexture> o = scratch(1, t.width, t.height); filter_pass(cb, pso_cas, t, o, q_sharpen / 100.f, 1); t = o; }
    }
    if (filters && getenv("STW_FILTERDUMP_ALL") && present_count == atoi(getenv("STW_FILTERDUMP_ALL"))) {   /* debugging aid: the SAME frame through every post filter: <path>_0 off, _1 FXAA, _2 sharpen 50, _3 both */
        for (int k = 0; k < 4; k++) {
            id<MTLCommandBuffer> fcb = [queue commandBuffer];
            id<MTLTexture> o = t;
            if (k & 1) { id<MTLTexture> a = scratch(0, t.width, t.height); filter_pass(fcb, pso_fxaa, o, a, 0, 0); o = a; }
            if (k & 2) { id<MTLTexture> b = scratch(1, t.width, t.height); filter_pass(fcb, pso_cas, o, b, 0.5f, 1); o = b; }
            [fcb commit]; [fcb waitUntilCompleted];
            char name[600];
            snprintf(name, sizeof name, "%s_%d.ppm", getenv("STW_FILTERDUMP_PATH") ? getenv("STW_FILTERDUMP_PATH") : "/tmp/stw_all", k);
            dump_texture(o, name);
        }
    }
    if (filters && getenv("STW_FILTERDUMP")) {                   /* debugging aid: STW_FILTERDUMP=<present numbers, comma separated>, STW_FILTERDUMP_PATH=/tmp/x_%d.ppm: the image at the internal resolution after the filters */
        int fn = present_count, want = 0;
        for (const char *q = getenv("STW_FILTERDUMP"); *q;) { char *e; long v = strtol(q, &e, 10); if (e == q) break; if (v == fn) want = 1; q = *e == ',' ? e + 1 : e; }
        if (want) {
            id<MTLBuffer> bb = [device newBufferWithLength:t.width * t.height * 4 options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:bb destinationOffset:0 destinationBytesPerRow:t.width * 4 destinationBytesPerImage:t.width * t.height * 4];
            [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
            const uint8_t *px = bb.contents;
            char name[600];
            snprintf(name, sizeof name, getenv("STW_FILTERDUMP_PATH") ? getenv("STW_FILTERDUMP_PATH") : "/tmp/stw_filtered_%d.ppm", fn);
            FILE *f = fopen(name, "wb");
            if (f) { fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)t.width, (unsigned long)t.height); for (size_t i = 0; i < t.width * t.height; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); } fclose(f); }
            cb = [queue commandBuffer];
        }
    }
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    {   /* keep the picture's shape (4:3, or 16:9 in widescreen mode): bars around it in any other window shape and in full screen */
        double dw = drawable.texture.width, dh = drawable.texture.height, a = present_aspect, vw = dw, vh = dh;
        if (dw / dh > a) vw = dh * a; else vh = dw / a;
        [enc setViewport:(MTLViewport){ floor((dw - vw) / 2), floor((dh - vh) / 2), vw, vh, 0, 1 }];
    }
    [enc setRenderPipelineState:pso];
    [enc setFragmentTexture:t atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb presentDrawable:drawable];
    [cb commit];
    static int present_n;
    if (getenv("STW_PRESENTDUMP") && ++present_n == atoi(getenv("STW_PRESENTDUMP"))) {
        [cb waitUntilCompleted];
        id<MTLTexture> dt = drawable.texture;
        NSUInteger w = dt.width, h = dt.height;
        uint8_t *px = malloc(w * h * 4);
        [dt getBytes:px bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        FILE *f = fopen(getenv("STW_PRESENTDUMP_PATH") ? getenv("STW_PRESENTDUMP_PATH") : "/tmp/stw_present.ppm", "wb");
        if (f) { fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h); for (size_t i = 0; i < w * h; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); } fclose(f); }
        free(px);
    }
}
void display_present_tex(void *mtl_texture, uint32_t width, uint32_t height) {
    (void)width; (void)height;
    if (!layer || !mtl_texture) return;
    @autoreleasepool { draw_texture((__bridge id<MTLTexture>)mtl_texture, 1); }
}
void display_present_ptr(const void *px, uint32_t pitch, uint32_t width, uint32_t height) {
    if (!layer || !width || !height) return;
    @autoreleasepool {
        if (!tex || tex_w != width || tex_h != height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:width height:height mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            tex = [device newTextureWithDescriptor:td];
            tex_w = width; tex_h = height;
        }
        [tex replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:px bytesPerRow:pitch];
        draw_texture(tex, 0);
    }
}
