/* Game controllers. See pad.h. Real pads come from Apple's GameController framework (Xbox, DualShock 4, DualSense, Switch Pro, MFi); the
 * extended-gamepad profile is positional, so one table covers them all. Ports are filled in connection order; the keyboard and mouse always
 * live on port 0 and are merged with the first controller (input.m). */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "input.h"

#define MAXPADS 4

void stw_pad_defaults(StwPadCfg *c) {
    static const int def[PO_COUNT] = { PS_A, PS_B, PS_X, PS_Y, PS_LB, PS_RB, PS_LT, PS_RT, PS_MENU, PS_OPTIONS, PS_LS, PS_RS, PS_DUP, PS_DDOWN, PS_DLEFT, PS_DRIGHT };
    memcpy(c->map, def, sizeof def);
    c->dz_left = 0.05f; c->dz_right = 0.05f;         /* the OS already removes a little; the game adds its own ~0.27 on top */
    c->sens_right = 1.0f;
    c->trig_thresh = 0.35f;
    c->invert_ly = c->invert_ry = 0;
    c->swap_face = 0;
    c->rumble = 1;
    c->rumble_strength = 0.8f;
}
const char *stw_pad_src_name(int s) {
    static const char *n[PS_COUNT] = { "A (bottom button)", "B (right button)", "X (left button)", "Y (top button)", "LB (left bumper)", "RB (right bumper)",
        "LT (left trigger)", "RT (right trigger)", "Menu (Start / Options)", "View (Back / Share)", "Home (guide)", "Left stick click", "Right stick click",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad click" };
    return s >= 0 && s < PS_COUNT ? n[s] : "(none)";
}
const char *stw_pad_out_name(int o) {
    static const char *n[PO_COUNT] = { "A", "B", "X", "Y", "Black", "White", "Left trigger", "Right trigger", "Start", "Back", "Left stick click",
        "Right stick click", "D-pad up", "D-pad down", "D-pad left", "D-pad right" };
    return o >= 0 && o < PO_COUNT ? n[o] : "?";
}

/* ---- raw -> Xbox ---- */
static void stick(float x, float y, float dz, float scale, int invert_y, int16_t *ox, int16_t *oy) {
    float m = hypotf(x, y);
    if (m <= dz || m < 1e-6f) { *ox = *oy = 0; return; }
    float k = (m - dz) / (1.0f - dz) * scale;          /* dead zone removed and the rest stretched back to full range */
    if (k > 1) k = 1;
    float nx = x / m * k, ny = y / m * k * (invert_y ? -1 : 1);
    *ox = (int16_t)lrintf(nx * 32767.f);
    *oy = (int16_t)lrintf(ny * 32767.f);
}
void stw_pad_map(const PadRaw *raw, const StwPadCfg *c, PadState *o) {
    float src[PS_COUNT];
    memcpy(src, raw->src, sizeof src);
    if (c->swap_face) {                                  /* Nintendo-style labels: swap A<->B, X<->Y */
        float t = src[PS_A]; src[PS_A] = src[PS_B]; src[PS_B] = t;
        t = src[PS_X]; src[PS_X] = src[PS_Y]; src[PS_Y] = t;
    }
    memset(o, 0, sizeof *o);
    for (int out = 0; out < PO_COUNT; out++) {
        int s = c->map[out];
        float v = s >= 0 && s < PS_COUNT ? src[s] : 0;
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        int is_trigger_src = s == PS_LT || s == PS_RT;
        int on = v >= (is_trigger_src ? c->trig_thresh : 0.5f);
        if (out <= PO_RT) o->analog[out] = (uint8_t)lrintf(v * 255.f);               /* A B X Y Black White LT RT are analog on the Xbox */
        else switch (out) {
            case PO_START: if (on) o->buttons |= 0x10; break;
            case PO_BACK: if (on) o->buttons |= 0x20; break;
            case PO_LTHUMB: if (on) o->buttons |= 0x40; break;
            case PO_RTHUMB: if (on) o->buttons |= 0x80; break;
            case PO_DUP: if (on) o->buttons |= 0x1; break;
            case PO_DDOWN: if (on) o->buttons |= 0x2; break;
            case PO_DLEFT: if (on) o->buttons |= 0x4; break;
            case PO_DRIGHT: if (on) o->buttons |= 0x8; break;
        }
    }
    for (int i = 0; i <= PO_RT; i++) if (o->analog[i] < 10) o->analog[i] = 0;        /* keep resting noise out of the analog buttons */
    stick(raw->lx, raw->ly, c->dz_left, 1.0f, c->invert_ly, &o->lx, &o->ly);
    stick(raw->rx, raw->ry, c->dz_right, c->sens_right, c->invert_ry, &o->rx, &o->ry);
}

/* ---- connected pads ---- */
@interface StwPadSlot : NSObject {
@public
    GCController *gc;
    BOOL fake;
    PadRaw fraw;
    NSString *name;
    CHHapticEngine *eng[2];
    id<CHHapticAdvancedPatternPlayer> pl[2];
    float lvl[2];
    unsigned gen;
}
@end
@implementation StwPadSlot
@end

static NSMutableArray<StwPadSlot *> *slotList;
static dispatch_queue_t rumbleQ;
static int started, headless;
static struct timespec t_start;

static NSString *gc_name(GCController *g) {
    NSString *n = g.vendorName ?: g.productCategory ?: @"Game controller";
    return n;
}
static void add_real(GCController *g) {
    @synchronized (slotList) {
        for (StwPadSlot *s in slotList) if (s->gc == g) return;
        if (slotList.count >= MAXPADS) return;
        StwPadSlot *s = [StwPadSlot new];
        s->gc = g; s->name = gc_name(g);
        [slotList addObject:s];
        g.playerIndex = (GCControllerPlayerIndex)(slotList.count - 1);        /* lights the pad's player LED where it has one */
        if (getenv("STW_PADLOG")) fprintf(stderr, "[pad] connected: %s on port %lu\n", s->name.UTF8String, (unsigned long)slotList.count - 1);
    }
}
static void add_fake(void) {
    @synchronized (slotList) {
        if (slotList.count >= MAXPADS) return;
        StwPadSlot *s = [StwPadSlot new];
        s->fake = YES; s->name = @"Fake pad";
        [slotList addObject:s];
        if (getenv("STW_PADLOG")) fprintf(stderr, "[pad] fake pad connected on port %lu\n", (unsigned long)slotList.count - 1);
    }
}
static void remove_slot(StwPadSlot *s) {
    for (int e = 0; e < 2; e++) { [s->pl[e] stopAtTime:0 error:nil]; [s->eng[e] stopWithCompletionHandler:nil]; }
    [slotList removeObject:s];
    if (getenv("STW_PADLOG")) fprintf(stderr, "[pad] disconnected: %s (%lu left)\n", s->name.UTF8String, (unsigned long)slotList.count);
}

/* STW_FAKEPAD_SCRIPT="2:plug;5:a=1;6:a=0;8:p1.lx=0.5;9:unplug": a time line for fake pads (seconds after start). Names: a b x y lb rb lt rt menu options home ls rs
 * dup ddown dleft dright paddle1-4 touchpad (0..1) and lx ly rx ry (-1..1); "pN." selects the pad (default 0); a state stays until changed. */
typedef struct { double t; char cmd[40]; } FakeEv;
static FakeEv fev[256];
static int nfev, fnext;
static int src_by_name(const char *n) {
    static const char *names[PS_COUNT] = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "menu", "options", "home", "ls", "rs", "dup", "ddown", "dleft", "dright",
        "paddle1", "paddle2", "paddle3", "paddle4", "touchpad" };
    for (int i = 0; i < PS_COUNT; i++) if (!strcmp(n, names[i])) return i;
    return -1;
}
static void fake_apply(const char *cmd) {
    if (!strcmp(cmd, "plug")) { add_fake(); return; }
    if (!strcmp(cmd, "unplug")) {
        for (NSInteger i = (NSInteger)slotList.count - 1; i >= 0; i--) if (slotList[i]->fake) { remove_slot(slotList[i]); return; }
        return;
    }
    char b[40]; snprintf(b, sizeof b, "%s", cmd);
    char *eq = strchr(b, '=');
    if (!eq) return;
    *eq = 0;
    float v = (float)atof(eq + 1);
    int port = 0;
    char *name = b;
    if (b[0] == 'p' && b[1] >= '0' && b[1] <= '9' && b[2] == '.') { port = b[1] - '0'; name = b + 3; }
    if (port >= (int)slotList.count || !slotList[port]->fake) return;
    PadRaw *r = &slotList[port]->fraw;
    if (!strcmp(name, "lx")) r->lx = v; else if (!strcmp(name, "ly")) r->ly = v;
    else if (!strcmp(name, "rx")) r->rx = v; else if (!strcmp(name, "ry")) r->ry = v;
    else { int s = src_by_name(name); if (s >= 0) r->src[s] = v; }
}
static void fake_advance(void) {                          /* caller holds @synchronized(slotList) */
    if (fnext >= nfev) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double now = (t.tv_sec - t_start.tv_sec) + (t.tv_nsec - t_start.tv_nsec) / 1e9;
    while (fnext < nfev && fev[fnext].t <= now) fake_apply(fev[fnext++].cmd);
}
static void parse_fake_script(const char *s) {
    for (const char *q = s; *q && nfev < 256;) {
        double at = strtod(q, (char **)&q);
        if (*q == ':') q++;
        while (*q && *q != ';') {                          /* "t:cmd,cmd" */
            char cmd[40]; int n = 0;
            while (*q && *q != ',' && *q != ';' && n < 39) cmd[n++] = *q++;
            cmd[n] = 0;
            if (n) { fev[nfev].t = at; strcpy(fev[nfev].cmd, cmd); nfev++; }
            if (*q == ',') q++;
        }
        if (*q == ';') q++;
    }
}

void stw_pad_start(void) {
    if (started) return;
    started = 1;
    headless = getenv("STW_HEADLESS") != NULL && !getenv("STW_REALPAD");
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    slotList = [NSMutableArray new];
    rumbleQ = dispatch_queue_create("stw.rumble", DISPATCH_QUEUE_SERIAL);
    if (!headless) {
        GCController.shouldMonitorBackgroundEvents = NO;
        [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) { add_real(n.object); }];
        [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            @synchronized (slotList) { for (StwPadSlot *s in [slotList copy]) if (s->gc == n.object) remove_slot(s); }
        }];
        for (GCController *g in GCController.controllers) add_real(g);
    }
    int nf = getenv("STW_FAKEPAD") ? atoi(getenv("STW_FAKEPAD")) : 0;
    for (int i = 0; i < nf; i++) add_fake();
    if (getenv("STW_FAKEPAD_SCRIPT")) parse_fake_script(getenv("STW_FAKEPAD_SCRIPT"));
}

static void read_gc(GCController *gc, PadRaw *r) {
    memset(r, 0, sizeof *r);
    GCExtendedGamepad *g = gc.extendedGamepad;
    if (!g) return;
    r->src[PS_A] = g.buttonA.value; r->src[PS_B] = g.buttonB.value; r->src[PS_X] = g.buttonX.value; r->src[PS_Y] = g.buttonY.value;
    r->src[PS_LB] = g.leftShoulder.value; r->src[PS_RB] = g.rightShoulder.value;
    r->src[PS_LT] = g.leftTrigger.value; r->src[PS_RT] = g.rightTrigger.value;
    r->src[PS_MENU] = g.buttonMenu.value;
    if (g.buttonOptions) r->src[PS_OPTIONS] = g.buttonOptions.value;
    if (g.buttonHome) r->src[PS_HOME] = g.buttonHome.value;
    if (g.leftThumbstickButton) r->src[PS_LS] = g.leftThumbstickButton.value;
    if (g.rightThumbstickButton) r->src[PS_RS] = g.rightThumbstickButton.value;
    r->src[PS_DUP] = g.dpad.up.value; r->src[PS_DDOWN] = g.dpad.down.value; r->src[PS_DLEFT] = g.dpad.left.value; r->src[PS_DRIGHT] = g.dpad.right.value;
    r->lx = g.leftThumbstick.xAxis.value; r->ly = g.leftThumbstick.yAxis.value;
    r->rx = g.rightThumbstick.xAxis.value; r->ry = g.rightThumbstick.yAxis.value;
    if ([g isKindOfClass:[GCXboxGamepad class]]) {
        GCXboxGamepad *x = (GCXboxGamepad *)g;
        if (x.paddleButton1) r->src[PS_PADDLE1] = x.paddleButton1.value;
        if (x.paddleButton2) r->src[PS_PADDLE2] = x.paddleButton2.value;
        if (x.paddleButton3) r->src[PS_PADDLE3] = x.paddleButton3.value;
        if (x.paddleButton4) r->src[PS_PADDLE4] = x.paddleButton4.value;
    } else if ([g isKindOfClass:[GCDualSenseGamepad class]]) {
        r->src[PS_TOUCHPAD] = ((GCDualSenseGamepad *)g).touchpadButton.value;
    } else if ([g isKindOfClass:[GCDualShockGamepad class]]) {
        r->src[PS_TOUCHPAD] = ((GCDualShockGamepad *)g).touchpadButton.value;
    }
}

int stw_pad_count(void) {
    stw_pad_start();
    @synchronized (slotList) { fake_advance(); return (int)slotList.count; }
}
int stw_pad_read(int port, PadRaw *raw) {
    stw_pad_start();
    @synchronized (slotList) {
        fake_advance();
        if (port < 0 || port >= (int)slotList.count) return 0;
        StwPadSlot *s = slotList[port];
        if (s->fake) *raw = s->fraw; else read_gc(s->gc, raw);
        return 1;
    }
}
const char *stw_pad_name(int port) {
    stw_pad_start();
    static char buf[4][96];
    @synchronized (slotList) {
        if (port < 0 || port >= (int)slotList.count || port > 3) return "";
        snprintf(buf[port], sizeof buf[port], "%s", slotList[port]->name.UTF8String);
        return buf[port];
    }
}
unsigned stw_pad_port_mask(void) { int n = stw_pad_count(); return 1u | ((1u << n) - 1); }
int stw_pad_changes(unsigned *ins, unsigned *rem) {
    static unsigned reported;
    unsigned cur = stw_pad_port_mask();
    *ins = cur & ~reported; *rem = reported & ~cur;
    reported = cur;
    if (*ins || *rem) { if (getenv("STW_PADLOG")) fprintf(stderr, "[pad] device changes seen by the game: inserted %#x removed %#x (now %#x)\n", *ins, *rem, cur); return 1; }
    return 0;
}
int stw_pad_capture(void) {
    stw_pad_start();
    for (int p = 0; p < MAXPADS; p++) {
        PadRaw r;
        if (!stw_pad_read(p, &r)) break;
        for (int s = 0; s < PS_COUNT; s++) if (r.src[s] > 0.6f) return s;
    }
    return -1;
}

/* ---- rumble ---- */
static void set_motor(StwPadSlot *s, int idx, float level) {          /* runs on rumbleQ */
    if (level < 0.02f) {
        if (s->lvl[idx] > 0) { [s->pl[idx] stopAtTime:0 error:nil]; s->lvl[idx] = 0; }
        return;
    }
    NSError *err = nil;
    if (!s->eng[idx]) {
        GCHapticsLocality loc = idx == 0 ? GCHapticsLocalityLeftHandle : GCHapticsLocalityRightHandle;
        CHHapticEngine *e = [s->gc.haptics createEngineWithLocality:loc];
        if (!e) e = [s->gc.haptics createEngineWithLocality:GCHapticsLocalityHandles];
        if (!e) e = [s->gc.haptics createEngineWithLocality:GCHapticsLocalityDefault];
        if (!e) return;
        __weak StwPadSlot *ws = s;
        int i = idx;
        e.stoppedHandler = ^(CHHapticEngineStoppedReason r) { (void)r; StwPadSlot *t = ws; if (t) { t->pl[i] = nil; t->eng[i] = nil; t->lvl[i] = 0; } };
        e.resetHandler = ^{ StwPadSlot *t = ws; if (t) { t->pl[i] = nil; t->eng[i] = nil; t->lvl[i] = 0; } };
        if (![e startAndReturnError:&err]) return;
        s->eng[idx] = e;
    }
    if (!s->pl[idx]) {
        CHHapticEvent *ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
            parameters:@[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f],
                         [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:idx == 0 ? 0.15f : 0.85f]]
            relativeTime:0 duration:5.0];
        CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
        if (!pat) return;
        id<CHHapticAdvancedPatternPlayer> pl = [s->eng[idx] createAdvancedPlayerWithPattern:pat error:&err];
        if (!pl) return;
        pl.loopEnabled = YES;
        s->pl[idx] = pl;
    }
    [s->pl[idx] sendParameters:@[[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level relativeTime:0]]
                        atTime:CHHapticTimeImmediate error:&err];
    if (s->lvl[idx] == 0) [s->pl[idx] startAtTime:0 error:&err];
    s->lvl[idx] = level;
}
void stw_pad_rumble(int port, unsigned left, unsigned right) {
    stw_pad_start();
    StwPadCfg c = stw_settings_get().pad;
    float l = c.rumble ? left / 65535.f * c.rumble_strength : 0, r = c.rumble ? right / 65535.f * c.rumble_strength : 0;
    StwPadSlot *s = nil;
    @synchronized (slotList) { if (port >= 0 && port < (int)slotList.count) s = slotList[port]; }
    if (getenv("STW_RUMBLETRACE")) fprintf(stderr, "[rumble] port %d left %u right %u -> %.2f / %.2f%s\n", port, left, right, l, r, s ? (s->fake ? " (fake pad)" : "") : " (no pad)");
    if (!s || s->fake || !s->gc.haptics) return;
    unsigned gen = ++s->gen;
    dispatch_async(rumbleQ, ^{
        set_motor(s, 0, l);
        set_motor(s, 1, r);
    });
    if (l > 0 || r > 0)                                        /* safety: a rumble the game never switches off ends after 15 s */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), rumbleQ, ^{ if (s->gen == gen) { set_motor(s, 0, 0); set_motor(s, 1, 0); } });
}
