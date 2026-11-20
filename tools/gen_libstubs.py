#!/usr/bin/env python3
"""Generate build/libstubs.c: for every call target the lifter left to the runtime that lies in a
static XDK library section (D3D, DSOUND, XGRPH, XPP, XMV, D3DX), the section name and the number
of stack bytes its first `ret` pops (so a stand-in can return with the right stack cleanup).

Names come from analysis/libsyms.json (tools/oovpa_match.py) and runtime/data/libnames.txt (manual
overrides). Handlers are `uint32_t lib_<Name>(CPU *, const uint32_t *args)` in runtime/*.c; args points
at the stack arguments (ecx/edx are in the CPU for fastcall/thiscall functions).

usage: gen_libstubs.py <default.xbe> <lifted.c.json> <out.c>
"""
import json
import os
import re
import sys
import importlib.util
import capstone

spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)


def first_ret_pop(md, img, base, t):
    """Stack bytes popped by the first `ret` on the straight-line path from t. Unconditional direct jumps are
    followed: many DSOUND methods are thunks (`jmp` into a shared body), and the first ret that lies after a
    jmp in address order belongs to a different function."""
    for _ in range(8):
        target = None
        for i in md.disasm(img[t - base:t - base + 4096], t):
            if i.mnemonic == "ret":
                return i.operands[0].imm if i.operands else 0
            if i.mnemonic == "jmp":
                if i.operands[0].type == capstone.x86.X86_OP_IMM:
                    target = i.operands[0].imm
                break
        if target is None:
            break
        t = target
    return 0


def main():
    base, img, secs, entry, thunk = disc.load_xbe(sys.argv[1])
    meta = json.load(open(sys.argv[2]))
    lifted = set(meta["liftable"])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    syms = json.load(open("analysis/libsyms.json")) if os.path.exists("analysis/libsyms.json") else {}
    manual, pops = {}, {}
    for line in open("runtime/data/libnames.txt"):
        line = line.split("#")[0].split()
        if len(line) in (2, 3):
            manual[int(line[0], 16)] = line[1]
            if len(line) == 3:
                pops[int(line[0], 16)] = int(line[2])
    impl = set()
    import glob
    for fn in glob.glob("runtime/*.c") + glob.glob("runtime/*.m") + glob.glob("runtime/*.mm"):
        impl |= set(re.findall(r"^uint32_t lib_(\w+)\(", open(fn).read(), re.M))
    rows = []
    # every overridden address needs an entry, not only direct-call targets: vtable and callback
    # dispatch reaches them through call_ind
    over = set(manual)
    for a, c in syms.items():
        names = {x["name"] for x in c}
        if len(names) == 1 and next(iter(names)) in impl:
            over.add(int(a, 16))
    for t in sorted(set(meta["ext_targets"]) | over):
        if t in lifted:
            continue
        sec = next((n for n, (va, vs, fl) in secs.items() if va <= t < va + vs), None)
        if sec in (None, ".text"):
            continue
        pop = first_ret_pop(md, img, base, t)
        pop = pops.get(t, pop)
        cands = [x["name"] for x in syms.get(hex(t), [])]
        name = manual.get(t) or "|".join(sorted(set(cands))) or "unknown_%x" % t
        handler = next((n for n in ([name] if t in manual else name.split("|")) if n in impl), None)
        rows.append((t, sec, pop, name, handler))
    with open(sys.argv[3], "w") as f:
        f.write('#include "lib.h"\n')
        for h in sorted({r[4] for r in rows if r[4]}):
            f.write(f"extern uint32_t lib_{h}(CPU *, const uint32_t *);\n")
        f.write("const LibStub LIBSTUBS[] = {\n")
        for t, sec, pop, name, h in rows:
            f.write(f'  {{0x{t:x}u, "{sec}", {pop}, "{name}", {"&lib_" + h if h else "0"}}},\n')
        f.write("  {0, 0, 0, 0, 0}\n};\n")
    print(f"{len(rows)} library entry points")


main()
