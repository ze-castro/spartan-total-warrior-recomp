#!/usr/bin/env python3
"""Generate build/ktable.c: the Xbox kernel export table (name, calling convention, argument
bytes, data/function) for the ordinals default.xbe imports, from runtime/data/xboxkrnl.def (nxdk).
Handlers are looked up by name as weak symbols `k_<Name>` (runtime/kernel.c).

usage: gen_kernel.py <default.xbe> <out.c>
"""
import glob
import re
import struct
import sys
import importlib.util

spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)


def main():
    base, img, secs, entry, thunk = disc.load_xbe(sys.argv[1])
    ords, o = [], thunk - base
    while struct.unpack_from("<I", img, o)[0]:
        ords.append(struct.unpack_from("<I", img, o)[0] & 0x7FFFFFFF)
        o += 4
    exports = {}
    for line in open("runtime/data/xboxkrnl.def"):
        m = re.match(r"\s+(@?)(\w+?)(?:@(\d+))?\s+@\s+(\d+)\s+NONAME(.*)", line)
        if m:
            fast, name, nb, num, rest = m.groups()
            exports[int(num)] = (name, bool(fast), int(nb) if nb else -1, "DATA" in rest)
    impl = set()
    for fn in glob.glob("runtime/k*.c"):
        impl |= set(re.findall(r"^uint32_t k_(\w+)\(", open(fn).read(), re.M))
    out = ['#include "kernel.h"\n']
    for n in sorted(set(ords)):
        name, fast, nb, data = exports[n]
        if not data and name in impl:
            out.append(f"extern uint32_t k_{name}(CPU *, const uint32_t *);\n")
    out.append("const KExport KEXPORTS[] = {\n")
    for n in sorted(set(ords)):
        name, fast, nb, data = exports[n]
        h = f"&k_{name}" if name in impl and not data else "0"
        out.append(f'  {{{n}, "{name}", {int(fast)}, {nb}, {int(data)}, {h}}},\n')
    out.append("  {0, 0, 0, 0, 0, 0}\n};\n")
    open(sys.argv[2], "w").write("".join(out))
    print(f"{len(set(ords))} imports")


main()
