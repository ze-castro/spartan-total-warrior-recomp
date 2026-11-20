/* Host input -> Xbox gamepad state. */
#ifndef STW_INPUT_H
#define STW_INPUT_H
#include <stdint.h>
#include "pad.h"                          /* PadState, StwPadCfg */
#define MOD_SHIFT 1
#define MOD_CMD 2
#define MOD_CTRL 4
#define MOD_OPT 8
/* Keyboard / mouse actions; each is bound to one key or mouse button (editable in the launcher). */
enum { ACT_FORWARD, ACT_BACK, ACT_LEFT, ACT_RIGHT, ACT_WALK, ACT_JUMP, ACT_INTERACT, ACT_ATTACK1, ACT_ATTACK2, ACT_SHIELD, ACT_RAGE1, ACT_RAGE2,
       ACT_ACCEPT, ACT_PAUSE, ACT_SELECT, ACT_NAV_UP, ACT_NAV_DOWN, ACT_NAV_LEFT, ACT_NAV_RIGHT, ACT_BLACK, ACT_WHITE, ACT_RTRIGGER, ACT_START, STW_NACTIONS };
typedef struct { int kind, code, mods; } StwBinding;       /* kind: 0 none, 1 key (macOS key code), 2 mouse button (0 left, 1 right, 2+ other) */
const char *stw_action_name(int action);
const char *stw_key_name(int keycode);
StwBinding stw_binding_get(int action);
StwBinding stw_binding_default(int action);
void stw_binding_put(int action, StwBinding b);             /* saves */
void stw_bindings_reset(void);
void input_key(uint16_t keycode, int down);   /* from the window */
void input_log(const char *fmt, ...);          /* windowed-run trace file /tmp/stw_input.log */
void input_release_all(void);                 /* focus lost: forget held keys and buttons */
void input_mods(int mask);                    /* modifier state (MOD_*); modifier keys also count as keys */
void input_mouse_button(int button, int down);   /* 0 = left, 1 = right, 2+ = other */
void input_mouse_move(double dx, double dy);     /* raw pixel deltas, accumulated until the next poll */
void input_scroll(double d);                     /* scroll wheel units (one notch = 1; up = positive) */
typedef struct {
    double sens; int scale; int hud; int fullscreen; int show_launcher;
    int widescreen;                                /* 0 = 4:3 (the console's picture), 1 = 16:9 anamorphic */
    char game_root[1024];                          /* folder with the extracted disc ("" = <data dir>/game) */
    char iso[1024];                                /* the disc image it was extracted from (informational) */
    /* quality extras (launcher "Quality" tab) */
    int aa;                                        /* 0 off, 1 FXAA post-filter, 2 MSAA 2x, 3 MSAA 4x (main scene target) */
    int sharpen;                                   /* output sharpening strength 0..100 (contrast-adaptive sharpen), 0 = off */
    int bloom_hq;                                  /* smooth (bicubic) upsampling of the low-resolution bloom textures at internal scale > 1 */
    StwPadCfg pad;                                 /* game controller mapping and options (Controller tab) */
} StwSettings;   /* stw_settings.txt */
StwSettings stw_settings_get(void);
void stw_settings_put(const StwSettings *c);       /* clamps and saves */
double input_sensitivity(void);                  /* camera sensitivity: full stick per 1/x pixels per poll */
int input_mouselook(void);                       /* mouse look active (cursor captured) */
void input_set_mouselook(int on);                /* main thread only: captures / releases the cursor */
void input_look_reset(void);
/* Merges mouse state into a pad (used by input_poll and by the STW_SCRIPT driver: dx in pixels per poll, zoom = stick amount). */
void input_script_mouse(PadState *p, int left, int right, int shift, double dx, double zoom);
void input_poll(PadState *out);               /* port 0: keyboard + mouse + the first controller */
int input_poll_port(int port, PadState *out); /* 0 = connected: port 0 = keyboard/mouse + controller 1, ports 1-3 = controllers 2-4 */
#endif
