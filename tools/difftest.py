#!/usr/bin/env python3
"""Differential test: run each lifted function in Unicorn (reference x86
emulator) and in the compiled C harness on identical random inputs, then
compare final registers, XMM registers, the x87 stack and CRCs of heap,
stack and writable image memory.

Each Unicorn run goes through a small x86 stub:
    fninit; fldcw [0x027F]; fld qword [..] x n; call <function>; fstp qword [..] x 8
so the x87 state is identical and fully observable on both sides.

Usage: .venv/bin/python -I tools/difftest.py [cases_per_function] [max_functions] [seed]
  env ONLY=0xaddr,..  test only those functions;  VERBOSE=1  print details of mismatches
Requires build/lifted.c (tools/lift.py).
"""
import importlib.util
import json
import os
import random
import struct
import subprocess
import sys
import zlib

import unicorn
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL
from unicorn import x86_const as U

HEAP_BASE, HEAP_SIZE = 0x04000000, 0x4000
STACK_BASE, STACK_SIZE = 0x05000000, 0x2000
STUB_BASE = 0x0E000000
CW_ADDR, FPU_VALS, DUMP_ADDR = STUB_BASE + 0x800, STUB_BASE + 0x808, STUB_BASE + 0x900
STALE_ADDR, SCRATCH = STUB_BASE + 0x840, STUB_BASE + 0xA00
# Contents of the x87 physical registers before the test values are pushed. Identical on both
# sides so functions that read below the live stack (underflow) are still comparable.
STALE = [1.5, -2.25, 3.125, 100.0, -0.5, 7.0, 0.1, 42.0]
ESP0 = STACK_BASE + 0x1000            # stack pointer before the stub's `call`
REGS = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
        U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
RNAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
WORDS = 60                            # result words per case (see runtime/harness.c)

SPECIAL_FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 100.0, -100.0, 1e-3, 3.14159, 1e10, -1e10,
                  float("inf"), float("-inf"), 2147483648.0, -2147483648.0, 2147483520.0,
                  4294967296.0, 0.1, 255.0, 65536.0]
SPECIAL_BITS = [0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x00000001, 0x007FFFFF,
                0x80000001, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7F7FFFFF]
SPECIAL_D = [0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0x0000000000000001,
             0x7FF0000000000000, 0xFFF0000000000000, 0x8000000000000000]


def rand_float_bits(rng):
    k = rng.random()
    if k < 0.35:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-200.0, 200.0)))[0]
    if k < 0.60:
        return struct.unpack("<I", struct.pack("<f", rng.choice(SPECIAL_FLOATS)))[0]
    if k < 0.75:
        return rng.choice(SPECIAL_BITS)
    if k < 0.85:
        return struct.unpack("<I", struct.pack("<f", float(rng.randrange(-1000, 1000))))[0]
    return rng.getrandbits(32)


def rand_double_bits(rng):
    k = rng.random()
    if k < 0.45:
        return struct.unpack("<Q", struct.pack("<d", rng.uniform(-300.0, 300.0)))[0]
    if k < 0.75:
        return struct.unpack("<Q", struct.pack("<d", rng.choice(SPECIAL_FLOATS)))[0]
    if k < 0.85:
        return rng.choice(SPECIAL_D)
    if k < 0.95:
        return struct.unpack("<Q", struct.pack("<d", float(rng.randrange(-100000, 100000))))[0]
    return rng.getrandbits(64)


def quiet_d(b):
    """Real x86 turns a signalling NaN into a quiet one on fld; Unicorn keeps it signalling."""
    if (b >> 52) & 0x7FF == 0x7FF and b & ((1 << 52) - 1) and not b & (1 << 51):
        b |= 1 << 51
    return b


def rand_word(rng):
    k = rng.random()
    if k < 0.35:                                   # pointer into the heap, half 16-byte aligned
        step = 16 if rng.random() < 0.5 else 4
        return HEAP_BASE + 0x400 + rng.randrange(0, 0x2800, step)
    if k < 0.55:
        return rng.randrange(0, 256)
    if k < 0.60:
        return rng.randrange(0, 0x10000)
    if k < 0.85:
        return rand_float_bits(rng)
    return rng.getrandbits(32)


def build_stub(fa, nfpu):
    code = bytearray(b"\xdb\xe3")                                  # fninit
    code += b"\xd9\x2d" + struct.pack("<I", CW_ADDR)               # fldcw [CW_ADDR]
    for k in range(8):                                             # fill all 8 registers, then pop
        code += b"\xdd\x05" + struct.pack("<I", STALE_ADDR + 8 * k)
    for k in range(8):
        code += b"\xdd\x1d" + struct.pack("<I", SCRATCH + 8 * k)
    for k in range(nfpu):
        code += b"\xdd\x05" + struct.pack("<I", FPU_VALS + 8 * k)  # fld qword [..]
    call_at = STUB_BASE + len(code)
    code += b"\xe8" + struct.pack("<i", fa - (call_at + 5))        # call function
    ret_to = STUB_BASE + len(code)
    for k in range(8):
        code += b"\xdd\x1d" + struct.pack("<I", DUMP_ADDR + 8 * k)  # fstp qword [..]
    code += b"\x90"
    return bytes(code), ret_to, STUB_BASE + len(code) - 1


def make_case(rng):
    regs = [rand_word(rng) for _ in range(8)]
    regs[4] = ESP0 - 4                                # as seen inside the callee
    xmm = [[rand_float_bits(rng) for _ in range(4)] for _ in range(8)]
    nfpu = rng.choice([0, 0, 1, 2, 3])
    fvals = [quiet_d(rand_double_bits(rng)) for _ in range(4)]
    heap = bytearray(HEAP_SIZE)
    for i in range(0, HEAP_SIZE, 4):
        struct.pack_into("<I", heap, i, rand_word(rng))
    stack = bytearray(STACK_SIZE)
    for i in range(0, STACK_SIZE, 4):
        struct.pack_into("<I", stack, i, rand_word(rng))
    return regs, xmm, nfpu, fvals, bytes(heap), bytes(stack)


def main():
    per_fn = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    max_fn = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)

    spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
    disc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(disc)
    base, img, secs, entry, thunk = disc.load_xbe("game/default.xbe")
    img_size = (len(img) + 0xFFF) & ~0xFFF
    # Everything except code is reset before each case and checksummed afterwards. Real Xbox pages
    # such as .rdata are read-only, but the test environment lets code write anywhere, and a write
    # that leaks into the next case would corrupt constants for every later function.
    writable = sorted((va, va + vs) for n, (va, vs, fl) in secs.items()
                      if n != ".text" and not n.startswith(("err_", "$$", "tw_", "dsp_", ".XT")))

    meta = json.load(open("build/lifted.c.json"))
    funcs = meta["liftable"][:max_fn]
    transc = set(meta.get("transcendental", []))
    if os.environ.get("ONLY"):
        funcs = [int(x, 16) for x in os.environ["ONLY"].split(",")]

    def make_uc():
        u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(base, img_size, UC_PROT_ALL)
        u.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        u.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        u.mem_map(STUB_BASE, 0x1000, UC_PROT_ALL)
        u.mem_write(base, img)
        u.mem_write(CW_ADDR, struct.pack("<H", 0x027F))
        u.mem_write(STALE_ADDR, struct.pack("<8d", *STALE))
        return u

    uc = make_uc()

    def crc_wr():
        c = 0
        for lo, hi in writable:
            c = zlib.crc32(bytes(uc.mem_read(lo, hi - lo)), c)
        return c

    cases, expect, heaps = [], [], []
    skipped = {"fault": 0, "timeout": 0}
    for fa in funcs:
        for case_no in range(per_fn):
            # per-case seed: a failing case can be replayed alone with ONLY=<addr> and the same seed
            regs, xmm, nfpu, fvals, heap, stack = make_case(random.Random(f"{seed}/{fa:x}/{case_no}"))
            stub, ret_to, end = build_stub(fa, nfpu)
            for lo, hi in writable:
                uc.mem_write(lo, img[lo - base:hi - base])
            uc.mem_write(HEAP_BASE, heap)
            uc.mem_write(STACK_BASE, stack)
            uc.mem_write(STUB_BASE, stub)
            uc.mem_write(FPU_VALS, struct.pack("<4Q", *fvals))
            for r, v in zip(REGS, regs):
                uc.reg_write(r, v)
            uc.reg_write(U.UC_X86_REG_ESP, ESP0)          # the stub's call pushes the return address
            for i in range(8):
                uc.reg_write(U.UC_X86_REG_XMM0 + i, sum(w << (32 * k) for k, w in enumerate(xmm[i])))
            uc.reg_write(U.UC_X86_REG_EFLAGS, 2)
            try:
                uc.emu_start(STUB_BASE, ret_to, count=200000)
            except UcError as ex:
                skipped["fault"] += 1
                if ex.errno not in (unicorn.UC_ERR_READ_UNMAPPED, unicorn.UC_ERR_WRITE_UNMAPPED, unicorn.UC_ERR_FETCH_UNMAPPED):
                    uc = make_uc()              # exceptions (int3, #DE, bad insn) can wedge the emulator
                continue
            if uc.reg_read(U.UC_X86_REG_EIP) != ret_to:
                skipped["timeout"] += 1
                uc = make_uc()          # a runaway case can leave the emulator wedged
                continue
            out = [uc.reg_read(r) for r in REGS]
            out += [zlib.crc32(bytes(uc.mem_read(HEAP_BASE, HEAP_SIZE))),
                    zlib.crc32(bytes(uc.mem_read(STACK_BASE, STACK_SIZE))), crc_wr()]
            for i in range(8):
                v = uc.reg_read(U.UC_X86_REG_XMM0 + i)
                out += [(v >> (32 * k)) & 0xFFFFFFFF for k in range(4)]
            top = (uc.reg_read(U.UC_X86_REG_FPSW) >> 11) & 7
            try:
                uc.emu_start(ret_to, end, count=1000)           # dump the x87 stack
            except UcError:
                skipped["fault"] += 1
                continue
            out.append(top)
            for i in range(16):
                out.append(struct.unpack("<I", bytes(uc.mem_read(DUMP_ADDR + 4 * i, 4)))[0])
            assert len(out) == WORDS
            heaps.append(bytes(uc.mem_read(HEAP_BASE, HEAP_SIZE)))
            cases.append((fa, regs, xmm, nfpu, fvals, heap, stack))
            expect.append(out)

    n_funcs_covered = len({c[0] for c in cases})
    print(f"functions tried: {len(funcs)}  with >=1 usable case: {n_funcs_covered}  "
          f"cases: {len(cases)}  skipped: {skipped}")

    with open("build/image.bin", "wb") as f:
        f.write(img)
    with open("build/cases.bin", "wb") as f:
        f.write(struct.pack("<III", base, len(img), len(writable)))
        for lo, hi in writable:
            f.write(struct.pack("<II", lo, hi))
        f.write(struct.pack("<IIIII", HEAP_BASE, HEAP_SIZE, STACK_BASE, STACK_SIZE, len(cases)))
        for fa, regs, xmm, nfpu, fvals, heap, stack in cases:
            f.write(struct.pack("<I8I", fa, *regs))
            for i in range(8):
                f.write(struct.pack("<4I", *xmm[i]))
            f.write(struct.pack("<I4Q", nfpu, *fvals))
            f.write(heap)
            stack = bytearray(stack)                       # the return slot the stub's call writes
            struct.pack_into("<I", stack, ESP0 - 4 - STACK_BASE, build_stub(fa, nfpu)[1])
            f.write(bytes(stack))

    cc = ["clang", "-O2", "-w", "-ffp-contract=off", "-DQEMU_REF", "-I", "runtime",
          "-o", "build/harness", "runtime/harness.c", "build/lifted.c"]
    subprocess.run(cc, check=True)
    if os.environ.get("VERBOSE"):
        os.environ["DUMP_HEAP"] = "1"
    r = subprocess.run(["build/harness", "build/image.bin", "build/cases.bin", "build/results.bin"],
                       timeout=900)
    if r.returncode != 0:
        print("harness failed with", r.returncode)
        sys.exit(1)

    data = open("build/results.bin", "rb").read()
    got = [struct.unpack_from(f"<{WORDS}I", data, i * WORDS * 4) for i in range(len(cases))]
    XN = [f"xmm{i}.{k}" for i in range(8) for k in range(4)]
    NAMES = RNAMES + ["heap", "stack", "image"] + XN + ["fpu_top"] + [f"st{i}.{h}" for i in range(8) for h in "lh"]
    bad, known_af, unlifted = {}, set(), 0
    for idx, ((fa, regs, xmm, nfpu, fvals, _, _), want, have) in enumerate(zip(cases, expect, got)):
        if have[0] == 0xBAD1BAD1 and have[1] == 0xBAD2BAD2:      # reached unlifted code via call_ind
            unlifted += 1
            continue
        depth = (8 - want[43]) & 7
        n_cmp = 44 + 2 * depth                    # only live x87 registers are meaningful
        diff = [NAMES[k] for k in range(n_cmp) if want[k] != have[k]]
        for i in range(depth):                    # NaNs equal modulo the quiet bit (Unicorn keeps SNaNs)
            lo, hi = 44 + 2 * i, 45 + 2 * i
            if all(((x[hi] >> 20) & 0x7FF) == 0x7FF and (x[lo] or x[hi] & 0xFFFFF) for x in (want, have)) \
                    and want[lo] == have[lo] and (want[hi] | 0x80000) == (have[hi] | 0x80000):
                diff = [d for d in diff if d not in (NAMES[lo], NAMES[hi])]
        if not diff:
            continue
        if diff == ["eax"] and (want[0] ^ have[0]) & ~0x1000 == 0:
            known_af.add(fa)       # Unicorn leaves AF alone after fcomi; real x86 clears it
            continue
        bad.setdefault(fa, []).append(diff)
        if os.environ.get("VERBOSE") and len(bad[fa]) <= 2:
            print(f"--- {fa:#x} diff {diff}  (initial x87 depth {nfpu}, top want {want[43]} have {have[43]})")
            for n in diff:
                k = NAMES.index(n)
                print(f"  {n}: want {want[k]:#010x} have {have[k]:#010x}")
            if os.environ.get("OPERANDS"):                  # dump memory the pointer registers refer to
                hp = cases[idx][5]
                print("  init regs:", {n: hex(v) for n, v in zip(RNAMES, regs)})
                for rn, rv in zip(RNAMES, regs):
                    if HEAP_BASE <= rv < HEAP_BASE + HEAP_SIZE - 64:
                        o = rv - HEAP_BASE
                        print(f"  [{rn}] floats:", [f"{struct.unpack('<I', hp[o+4*j:o+4*j+4])[0]:#010x}" for j in range(8)])
                st = cases[idx][6]
                eo = regs[4] - STACK_BASE
                print("  stack args:", [f"{struct.unpack('<I', st[eo+4*j:eo+4*j+4])[0]:#010x}" for j in range(1, 4)])
                for j in range(1, 4):
                    av = struct.unpack('<I', st[eo+4*j:eo+4*j+4])[0]
                    if HEAP_BASE <= av < HEAP_BASE + HEAP_SIZE - 64:
                        o = av - HEAP_BASE
                        print(f"  [arg{j}={av:#x}] floats:", [f"{struct.unpack('<I', hp[o+4*q:o+4*q+4])[0]:#010x}" for q in range(4)])
            if "heap" in diff:
                hh = open("build/results.heap", "rb").read()[idx * HEAP_SIZE:(idx + 1) * HEAP_SIZE]
                for o in range(0, HEAP_SIZE, 4):
                    if hh[o:o + 4] != heaps[idx][o:o + 4]:
                        print(f"  heap+{o:#x}: want {heaps[idx][o:o+4][::-1].hex()} have {hh[o:o+4][::-1].hex()}")

    if unlifted:
        print(f"{unlifted} cases skipped: reached unlifted code through an indirect call")
    if known_af:
        print(f"known Unicorn deviation (AF after fcomi), excluded: {sorted(hex(a) for a in known_af)}")
    print(f"mismatching functions: {len(bad)} / {n_funcs_covered}   "
          f"matching: {n_funcs_covered - len(bad)}")
    for fa in sorted(bad)[:30]:
        tag = "  [uses fsin/fcos/fpatan/fyl2x]" if fa in transc else ""
        print(f"  {fa:#x}: {len(bad[fa])}/{per_fn} cases differ in "
              f"{sorted(set(sum(bad[fa], [])))[:8]}{tag}")
    json.dump({hex(k): v for k, v in bad.items()}, open("build/mismatches.json", "w"))


if __name__ == "__main__":
    main()
