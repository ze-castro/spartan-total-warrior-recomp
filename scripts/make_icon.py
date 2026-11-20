#!/usr/bin/env python3
"""Draws the app icon (a bronze Spartan lambda on a crimson shield) with nothing but the standard library: usage make_icon.py out.png [size]."""
import math, struct, sys, zlib

size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
S = size

def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

def over(bg, fg, a):
    return tuple(int(bg[i] * (1 - a) + fg[i] * a) for i in range(3))

rows = []
cx = cy = S / 2
for y in range(S):
    row = bytearray([0])
    for x in range(S):
        u, v = x / S, y / S
        # rounded square mask (the macOS icon silhouette)
        r = 0.225
        qx, qy = abs(u - 0.5) - (0.5 - r), abs(v - 0.5) - (0.5 - r)
        inside = math.hypot(max(qx, 0), max(qy, 0)) - r
        a_mask = max(0.0, min(1.0, 0.5 - inside * S))
        if a_mask <= 0:
            row += bytes((0, 0, 0, 0)); continue
        # crimson background with a soft vignette
        d = math.hypot(u - 0.5, v - 0.42)
        col = (int(150 - 90 * d), int(22 - 12 * d), int(26 - 14 * d))
        # bronze ring
        ring = abs(math.hypot(u - 0.5, v - 0.5) - 0.37) * S
        a = max(0.0, min(1.0, 1.0 - (ring - 0.022 * S) / 2))
        col = over(col, (214, 160, 70), a)
        # lambda: two strokes meeting at the top, a short crossbar
        w = 0.052 * S
        ds = min(seg_dist(x, y, 0.5 * S, 0.24 * S, 0.30 * S, 0.76 * S), seg_dist(x, y, 0.5 * S, 0.24 * S, 0.70 * S, 0.76 * S))
        a = max(0.0, min(1.0, 1.0 - (ds - w) / 2))
        if a > 0:
            shade = 0.82 + 0.18 * (1 - v)
            col = over(col, tuple(min(255, int(c * shade)) for c in (232, 188, 96)), a)
        row += bytes(col) + bytes((int(255 * a_mask),))
    rows.append(bytes(row))

def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
