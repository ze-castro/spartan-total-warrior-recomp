#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png  -- convert a frame dumped with STW_DUMPFRAME."""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
i = d.index(b'255\n') + 4
w, h = map(int, d[:i].split()[1:3])
raw = d[i:]
rows = b''.join(b'\x00' + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
