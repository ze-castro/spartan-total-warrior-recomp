#!/usr/bin/env python3
"""Minimal XDVDFS (original Xbox) extractor.

Usage: python3 -I xiso_extract.py <image.iso> <empty-output-dir> [--list]
Handles redump-style full images (game partition at 0x18300000) and
plain XISOs (partition at 0).
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0x18300000, 0x0, 0xFD90000, 0x2080000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("No XDVDFS volume descriptor found")


def walk(f, base, dir_sector, dir_size, rel=""):
    f.seek(base + dir_sector * SECTOR)
    data = f.read(dir_size)

    def visit(off):
        if off * 4 + 14 > len(data):
            return
        o = off * 4
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", data, o)
        if left == 0xFFFF and right == 0xFFFF and sector == 0xFFFFFFFF:
            return
        name = data[o + 14:o + 14 + nlen].decode("latin-1")
        if left:
            yield from visit(left)
        path = rel + name
        if attr & 0x10:
            yield (path + "/", None, None)
            if size:
                yield from walk(f, base, sector, size, path + "/")
        else:
            yield (path, sector, size)
        if right:
            yield from visit(right)

    # An empty directory starts with 0xFFFF padding
    if len(data) >= 2 and data[:2] == b"\xff\xff":
        return
    yield from visit(0)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    iso, out = sys.argv[1], sys.argv[2]
    list_only = "--list" in sys.argv
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = list(walk(f, base, root_sector, root_size))
        total = sum(s for _, _, s in entries if s)
        print(f"partition @ {hex(base)}, {len(entries)} entries, {total/1e6:.1f} MB of files")
        if list_only:
            for p, sec, sz in entries:
                print(f"{sz if sz is not None else '<dir>':>12}  {p}")
            return
        if os.path.exists(out) and os.listdir(out):
            raise SystemExit(f"{out} is not empty; refusing to extract")
        os.makedirs(out, exist_ok=True)
        root = os.path.realpath(out)
        for p, sec, sz in entries:
            dest = os.path.realpath(os.path.join(out, p.replace("\\", "/")))
            if not dest.startswith(root + os.sep) and dest != root:
                raise SystemExit(f"unsafe path in image: {p}")
            if sec is None:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(base + sec * SECTOR)
            with open(dest, "wb") as o:
                left = sz
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    if not chunk:
                        raise SystemExit(f"short read in {p}")
                    o.write(chunk)
                    left -= len(chunk)
        print("done")


if __name__ == "__main__":
    main()
