/* XAPI peripheral (XPP) replacements: the game's controller API, without the USB host controller
 * underneath. One gamepad on port 0 is always present. Everything not overridden here (the rest of
 * XPP, XGRPH, D3DX ...) is lifted and runs natively. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lib.h"
#include "kernel.h"
#include "input.h"
#include "nv2a.h"

#define ERROR_SUCCESS 0
#define PAD_HANDLE_BASE 0x4000u

uint32_t lib_XInitDevices(CPU *c, const uint32_t *a) { (void)c; (void)a; stw_pad_start(); return 0; }

/* The ports the game sees: port 0 is always present (keyboard and mouse, plus the first controller); more controllers fill ports 1-3. */
#define ERROR_DEVICE_NOT_CONNECTED 1167
#define ERROR_IO_PENDING 997
uint32_t lib_XGetDevices(CPU *c, const uint32_t *a) { (void)c; (void)a; return stw_pad_port_mask(); }   /* XGetDevices(DeviceType*) -> bitmask of connected ports */

/* XGetDeviceChanges(DeviceType*, DWORD *insertions, DWORD *removals): the first call reports the pads as inserted; later calls report hot-plug. */
uint32_t lib_XGetDeviceChanges(CPU *c, const uint32_t *a) {
    (void)c;
    unsigned ins, rem;
    int changed = stw_pad_changes(&ins, &rem);
    if (a[1]) ST32(a[1], ins);
    if (a[2]) ST32(a[2], rem);
    return changed;
}

/* XInputOpen(DeviceType*, port, slot, XINPUT_POLLING_PARAMETERS*): the handle encodes the port */
uint32_t lib_XInputOpen(CPU *c, const uint32_t *a) { (void)c; return a[1] < 4 && (stw_pad_port_mask() >> a[1] & 1) ? PAD_HANDLE_BASE + a[1] : 0; }
uint32_t lib_XInputClose(CPU *c, const uint32_t *a) { (void)c; (void)a; return 0; }

/* XINPUT_CAPABILITIES { BYTE SubType; BYTE Reserved; XINPUT_STATE In; XINPUT_RUMBLE Out; } */
uint32_t lib_XInputGetCapabilities(CPU *c, const uint32_t *a) {
    (void)c;
    memset(G + a[1], 0, 28);
    ST8(a[1], 1);                                        /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    return ERROR_SUCCESS;
}

/* XINPUT_STATE { DWORD dwPacketNumber; XINPUT_GAMEPAD { WORD wButtons; BYTE bAnalogButtons[8];
 *   SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } } = 22 bytes. */
/* STW_SCRIPT="8:start;12:a;14:down" presses each button for 0.3 s at the given second after startup
 * (debugging aid for driving the menus without a keyboard). Buttons: start back a b x y up down left right. */
static void scripted_input(PadState *p) {
    static const char *script;
    static struct timespec t0;
    if (!script) { script = getenv("STW_SCRIPT") ? getenv("STW_SCRIPT") : ""; clock_gettime(CLOCK_MONOTONIC, &t0); }
    if (!*script) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double now = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    int m_l = 0, m_r = 0, m_shift = 0;
    double m_dx = 0, m_dy = 0;                              /* mouse emulation: dx pixels per poll, dy = zoom stick amount */
    for (const char *q = script; *q;) {
        double at = strtod(q, (char **)&q);
        if (*q == ':') q++;
        char name[16]; int n = 0;
        double dur = 0.3;                                   /* "l_up*3" holds for 3 s */
        while (*q && *q != ';' && *q != '*' && n < 15) name[n++] = *q++;
        name[n] = 0;
        if (*q == '*') { q++; dur = strtod(q, (char **)&q); }
        if (*q == ';') q++;
        if (now < at || now > at + dur) continue;
        if (!strcmp(name, "start")) p->buttons |= 0x10;
        else if (!strcmp(name, "back")) p->buttons |= 0x20;
        else if (!strcmp(name, "up")) p->buttons |= 0x1;
        else if (!strcmp(name, "down")) p->buttons |= 0x2;
        else if (!strcmp(name, "left")) p->buttons |= 0x4;
        else if (!strcmp(name, "right")) p->buttons |= 0x8;
        else if (!strcmp(name, "a")) p->analog[0] = 255;
        else if (!strcmp(name, "b")) p->analog[1] = 255;
        else if (!strcmp(name, "x")) p->analog[2] = 255;
        else if (!strcmp(name, "y")) p->analog[3] = 255;
        else if (!strcmp(name, "l_up")) p->ly = 30000;
        else if (!strcmp(name, "l_down")) p->ly = -30000;
        else if (!strcmp(name, "l_left")) p->lx = -30000;
        else if (!strcmp(name, "l_right")) p->lx = 30000;
        else if (!strcmp(name, "r_left")) p->rx = -30000;
        else if (!strcmp(name, "r_right")) p->rx = 30000;
        else if (!strcmp(name, "lt")) p->analog[6] = 255;
        else if (!strcmp(name, "rt")) p->analog[7] = 255;
        else if (!strcmp(name, "white")) p->analog[5] = 255;
        else if (!strcmp(name, "black")) p->analog[4] = 255;
        else if (!strcmp(name, "lclick")) m_l = 1;           /* left click: A, or B with shift */
        else if (!strcmp(name, "rclick")) m_r = 1;           /* right click: left trigger (shield) */
        else if (!strcmp(name, "shift")) m_shift = 1;
        else if (!strncmp(name, "mdx=", 4)) m_dx = atof(name + 4);   /* mouse pixels per poll (negative = left) */
        else if (!strncmp(name, "rx=", 3)) p->rx = (int16_t)atoi(name + 3);    /* raw right stick values, for measuring the camera response */
        else if (!strncmp(name, "ry=", 3)) p->ry = (int16_t)atoi(name + 3);
        else if (!strcmp(name, "m_right")) m_dx = 15;
        else if (!strcmp(name, "m_left")) m_dx = -15;
        else if (!strcmp(name, "s_up")) m_dy = 0.6;          /* scroll wheel zoom in / out (stick amount) */
        else if (!strcmp(name, "s_down")) m_dy = -0.6;
        else if (!strcmp(name, "r_up")) p->ry = 30000;
        else if (!strcmp(name, "r_down")) p->ry = -30000;
    }
    input_script_mouse(p, m_l, m_r, m_shift, m_dx, m_dy);   /* every poll: the look controller pays out owed rotation after the token ends */
}

uint32_t lib_XInputGetState(CPU *c, const uint32_t *a) {      /* (HANDLE, XINPUT_STATE *) */
    (void)c;
    static uint32_t packet;
    PadState p;
    memset(&p, 0, sizeof p);
    int port = a[0] >= PAD_HANDLE_BASE && a[0] < PAD_HANDLE_BASE + 4 ? (int)(a[0] - PAD_HANDLE_BASE) : 0;
    if (input_poll_port(port, &p)) return ERROR_DEVICE_NOT_CONNECTED;       /* a controller was unplugged */
    if (port == 0) scripted_input(&p);
    {                                                       /* windowed runs: trace pad changes (and the scene flag) to /tmp/stw_input.log */
        static PadState seen;
        if (memcmp(&p, &seen, sizeof p)) {
            input_log("pad btn=%02x A=%d B=%d X=%d Y=%d LT=%d RT=%d L=(%d,%d) R=(%d,%d) scene3d=%d", p.buttons, p.analog[0] > 0, p.analog[1] > 0,
                      p.analog[2] > 0, p.analog[3] > 0, p.analog[6], p.analog[7], p.lx, p.ly, p.rx, p.ry, nv2a_scene_3d());
            seen = p;
        }
    }
    if (getenv("STW_INPUTLOG")) {                           /* prints each change of the pad state handed to the game */
        static PadState last;
        if (memcmp(&p, &last, sizeof p)) {
            fprintf(stderr, "[pad] btn=%02x A=%d B=%d X=%d Y=%d blk=%d wht=%d LT=%d RT=%d L=(%d,%d) R=(%d,%d)\n", p.buttons, p.analog[0] > 0,
                    p.analog[1] > 0, p.analog[2] > 0, p.analog[3] > 0, p.analog[4] > 0, p.analog[5] > 0, p.analog[6], p.analog[7], p.lx, p.ly, p.rx, p.ry);
            last = p;
        }
    }
    if (getenv("STW_POLLSTAT")) {                           /* XInputGetState calls per second (a busy game thread polls less) */
        static struct timespec t0; static uint32_t n;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if (!t0.tv_sec) t0 = t;
        n++;
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= 1000) { fprintf(stderr, "[poll] %u XInputGetState/s at +%lds\n", n, (long)(t.tv_sec % 100000)); n = 0; t0 = t; }
    }
    memset(G + a[1], 0, 22);
    ST32(a[1], ++packet);
    ST16(a[1] + 4, p.buttons);
    for (int i = 0; i < 8; i++) ST8(a[1] + 6 + i, p.analog[i]);
    ST16(a[1] + 14, (uint16_t)p.lx); ST16(a[1] + 16, (uint16_t)p.ly);
    ST16(a[1] + 18, (uint16_t)p.rx); ST16(a[1] + 20, (uint16_t)p.ry);
    return ERROR_SUCCESS;
}
uint32_t lib_XInputSetState(CPU *c, const uint32_t *a) {   /* (HANDLE, XINPUT_FEEDBACK *): Header { dwStatus, hEvent, Reserved[58] }, then XINPUT_RUMBLE { WORD left, right } at +66 */
    (void)c;
    int port = a[0] >= PAD_HANDLE_BASE && a[0] < PAD_HANDLE_BASE + 4 ? (int)(a[0] - PAD_HANDLE_BASE) : 0;
    unsigned left = LD16(a[1] + 66), right = LD16(a[1] + 68);
    if (getenv("STW_RUMBLETRACE")) fprintf(stderr, "[rumble] game request: handle=%#x left=%#x right=%#x\n", a[0], left, right);
    ST32(a[1], ERROR_SUCCESS);                           /* Header.dwStatus: the request is already complete */
    stw_pad_rumble(port, left, right);
    return ERROR_SUCCESS;
}

