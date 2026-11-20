#!/usr/bin/env python3
"""montage.py out.png in1.ppm in2.ppm ...  -- tile frames (each scaled to 320x240) into one PNG, for STW_DUMPALL output."""
import struct, sys, zlib
def load(f):
    d = open(f, 'rb').read(); i = d.index(b'255\n') + 4; w, h = map(int, d[:i].split()[1:3]); return w, h, d[i:]
files = sys.argv[2:]; cols = 3; rows = (len(files) + cols - 1) // cols
W, H = cols * 320, rows * 240
img = bytearray(W * H * 3)
for k, f in enumerate(files):
    w, h, px = load(f); ox, oy = (k % cols) * 320, (k // cols) * 240
    for y in range(240):
        sy = y * h // 240
        row = bytearray()
        for x in range(320):
            o = (sy * w + x * w // 320) * 3; row += px[o:o + 3]
        img[((oy + y) * W + ox) * 3:((oy + y) * W + ox + 320) * 3] = row
def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
raw = b''.join(b'\x00' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
