/* Game controllers: GameController framework layer (hot-plug, up to 4 ports), mapping onto the Xbox pad, rumble.
 * stw_pad_map() is a pure function so the tables can be tested without hardware; STW_FAKEPAD injects pads for headless tests. */
#ifndef STW_PAD_H
#define STW_PAD_H
#include <stdint.h>

typedef struct {
    uint16_t buttons;               /* XINPUT_GAMEPAD_*: dpad 0x1..0x8, start 0x10, back 0x20, l/r thumb 0x40/0x80 */
    uint8_t analog[8];              /* A, B, X, Y, Black, White, LeftTrigger, RightTrigger (0..255) */
    int16_t lx, ly, rx, ry;
} PadState;

/* Physical inputs of a modern pad, named by position on an Xbox pad (PlayStation: Cross=A, Circle=B, Square=X, Triangle=Y, L1/R1 = LB/RB,
 * Options = Menu, Share/Create = Options, PS = Home; Switch Pro: the positional equivalents). */
enum { PS_A, PS_B, PS_X, PS_Y, PS_LB, PS_RB, PS_LT, PS_RT, PS_MENU, PS_OPTIONS, PS_HOME, PS_LS, PS_RS,
       PS_DUP, PS_DDOWN, PS_DLEFT, PS_DRIGHT, PS_PADDLE1, PS_PADDLE2, PS_PADDLE3, PS_PADDLE4, PS_TOUCHPAD, PS_COUNT };
/* Outputs: the buttons of the original Xbox pad. */
enum { PO_A, PO_B, PO_X, PO_Y, PO_BLACK, PO_WHITE, PO_LT, PO_RT, PO_START, PO_BACK, PO_LTHUMB, PO_RTHUMB,
       PO_DUP, PO_DDOWN, PO_DLEFT, PO_DRIGHT, PO_COUNT };

typedef struct { float src[PS_COUNT]; float lx, ly, rx, ry; } PadRaw;      /* buttons 0..1, sticks -1..1 (up/right positive) */

typedef struct {
    int map[PO_COUNT];          /* PS_* source of every Xbox output; -1 = none */
    float dz_left, dz_right;    /* extra radial dead zone (0..0.5) on top of the OS's; the game has its own ~0.27, so the default is small (drift only) */
    float sens_right;           /* camera stick scale (0.5 .. 2.0) */
    float trig_thresh;          /* analog trigger value that counts as pressed when it drives a digital Xbox button */
    int invert_ly, invert_ry;
    int swap_face;              /* swap A<->B and X<->Y (Nintendo-style labels) */
    int rumble;                 /* 0/1 */
    float rumble_strength;      /* 0..1 */
} StwPadCfg;

void stw_pad_defaults(StwPadCfg *c);
const char *stw_pad_src_name(int src);
const char *stw_pad_out_name(int out);

/* raw physical state -> the Xbox XINPUT_GAMEPAD fields */
void stw_pad_map(const PadRaw *raw, const StwPadCfg *c, PadState *out);

void stw_pad_start(void);                                  /* register for connect / disconnect, pick up pads that are already connected */
int stw_pad_count(void);                                   /* controllers present (ports 0 .. n-1) */
int stw_pad_read(int port, PadRaw *raw);                   /* 1 if a controller is on that port */
const char *stw_pad_name(int port);                        /* product name for the UI, "" if none */
/* Ports as the game sees them: port 0 is always present (keyboard / mouse), extra controllers fill ports 0.. in connection order. */
unsigned stw_pad_port_mask(void);
int stw_pad_changes(unsigned *inserted, unsigned *removed);  /* diff since the previous call (for XGetDeviceChanges); returns 1 if anything changed */
void stw_pad_rumble(int port, unsigned left, unsigned right);   /* Xbox motor speeds 0..65535 (left = heavy, right = light) */
int stw_pad_capture(void);                                 /* the first physical input currently pressed (PS_*), or -1: for "press a button" binding */
#endif
