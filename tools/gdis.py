#!/usr/bin/env python3
"""Disassemble guest code: dis.py <addr> [count]  (stops at the first ret after 8 instructions)."""
import importlib.util
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)
base, img, secs, entry, thunk = disc.load_xbe("game/default.xbe")
md = Cs(CS_ARCH_X86, CS_MODE_32)
a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
for k, i in enumerate(md.disasm(img[a - base:a - base + 0x400], a)):
    print(f"{i.address:#x}  {i.mnemonic} {i.op_str}")
    if k >= n or (i.mnemonic == "ret" and k > 8):
        break
