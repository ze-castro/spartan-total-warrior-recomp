/* Keyboard, mouse and game controller input.
 *
 * Every keyboard / mouse control is an action bound to ONE key or mouse button (optionally with required modifiers: Shift, Cmd, Ctrl,
 * Option). The bindings are edited in the launcher window (Controls tab) and stored in stw_settings.txt. A binding fires when its key or
 * button is down and its required modifiers are held; when several bindings share a key or button, the one that requires the most
 * modifiers wins (so Shift + left click = Attack 2 takes the click away from plain Attack 1). Defaults:
 *   W A S D move (Shift held = walk), Space jump, E interact, left click Attack 1, Shift + left click Attack 2, right click shield
 *   (with left click = shield bash), Cmd + left click rage move 1 (click left thumbstick + A), Cmd + Shift + left click rage move 2 (+ B),
 *   shield + jump + arrow = roll toward the arrow, Enter / left click accept (A; on the title screen and in flat menus also Start, because the title
 *   needs it, but never during a scene), Shift + Enter = the Start button itself (continue prompts), Esc pause (Start)
 *   during a scene and back (B) in menus, arrows =
 *   D-pad, mouse left/right = camera, scroll wheel = zoom (up = in). WASD and the arrows also steer menus.
 * The mouse is captured whenever the game window has focus. Sensitivity: the launcher slider (stw_settings.txt), STW_MOUSE_SENS overrides;
 * STW_SCROLL_SENS scales the zoom (STW_SCROLL_INVERT=1 flips its direction), STW_MOUSE_DEADZONE (default 9000) is where the camera turn starts.
 * Controllers: pad.m (GameController framework, up to 4, hot-plug, rumble). The first one shares port 0 with the keyboard and mouse (merged); the
 * mapping, dead zones, inversion and rumble are set in the launcher's Controller tab.
 */
#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "nv2a.h"
#include "paths.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Windowed runs keep a trace of key events and the pad state the game is handed in /tmp/stw_input.log (STW_NOINPUTLOG=1 turns it off). */
#include <stdarg.h>
#include <time.h>
void input_log(const char *fmt, ...) {
    static FILE *f;
    static int off = -1;
    if (off < 0) off = getenv("STW_NOINPUTLOG") != NULL || getenv("STW_HEADLESS") != NULL;
    if (off) return;
    if (!f) { f = fopen("/tmp/stw_input.log", "w"); if (!f) { off = 1; return; } }
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    fprintf(f, "%8.3f ", (t.tv_sec % 100000) + t.tv_nsec / 1e9);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}

static uint8_t keys[128], mbtn[8];
/* While Cmd is down macOS does not send key-up events for the other keys. A key that was held when Cmd went down, or pressed with Cmd held, is
 * therefore "unconfirmed": at every poll it is checked against the real keyboard state, so releasing it while Cmd is still held (hold W, Cmd + click
 * for a rage move, let go of W) cannot leave it stuck. */
static uint8_t unconfirmed[128];
static int mods_held, mouselook;
static double acc_dx, acc_scroll;                   /* mouse motion / scroll wheel since the last poll */
static double zoom_level;                           /* leaky integrator: a wheel flick keeps zooming for a few polls */

/* ---- bindings ---- */
static const struct { const char *name; StwBinding def; } actions[STW_NACTIONS] = {
    [ACT_FORWARD] = { "Move forward", { 1, 13, 0 } },       [ACT_BACK] = { "Move back", { 1, 1, 0 } },
    [ACT_LEFT] = { "Move left", { 1, 0, 0 } },              [ACT_RIGHT] = { "Move right", { 1, 2, 0 } },
    [ACT_WALK] = { "Walk (hold)", { 1, 56, 0 } },           [ACT_JUMP] = { "Jump", { 1, 49, 0 } },
    [ACT_INTERACT] = { "Interact / use object", { 1, 14, 0 } },
    [ACT_ATTACK1] = { "Attack 1", { 2, 0, 0 } },            [ACT_ATTACK2] = { "Attack 2", { 2, 0, MOD_SHIFT } },
    [ACT_SHIELD] = { "Shield (+ attack = shield bash)", { 2, 1, 0 } },
    [ACT_RAGE1] = { "Rage move 1", { 2, 0, MOD_CMD } },     [ACT_RAGE2] = { "Rage move 2", { 2, 0, MOD_CMD | MOD_SHIFT } },
    [ACT_ACCEPT] = { "Accept (menus)", { 1, 36, 0 } },      [ACT_PAUSE] = { "Pause menu / back", { 1, 53, 0 } },
    [ACT_SELECT] = { "Back button", { 1, 48, 0 } },
    [ACT_NAV_UP] = { "D-pad up / roll up", { 1, 126, 0 } }, [ACT_NAV_DOWN] = { "D-pad down / roll down", { 1, 125, 0 } },
    [ACT_NAV_LEFT] = { "D-pad left / roll left", { 1, 123, 0 } }, [ACT_NAV_RIGHT] = { "D-pad right / roll right", { 1, 124, 0 } },
    [ACT_BLACK] = { "Black button", { 1, 6, 0 } },          [ACT_WHITE] = { "White button", { 1, 8, 0 } },
    [ACT_RTRIGGER] = { "Right trigger", { 1, 12, 0 } },
    [ACT_START] = { "Start button (title screen / continue)", { 1, 36, MOD_SHIFT } },
};
const char *stw_action_name(int a) { return a >= 0 && a < STW_NACTIONS ? actions[a].name : "?"; }
static StwBinding binds[STW_NACTIONS];
static void binds_defaults(void) { for (int i = 0; i < STW_NACTIONS; i++) binds[i] = actions[i].def; }

const char *stw_key_name(int code) {
    static const struct { int c; const char *n; } t[] = {
        {0,"A"},{1,"S"},{2,"D"},{3,"F"},{4,"H"},{5,"G"},{6,"Z"},{7,"X"},{8,"C"},{9,"V"},{11,"B"},{12,"Q"},{13,"W"},{14,"E"},{15,"R"},{16,"Y"},{17,"T"},
        {18,"1"},{19,"2"},{20,"3"},{21,"4"},{22,"6"},{23,"5"},{24,"="},{25,"9"},{26,"7"},{27,"-"},{28,"8"},{29,"0"},{30,"]"},{31,"O"},{32,"U"},{33,"["},
        {34,"I"},{35,"P"},{36,"Return"},{37,"L"},{38,"J"},{39,"'"},{40,"K"},{41,";"},{42,"\\"},{43,","},{44,"/"},{45,"N"},{46,"M"},{47,"."},{48,"Tab"},
        {49,"Space"},{50,"`"},{51,"Delete"},{53,"Esc"},{54,"Right Cmd"},{55,"Cmd"},{56,"Shift"},{57,"Caps Lock"},{58,"Option"},{59,"Control"},{60,"Right Shift"},
        {61,"Right Option"},{62,"Right Control"},{123,"Left arrow"},{124,"Right arrow"},{125,"Down arrow"},{126,"Up arrow"},{117,"Fwd Delete"},{115,"Home"},{119,"End"},
        {116,"Page Up"},{121,"Page Down"},
    };
    static char buf[16];
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].c == code) return t[i].n;
    snprintf(buf, sizeof buf, "Key %d", code);
    return buf;
}

/* ---- settings: stw_settings.txt (working directory) ---- */
#define SENS_MIN 0.01
#define SENS_MAX 2.0
static StwSettings cfg = { 0.13333, 2, 0, 0, 1, 0, "", "" };           /* sens 0.13333 = 1/30 of full stick per mouse pixel: the original mouse look */
static int cfg_loaded;
static void cfg_load(void) {
    if (cfg_loaded) return;
    cfg_loaded = 1;
    cfg.bloom_hq = 1;                                   /* quality defaults: smooth bloom on, the rest off */
    binds_defaults();
    stw_pad_defaults(&cfg.pad);
    char sp[1100];
    stw_path(sp, sizeof sp, "stw_settings.txt");
    FILE *f = fopen(sp, "r");
    if (f) {
        char line[1100]; double v; int n, k, c, m;
        double ver = 1;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "game_root=", 10)) snprintf(cfg.game_root, sizeof cfg.game_root, "%s", line + 10);
            else if (!strncmp(line, "iso=", 4)) snprintf(cfg.iso, sizeof cfg.iso, "%s", line + 4);
            else if (!strncmp(line, "pad_map=", 8)) {                       /* controller mapping: 16 comma separated source numbers */
                int m[PO_COUNT]; int got = sscanf(line + 8, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7],
                                                  &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]);
                if (got == PO_COUNT) for (int i = 0; i < PO_COUNT; i++) cfg.pad.map[i] = m[i] >= -1 && m[i] < PS_COUNT ? m[i] : cfg.pad.map[i];
            }
            else if (sscanf(line, "pad_dz_left=%lf", &v) == 1) cfg.pad.dz_left = (float)fmin(fmax(v, 0), 0.5);
            else if (sscanf(line, "pad_dz_right=%lf", &v) == 1) cfg.pad.dz_right = (float)fmin(fmax(v, 0), 0.5);
            else if (sscanf(line, "pad_sens_right=%lf", &v) == 1) cfg.pad.sens_right = (float)fmin(fmax(v, 0.25), 3.0);
            else if (sscanf(line, "pad_trig=%lf", &v) == 1) cfg.pad.trig_thresh = (float)fmin(fmax(v, 0.05), 0.95);
            else if (sscanf(line, "pad_inv_ly=%lf", &v) == 1) cfg.pad.invert_ly = v != 0;
            else if (sscanf(line, "pad_inv_ry=%lf", &v) == 1) cfg.pad.invert_ry = v != 0;
            else if (sscanf(line, "pad_swap=%lf", &v) == 1) cfg.pad.swap_face = v != 0;
            else if (sscanf(line, "pad_rumble=%lf", &v) == 1) cfg.pad.rumble = v != 0;
            else if (sscanf(line, "pad_rumble_strength=%lf", &v) == 1) cfg.pad.rumble_strength = (float)fmin(fmax(v, 0), 1);
            else if (sscanf(line, "version=%lf", &v) == 1) ver = v;
            else if (sscanf(line, "sensitivity=%lf", &v) == 1 && v >= SENS_MIN && v <= SENS_MAX) cfg.sens = v;
            else if (sscanf(line, "scale=%lf", &v) == 1 && v >= 1 && v <= 4) cfg.scale = (int)v == 1 ? 0 : (int)v == 2 ? 2 : (int)v;   /* old whole-multiple setting: 1x -> Original, 2x -> 1080p, 3x -> 1440p, 4x -> 2160p */
            else if (sscanf(line, "res=%lf", &v) == 1 && v >= 0 && v <= 4) cfg.scale = (int)v;
            else if (sscanf(line, "aa=%lf", &v) == 1 && v >= 0 && v <= 3) cfg.aa = (int)v;
            else if (sscanf(line, "sharpen=%lf", &v) == 1 && v >= 0 && v <= 100) cfg.sharpen = (int)v;
            else if (sscanf(line, "bloomhq=%lf", &v) == 1) cfg.bloom_hq = v != 0;
            else if (sscanf(line, "hud=%lf", &v) == 1) cfg.hud = v != 0;
            else if (sscanf(line, "fullscreen=%lf", &v) == 1) cfg.fullscreen = v != 0;
            else if (sscanf(line, "launcher=%lf", &v) == 1) cfg.show_launcher = v != 0;
            else if (sscanf(line, "widescreen=%lf", &v) == 1) cfg.widescreen = v != 0;
            else if (sscanf(line, "bind%d=%d,%d,%d", &n, &k, &c, &m) == 4 && n >= 0 && n < STW_NACTIONS && k >= 0 && k <= 2 && c >= 0 && c < 128) binds[n] = (StwBinding){ k, c, m & 15 };
        }
        fclose(f);
        if (ver < 3) cfg.sens = 0.25;                     /* older files used another sensitivity scale */
        if (ver < 3) binds_defaults();                     /* and an older control scheme */
    }
    if (getenv("STW_MOUSE_SENS")) cfg.sens = atof(getenv("STW_MOUSE_SENS"));
}
static void cfg_save(void) {
    char sp[1100];
    stw_path(sp, sizeof sp, "stw_settings.txt");
    FILE *f = fopen(sp, "w");
    if (!f) return;
    fprintf(f, "version=4\nsensitivity=%.5f\nres=%d\nhud=%d\nfullscreen=%d\nlauncher=%d\nwidescreen=%d\n", cfg.sens, cfg.scale, cfg.hud, cfg.fullscreen, cfg.show_launcher, cfg.widescreen);
    for (int i = 0; i < STW_NACTIONS; i++) fprintf(f, "bind%d=%d,%d,%d\n", i, binds[i].kind, binds[i].code, binds[i].mods);
    fprintf(f, "aa=%d\nsharpen=%d\nbloomhq=%d\n", cfg.aa, cfg.sharpen, cfg.bloom_hq);
    fprintf(f, "game_root=%s\niso=%s\n", cfg.game_root, cfg.iso);
    fprintf(f, "pad_map=");
    for (int i = 0; i < PO_COUNT; i++) fprintf(f, "%d%s", cfg.pad.map[i], i + 1 < PO_COUNT ? "," : "\n");
    fprintf(f, "pad_dz_left=%.3f\npad_dz_right=%.3f\npad_sens_right=%.3f\npad_trig=%.3f\npad_inv_ly=%d\npad_inv_ry=%d\npad_swap=%d\npad_rumble=%d\npad_rumble_strength=%.3f\n",
            cfg.pad.dz_left, cfg.pad.dz_right, cfg.pad.sens_right, cfg.pad.trig_thresh, cfg.pad.invert_ly, cfg.pad.invert_ry, cfg.pad.swap_face, cfg.pad.rumble, cfg.pad.rumble_strength);
    fclose(f);
}
StwSettings stw_settings_get(void) { pthread_mutex_lock(&lock); cfg_load(); StwSettings c = cfg; pthread_mutex_unlock(&lock); return c; }
void stw_settings_put(const StwSettings *c) {
    pthread_mutex_lock(&lock);
    cfg_load();
    cfg = *c;
    if (cfg.sens < SENS_MIN) cfg.sens = SENS_MIN;
    if (cfg.sens > SENS_MAX) cfg.sens = SENS_MAX;
    cfg_save();
    pthread_mutex_unlock(&lock);
}
StwBinding stw_binding_get(int a) { pthread_mutex_lock(&lock); cfg_load(); StwBinding b = binds[a]; pthread_mutex_unlock(&lock); return b; }
void stw_binding_put(int a, StwBinding b) { pthread_mutex_lock(&lock); cfg_load(); binds[a] = b; cfg_save(); pthread_mutex_unlock(&lock); }
StwBinding stw_binding_default(int a) { return actions[a].def; }
void stw_bindings_reset(void) { pthread_mutex_lock(&lock); cfg_load(); binds_defaults(); cfg_save(); pthread_mutex_unlock(&lock); }
double input_sensitivity(void) { return stw_settings_get().sens; }

/* ---- events from the window ---- */
void input_scroll(double d) { pthread_mutex_lock(&lock); acc_scroll += d; pthread_mutex_unlock(&lock); }
void input_key(uint16_t code, int down) {
    if (code >= 128) return;
    pthread_mutex_lock(&lock);
    keys[code] = down;
    unconfirmed[code] = down && (mods_held & MOD_CMD);              /* pressed with Cmd held: its key-up may never arrive */
    pthread_mutex_unlock(&lock);
}
void input_mods(int mask) {                          /* modifier keys also count as keys, so "Shift" can be a binding of its own */
    pthread_mutex_lock(&lock);
    if ((mask & MOD_CMD) && !(mods_held & MOD_CMD))                 /* Cmd went down: every key held right now loses its key-up from here on */
        for (int c = 0; c < 128; c++) if (keys[c] && c != 54 && c != 55) unconfirmed[c] = 1;
    mods_held = mask;
    keys[56] = keys[60] = (mask & MOD_SHIFT) != 0;
    keys[55] = keys[54] = (mask & MOD_CMD) != 0;
    keys[59] = keys[62] = (mask & MOD_CTRL) != 0;
    keys[58] = keys[61] = (mask & MOD_OPT) != 0;
    pthread_mutex_unlock(&lock);
}
void input_release_all(void) { pthread_mutex_lock(&lock); memset(keys, 0, sizeof keys); memset(unconfirmed, 0, sizeof unconfirmed); memset(mbtn, 0, sizeof mbtn); mods_held = 0; pthread_mutex_unlock(&lock); }
void input_mouse_button(int b, int down) { if (b >= 0 && b < 8) { pthread_mutex_lock(&lock); mbtn[b] = down; pthread_mutex_unlock(&lock); } }
void input_mouse_move(double dx, double dy) { (void)dy; pthread_mutex_lock(&lock); acc_dx += dx; pthread_mutex_unlock(&lock); }     /* vertical motion is ignored */
int input_mouselook(void) { return mouselook; }
void input_set_mouselook(int on) {
    if (on == mouselook) return;
    mouselook = on;
    pthread_mutex_lock(&lock); acc_dx = acc_scroll = 0; zoom_level = 0; pthread_mutex_unlock(&lock);
    CGAssociateMouseAndMouseCursorPosition(!on);
    if (on) [NSCursor hide]; else [NSCursor unhide];
}

static int16_t axis(float v) { v = v > 1 ? 1 : v < -1 ? -1 : v; return (int16_t)(v * 32767); }
static uint8_t trig(float v) { return (uint8_t)(v * 255); }

/* ---- camera ---- */
/* Which way the game's right stick Y zooms: -1 = scroll up pushes the stick down, which zooms the game's camera in. */
#define ZOOM_IN_STICK -1
#define LOOK_STICK_CAP 32767.0                       /* highest stick value mouse look may use */
/* Horizontal mouse motion -> right stick X. Like pushing a joystick: deflection follows the mouse speed, with no smoothing tail, starting
 * at the game's dead zone (where the game's own turn is already slow). */
static int16_t look_stick(double px, double sens) {
    static double dz = -1;
    if (dz < 0) dz = getenv("STW_MOUSE_DEADZONE") ? atof(getenv("STW_MOUSE_DEADZONE")) : 9000;
    double v = px * sens / 4.0;
    if (v == 0) return 0;
    double a = fabs(v) > 1 ? 1 : fabs(v);
    return (int16_t)((v < 0 ? -1 : 1) * (dz + a * (LOOK_STICK_CAP - dz)));
}
void input_look_reset(void) {}

/* Scroll zoom: a stick amount in -1..1 pushed past the game's dead zone. */
static int16_t look_axis(double v, double dz) {
    if (v == 0) return 0;
    double a = fabs(v) > 1 ? 1 : fabs(v);
    return (int16_t)((v < 0 ? -1 : 1) * (dz + a * (32767 - dz)));
}
static void apply_look(PadState *p, double dx, double zoom) {
    int16_t lk = look_stick(dx, input_sensitivity());
    if (lk) p->rx = lk;
    static int flip = -1;
    if (flip < 0) flip = getenv("STW_SCROLL_INVERT") && atoi(getenv("STW_SCROLL_INVERT"));   /* scroll up zooms out instead */
    if (zoom != 0) p->ry = look_axis(zoom, 9000) * ZOOM_IN_STICK * (flip ? -1 : 1);
}
/* STW_SCRIPT driver: left / right click, Shift, mouse pixels and zoom as stick amount. */
void input_script_mouse(PadState *p, int left, int right, int shift, double dx, double zoom) {
    if (left) p->analog[shift ? 1 : 0] = 255;       /* A: attack 1, Shift: B: attack 2 */
    if (right) p->analog[6] = 255;                  /* left trigger: shield */
    apply_look(p, dx, zoom);
}

/* ---- keyboard / mouse -> pad ---- */
static int src_down(StwBinding b, const uint8_t *k, const uint8_t *mb) { return b.kind == 1 ? k[b.code] : b.kind == 2 ? (b.code < 8 && mb[b.code]) : 0; }
static int popcount4(int m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1) + ((m >> 3) & 1); }
static int is_active(int act, const uint8_t *k, const uint8_t *mb, int mods) {
    StwBinding b = binds[act];
    if (!b.kind || !src_down(b, k, mb) || (b.mods & ~mods)) return 0;
    for (int j = 0; j < STW_NACTIONS; j++) {          /* a more specific binding of the same key / button takes it over */
        if (j == act || binds[j].kind != b.kind || binds[j].code != b.code) continue;
        if (!(binds[j].mods & ~mods) && popcount4(binds[j].mods) > popcount4(b.mods)) return 0;
    }
    return 1;
}

/* Keyboard / mouse and a controller on the same port: buttons are ORed, analog buttons take the larger value, each stick the larger deflection. */
static void pad_merge(PadState *d, const PadState *s) {
    d->buttons |= s->buttons;
    for (int i = 0; i < 8; i++) if (s->analog[i] > d->analog[i]) d->analog[i] = s->analog[i];
    if (abs(s->lx) + abs(s->ly) > abs(d->lx) + abs(d->ly)) { d->lx = s->lx; d->ly = s->ly; }
    if (abs(s->rx) + abs(s->ry) > abs(d->rx) + abs(d->ry)) { d->rx = s->rx; d->ry = s->ry; }
}

void input_poll(PadState *p) {
    memset(p, 0, sizeof *p);
    uint8_t k[128], mb[8];
    int mods;
    double dx, scroll;
    pthread_mutex_lock(&lock);
    cfg_load();
    for (int c = 0; c < 128; c++)                                   /* drop unconfirmed keys the keyboard says are up */
        if (unconfirmed[c] && keys[c] && !CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, (CGKeyCode)c)) keys[c] = 0;
    for (int c = 0; c < 128; c++) if (!keys[c]) unconfirmed[c] = 0;
    memcpy(k, keys, sizeof k); memcpy(mb, mbtn, sizeof mb); mods = mods_held;
    dx = acc_dx; scroll = acc_scroll; acc_dx = acc_scroll = 0;
    int on[STW_NACTIONS];
    for (int i = 0; i < STW_NACTIONS; i++) on[i] = is_active(i, k, mb, mods);
    pthread_mutex_unlock(&lock);
    int scene = nv2a_scene_3d();
    int nav = on[ACT_NAV_UP] | on[ACT_NAV_DOWN] | on[ACT_NAV_LEFT] | on[ACT_NAV_RIGHT];
    int roll = on[ACT_SHIELD] && on[ACT_JUMP] && nav;       /* shield + jump + arrow: the arrow picks the roll direction */
    float mx = on[ACT_RIGHT] - on[ACT_LEFT], my = on[ACT_FORWARD] - on[ACT_BACK];
    if (roll) { mx = on[ACT_NAV_RIGHT] - on[ACT_NAV_LEFT]; my = on[ACT_NAV_UP] - on[ACT_NAV_DOWN]; }
    float len = sqrtf(mx * mx + my * my);
    if (len > 0) {
        float mag = (on[ACT_WALK] && !roll) ? 0.45f : 1.0f;
        p->lx = axis(mx / len * mag); p->ly = axis(my / len * mag);
    }
    if (!roll) {
        if (on[ACT_NAV_UP]) p->buttons |= 0x1;    if (on[ACT_NAV_DOWN]) p->buttons |= 0x2;
        if (on[ACT_NAV_LEFT]) p->buttons |= 0x4;  if (on[ACT_NAV_RIGHT]) p->buttons |= 0x8;
    }
    if (!scene) {                                        /* menus: the movement keys steer the D-pad and left stick too */
        if (on[ACT_FORWARD]) p->buttons |= 0x1;   if (on[ACT_BACK]) p->buttons |= 0x2;
        if (on[ACT_LEFT]) p->buttons |= 0x4;      if (on[ACT_RIGHT]) p->buttons |= 0x8;
        float ax = (on[ACT_RIGHT] | on[ACT_NAV_RIGHT]) - (on[ACT_LEFT] | on[ACT_NAV_LEFT]), ay = (on[ACT_FORWARD] | on[ACT_NAV_UP]) - (on[ACT_BACK] | on[ACT_NAV_DOWN]);
        float al = sqrtf(ax * ax + ay * ay);
        if (al > 0) { p->lx = axis(ax / al); p->ly = axis(ay / al); }
    }
    if (on[ACT_PAUSE]) { if (scene) p->buttons |= 0x10; else p->analog[1] = 255; }      /* pause menu during a scene, back in menus */
    if (on[ACT_ACCEPT]) p->analog[0] = 255;                                              /* accept: A */
    if (!scene && (on[ACT_ACCEPT] || on[ACT_ATTACK1])) p->buttons |= 0x10;               /* title screen / flat menus: "press start" is accepted as well; never during a scene (it would pause) */
    if (on[ACT_START]) p->buttons |= 0x10;                                               /* the Start button itself (title screen, continue prompts) */
    if (on[ACT_SELECT]) p->buttons |= 0x20;
    if (on[ACT_INTERACT]) p->analog[2] = 255;
    if (on[ACT_JUMP]) p->analog[3] = 255;
    if (on[ACT_BLACK]) p->analog[4] = 255;    if (on[ACT_WHITE]) p->analog[5] = 255;
    if (on[ACT_ATTACK1]) p->analog[0] = 255;  if (on[ACT_ATTACK2]) p->analog[1] = 255;
    if (on[ACT_SHIELD]) p->analog[6] = 255;
    if (on[ACT_RTRIGGER]) p->analog[7] = 255;
    if (on[ACT_RAGE1]) { p->buttons |= 0x40; p->analog[0] = 255; }                       /* rage move: click the left thumbstick + A / B */
    if (on[ACT_RAGE2]) { p->buttons |= 0x40; p->analog[1] = 255; }
    static double scroll_gain = -1;
    if (scroll_gain < 0) scroll_gain = getenv("STW_SCROLL_SENS") ? atof(getenv("STW_SCROLL_SENS")) : 1.0;
    zoom_level = zoom_level * 0.8 + scroll * 0.5 * scroll_gain;
    if (fabs(zoom_level) < 0.02) zoom_level = 0;
    if (mouselook && scene) apply_look(p, dx, zoom_level);   /* no camera or zoom in menus */

    PadRaw raw;                                           /* the first controller shares port 0 with the keyboard and mouse */
    if (stw_pad_read(0, &raw)) {
        PadState gp;
        pthread_mutex_lock(&lock);
        StwPadCfg pc = cfg.pad;
        pthread_mutex_unlock(&lock);
        stw_pad_map(&raw, &pc, &gp);
        pad_merge(p, &gp);
    }
}

int input_poll_port(int port, PadState *p) {
    if (port == 0) { input_poll(p); return 0; }
    memset(p, 0, sizeof *p);
    PadRaw raw;
    if (port < 0 || port >= 4 || !stw_pad_read(port, &raw)) return 1;
    pthread_mutex_lock(&lock);
    cfg_load();
    StwPadCfg pc = cfg.pad;
    pthread_mutex_unlock(&lock);
    stw_pad_map(&raw, &pc, p);
    return 0;
}
