#!/usr/bin/env python3
"""Dump the structure of an original-Xbox XBE: header, certificate,
sections, kernel imports (by ordinal) and linked XDK library versions.

Usage: python3 -I xbe_info.py <default.xbe>
"""
import struct
import sys

ENTRY_KEYS = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}
SECTION_FLAGS = [
    (0x1, "writable"), (0x2, "preload"), (0x4, "executable"),
    (0x8, "inserted_file"), (0x10, "head_page_ro"), (0x20, "tail_page_ro"),
]


def main():
    data = open(sys.argv[1], "rb").read()
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    u16 = lambda o: struct.unpack_from("<H", data, o)[0]
    assert data[:4] == b"XBEH", "not an XBE"

    base = u32(0x104)
    soh, soi = u32(0x108), u32(0x10C)
    cert = u32(0x118) - base
    nsec = u32(0x11C)
    sec_tbl = u32(0x120) - base
    init_flags = u32(0x124)

    # Retail vs debug: pick whichever key puts entry point/thunk inside the image
    kind = None
    for k in ("retail", "debug"):
        e = u32(0x128) ^ ENTRY_KEYS[k]
        if base <= e < base + soi:
            kind = k
            break
    kind = kind or "retail"
    entry = u32(0x128) ^ ENTRY_KEYS[kind]
    thunk = (u32(0x158) ^ THUNK_KEYS[kind])
    nlib, lib_tbl = u32(0x160), u32(0x164) - base

    print(f"== XBE ({kind} keys) ==")
    print(f"base address     : {base:#010x}")
    print(f"image size       : {soi:#x} ({soi/1e6:.2f} MB), headers {soh:#x}")
    print(f"entry point      : {entry:#010x}")
    print(f"kernel thunk tbl : {thunk:#010x}")
    print(f"init flags       : {init_flags:#x}")

    title_id = u32(cert + 8)
    name = data[cert + 0xC:cert + 0xC + 80].decode("utf-16le").rstrip("\0")
    region = u32(cert + 0xA0)
    media = u32(cert + 0x9C)
    print(f"\n== Certificate ==")
    print(f"title            : {name!r}")
    print(f"title id         : {title_id:#010x}")
    print(f"region flags     : {region:#x}  (1=NA 2=JP 4=EU/RoW)")
    print(f"allowed media    : {media:#x}")

    print(f"\n== Sections ({nsec}) ==")
    print(f"{'name':<12}{'vaddr':>12}{'vsize':>10}{'file off':>10}{'fsize':>10}  flags")
    text_ranges = []
    for i in range(nsec):
        o = sec_tbl + i * 56
        flags, va, vs, ro, rs, nameaddr = struct.unpack_from("<IIIIII", data, o)
        no = nameaddr - base
        sname = data[no:data.index(b"\0", no)].decode("latin-1")
        fl = ",".join(n for b, n in SECTION_FLAGS if flags & b)
        print(f"{sname:<12}{va:#12x}{vs:#10x}{ro:#10x}{rs:#10x}  {fl}")
        if flags & 0x4:
            text_ranges.append((sname, va, vs))
    code_bytes = sum(vs for _, _, vs in text_ranges)
    print(f"executable sections: {[n for n,_,_ in text_ranges]} = {code_bytes/1e6:.2f} MB of code")

    # Kernel thunk table lives in a section; translate VA -> file offset
    def va_to_off(va):
        for i in range(nsec):
            o = sec_tbl + i * 56
            _, sva, svs, sro, srs, _ = struct.unpack_from("<IIIIII", data, o)
            if sva <= va < sva + svs:
                return sro + (va - sva)
        return None

    off = va_to_off(thunk)
    ords = []
    while off is not None:
        v = u32(off)
        if v == 0:
            break
        ords.append(v & 0x7FFFFFFF)
        off += 4
    print(f"\n== Kernel imports: {len(ords)} functions (by ordinal) ==")
    print(" ".join(str(x) for x in sorted(ords)))

    print(f"\n== Statically linked XDK libraries ({nlib}) ==")
    for i in range(nlib):
        o = lib_tbl + i * 16
        n = data[o:o + 8].rstrip(b"\0").decode("latin-1")
        major, minor, build, flags = struct.unpack_from("<HHHH", data, o + 8)
        print(f"{n:<10} {major}.{minor}.{build}  flags={flags:#06x}")


if __name__ == "__main__":
    main()
