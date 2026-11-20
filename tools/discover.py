#!/usr/bin/env python3
"""Function discovery for an original-Xbox XBE (32-bit x86), v2.

Every candidate function is trial-disassembled first and only committed if it
decodes cleanly (no junk/privileged instructions, no overlap with already
accepted code). Seeds, in priority order:
  1. entry point, then transitive direct calls / tail jumps
  2. code pointers stored in data (vtables first, then isolated pointers)
  3. first non-padding byte after each remaining gap in .text
`switch` jump tables (jmp [reg*4+table], optionally via a byte index table)
are resolved and their ranges marked as data.

Usage: .venv/bin/python -I tools/discover.py game/default.xbe analysis
"""
import collections
import json
import os
import struct
import sys

import capstone
from capstone import x86_const as X

THUNK_KEY = 0x5B6D40B6
ENTRY_KEY = 0xA8FC57AB

# Instructions that essentially never appear in 32-bit flat-mode game code;
# hitting one means we are decoding data or the middle of something else.
JUNK = {
    "in", "out", "cli", "sti", "int", "into", "hlt", "insb", "insw", "insd",
    "outsb", "outsw", "outsd", "iret", "iretd", "retf", "arpl", "bound", "les",
    "lds", "daa", "das", "aaa", "aas", "aam", "aad", "salc", "lgdt", "lidt",
    "wbinvd", "rdmsr", "wrmsr", "clts", "invd", "sysenter", "sysexit", "lcall",
    "ljmp", "ud2", "xlatb", "lgs", "lfs", "lss", "popal", "popaw", "pushal",
    "pushaw", "ins", "outs", "enter",
}
SEG_JUNK_OPS = {"cs", "ds", "es", "ss"}
PADDING = {0xCC, 0x90, 0x00}
FAM = {}
for _l in "abcd":
    for _n in (f"{_l}l", f"{_l}h", f"{_l}x", f"e{_l}x"):
        FAM[_n] = _l
for _n in ("si", "esi"):
    FAM[_n] = "si"
for _n in ("di", "edi"):
    FAM[_n] = "di"
for _n in ("bp", "ebp"):
    FAM[_n] = "bp"


def load_xbe(path):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    base, soi, nsec = u32(0x104), u32(0x10C), u32(0x11C)
    sec_tbl = u32(0x120) - base
    entry = u32(0x128) ^ ENTRY_KEY
    thunk = u32(0x158) ^ THUNK_KEY
    img = bytearray(soi)
    secs = {}
    for i in range(nsec):
        flags, va, vs, ro, rs, na = struct.unpack_from("<IIIIII", d, sec_tbl + i * 56)
        name = d[na - base:d.index(b"\0", na - base)].decode()
        img[va - base:va - base + rs] = d[ro:ro + rs]
        secs[name] = (va, vs, flags)
    return base, bytes(img), secs, entry, thunk


def main():
    xbe, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    base, img, secs, entry, thunk = load_xbe(xbe)
    tva, tvs, _ = secs[".text"]
    tend = tva + tvs
    imgend = base + len(img)

    def rd32(va):
        if base <= va and va + 4 <= imgend:
            return struct.unpack_from("<I", img, va - base)[0]
        return None

    def rd8(va):
        return img[va - base] if base <= va < imgend else None

    in_text = lambda a: tva <= a < tend

    thunks = {}
    a = thunk
    while rd32(a):
        thunks[a] = rd32(a) & 0x7FFFFFFF
        a += 4
    lib_ranges = {n: (v[0], v[0] + v[1]) for n, v in secs.items()
                  if n in ("XMV", "D3D", "D3DX", "DSOUND", "XGRPH", "XPP", "DOLBY")}

    def lib_of(a):
        for n, (lo, hi) in lib_ranges.items():
            if lo <= a < hi:
                return n

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # cover[i]: 0 unknown, 1 instruction start, 2 instruction interior, 3 data
    cover = bytearray(tvs)
    owner = {}
    funcs = {}
    insn_len = {}
    mnem = collections.Counter()
    lib_calls = collections.Counter()
    kcalls = collections.Counter()
    indirect = collections.Counter()
    indirect_sites = []
    jump_tables = []          # dicts
    rejected = collections.defaultdict(list)   # reason -> [addr]
    reject_seen = {}
    callq = collections.deque()

    def fam_of(reg):
        return FAM.get(md.reg_name(reg), md.reg_name(reg))

    def resolve_switch(ins, hist, start):
        """Return (table_va, entries, byte_table_va|None) or None."""
        o = ins.operands[0]
        table = o.mem.disp & 0xFFFFFFFF
        idxfam = fam_of(o.mem.index)
        bound = None
        for h in reversed(hist):
            if h.mnemonic == "cmp" and len(h.operands) == 2 and \
                    h.operands[1].type == X.X86_OP_IMM and \
                    h.operands[0].type in (X.X86_OP_REG, X.X86_OP_MEM):
                bound = (h.operands[1].imm & 0xFFFFFFFF) + 1
                break
        byte_tbl = None
        for h in reversed(hist):
            _, written = h.regs_access()
            if idxfam in {fam_of(r) for r in written}:
                if h.mnemonic in ("movzx", "mov") and len(h.operands) == 2 and \
                        h.operands[1].type == X.X86_OP_MEM and h.operands[1].size == 1 and \
                        base <= (h.operands[1].mem.disp & 0xFFFFFFFF) < imgend:
                    byte_tbl = h.operands[1].mem.disp & 0xFFFFFFFF
                break
        if bound is not None and not (1 <= bound <= 1024):
            return None
        if byte_tbl is not None:
            if bound is None:
                return None
            vals = [rd8(byte_tbl + i) for i in range(bound)]
            if None in vals:
                return None
            count = max(vals) + 1
        else:
            count = bound if bound is not None else 256
        entries = []
        for i in range(count):
            v = rd32(table + 4 * i)
            if v is None or not in_text(v):
                break
            if bound is None and abs(v - start) > 0x10000:
                break
            entries.append(v)
        if not entries or (bound is not None and byte_tbl is None and len(entries) < bound):
            # bound said N entries but the table ended early: still use what we have
            if not entries:
                return None
        return table, entries, byte_tbl

    def trial(start):
        """Disassemble everything reachable from start. Returns result dict
        or ('reject', reason)."""
        visited = {}
        used = set()
        calls = []
        lib_l = collections.Counter()
        k_l = collections.Counter()
        ind_l = []
        jts = []
        work = [start]
        while work:
            a = work.pop()
            hist = collections.deque(maxlen=8)
            while True:
                if not in_text(a):
                    break
                if a in visited:
                    break
                c = cover[a - tva]
                if c == 2 or c == 3 or (a in used):
                    return ("reject", "overlap")
                if c == 1:
                    break                      # runs into already accepted code
                ins = next(md.disasm(img[a - base:a - base + 15], a, 1), None)
                if ins is None:
                    return ("reject", "decode")
                m = ins.mnemonic
                if m in JUNK:
                    return ("reject", "junk:" + m)
                if ins.bytes[:2] == b"\x00\x00":
                    return ("reject", "junk:zeros")
                if m in ("push", "pop") and ins.op_str in SEG_JUNK_OPS:
                    return ("reject", "junk:segpush")
                if m == "int3" and a == start:
                    return ("reject", "starts-with-int3")
                visited[a] = (ins.size, m)
                for b in range(a, a + ins.size):
                    used.add(b)
                hist.append(ins)
                nxt = a + ins.size
                ops = ins.operands
                if m == "call":
                    o = ops[0]
                    if o.type == X.X86_OP_IMM:
                        t = o.imm & 0xFFFFFFFF
                        if in_text(t):
                            calls.append(t)
                        elif lib_of(t):
                            lib_l[lib_of(t)] += 1
                    elif o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0:
                        t = o.mem.disp & 0xFFFFFFFF
                        if t in thunks:
                            k_l[thunks[t]] += 1
                        else:
                            ind_l.append((a, "call [abs]"))
                    elif o.type == X.X86_OP_MEM:
                        ind_l.append((a, "call [reg+disp] (vtable/fnptr)"))
                    else:
                        ind_l.append((a, "call reg"))
                    a = nxt
                elif m == "jmp":
                    o = ops[0]
                    if o.type == X.X86_OP_IMM:
                        t = o.imm & 0xFFFFFFFF
                        if in_text(t):
                            if t < start or cover[t - tva] == 1:
                                calls.append(t)           # tail call
                            elif t not in visited:
                                work.append(t)
                        elif lib_of(t):
                            lib_l[lib_of(t)] += 1
                    elif o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 \
                            and (o.mem.disp & 0xFFFFFFFF) in thunks:
                        k_l[thunks[o.mem.disp & 0xFFFFFFFF]] += 1
                    elif o.type == X.X86_OP_MEM and o.mem.index != 0 and o.mem.scale == 4 \
                            and o.mem.base == 0:
                        r = resolve_switch(ins, hist, start)
                        if r is None:
                            ind_l.append((a, "jmp [reg*4+tbl] UNRESOLVED"))
                        else:
                            table, entries, btbl = r
                            jts.append({"jmp": a, "table": table, "entries": len(entries),
                                        "byte_table": btbl})
                            for e in entries:
                                if e not in visited:
                                    work.append(e)
                    elif o.type == X.X86_OP_MEM and o.mem.index == 0:
                        ind_l.append((a, "jmp [reg+disp] (vtable tail call)"))
                    elif o.type == X.X86_OP_MEM:
                        ind_l.append((a, "jmp [other mem]"))
                    else:
                        ind_l.append((a, "jmp reg"))
                    break
                elif m in ("ret", "int3"):
                    break
                elif ins.group(X.X86_GRP_JUMP):
                    o = ops[0]
                    if o.type == X.X86_OP_IMM:
                        t = o.imm & 0xFFFFFFFF
                        if in_text(t) and t not in visited:
                            work.append(t)
                    a = nxt
                else:
                    a = nxt
        return {"visited": visited, "calls": calls, "lib": lib_l, "k": k_l,
                "ind": ind_l, "jts": jts}

    def commit(start, r, why):
        end = start
        for a, (size, m) in r["visited"].items():
            cover[a - tva] = 1
            for b in range(a + 1, a + size):
                cover[b - tva] = 2
            owner[a] = start
            insn_len[a] = size
            mnem[m] += 1
            end = max(end, a + size)
        funcs[start] = {"why": why, "end": end, "insns": len(r["visited"])}
        lib_calls.update(r["lib"])
        kcalls.update(r["k"])
        for site, kind in r["ind"]:
            indirect[kind] += 1
            indirect_sites.append((site, kind))
        for jt in r["jts"]:
            jump_tables.append(jt)
            n = jt["entries"]
            if in_text(jt["table"]):
                for b in range(jt["table"], min(tend, jt["table"] + 4 * n)):
                    if cover[b - tva] == 0:
                        cover[b - tva] = 3
            if jt["byte_table"] and in_text(jt["byte_table"]):
                for b in range(jt["byte_table"], min(tend, jt["byte_table"] + 256)):
                    if cover[b - tva] == 0:
                        cover[b - tva] = 3
        for t in r["calls"]:
            callq.append(t)

    def try_seed(addr, why):
        if not in_text(addr) or addr in funcs or addr in reject_seen:
            return False
        c = cover[addr - tva]
        if c != 0:
            return False
        r = trial(addr)
        if isinstance(r, tuple):
            reject_seen[addr] = r[1]
            rejected[r[1].split(":")[0] + (":" + r[1].split(":")[1] if ":" in r[1] else "")].append(addr)
            return False
        commit(addr, r, why)
        return True

    def drain():
        n = 0
        while callq:
            t = callq.popleft()
            if try_seed(t, "call"):
                n += 1
        return n

    # --- tier 1: entry point and everything reachable by direct calls
    assert try_seed(entry, "entry")
    drain()
    t1 = len(funcs)

    # --- tier 2: code pointers in data
    jt_ranges = [(jt["table"], jt["table"] + 4 * jt["entries"]) for jt in jump_tables]
    in_jt = lambda o: any(lo <= o < hi for lo, hi in jt_ranges)
    scan_secs = [(n, v) for n, v in secs.items()
                 if n != ".text" and n not in lib_ranges
                 and not n.startswith(("err_", "$$", "tw_", "dsp_", ".XT"))]
    seeds_by_run = {}
    for n, (va, vs, fl) in scan_secs:
        run = []
        for o in list(range(va, va + vs - 3, 4)) + [None]:
            v = rd32(o) if o is not None else None
            if v is not None and in_text(v) and not in_jt(o):
                run.append(v)
            else:
                for x in run:
                    seeds_by_run[x] = max(seeds_by_run.get(x, 0), len(run))
                run = []
    ordered = sorted(seeds_by_run, key=lambda x: (-min(seeds_by_run[x], 3), x))
    acc2 = rej2 = 0
    for v in ordered:
        if try_seed(v, "data-ptr-vtable" if seeds_by_run[v] >= 3 else "data-ptr"):
            acc2 += 1
        drain()
    t2 = len(funcs)

    # --- tier 3: first non-padding byte after each gap
    def gaps():
        out, i = [], 0
        while i < tvs:
            if cover[i] == 0:
                j = i
                while j < tvs and cover[j] == 0:
                    j += 1
                out.append((tva + i, tva + j))
                i = j
            else:
                i += 1
        return out

    def sweep_gaps():
        added = 0
        for lo, hi in gaps():
            p = lo
            while p < hi:
                while p < hi and rd8(p) in PADDING:
                    p += 1
                if p >= hi:
                    break
                if cover[p - tva] != 0:
                    while p < hi and cover[p - tva] != 0:
                        p += 1
                    continue
                if try_seed(p, "gap-start"):
                    added += 1 + drain()
                else:
                    # junk or data: resume after the next run of 0xCC padding
                    q = p + 1
                    while q < hi and not (rd8(q - 1) == 0xCC and rd8(q) != 0xCC):
                        q += 1
                    p = q
        return added

    for _ in range(20):
        if not sweep_gaps():
            break
    t3 = len(funcs)

    # --- statistics
    covered = sum(insn_len.values())
    data_bytes = sum(1 for c in cover if c == 3)
    gap_list = gaps()
    pad_bytes = unexplained = 0
    big = []
    for lo, hi in gap_list:
        seg = img[lo - base:hi - base]
        if all(b in PADDING for b in seg):
            pad_bytes += hi - lo
        else:
            unexplained += hi - lo
            big.append((hi - lo, lo))
    big.sort(reverse=True)

    cat = collections.Counter()
    for m, n in mnem.items():
        if m.startswith("f") and m not in ("fs",):
            cat["x87 FPU"] += n
        elif m.endswith(("ps", "ss", "sd", "pd")) or m.startswith(("cvt", "comis", "ucomis", "movd", "movq", "unpck", "shuf")):
            cat["SSE"] += n
        elif m.startswith("p") and m not in ("push", "pop", "pushfd", "popfd", "pushal", "popal"):
            cat["MMX/SSE-int"] += n
        elif m in ("rdtsc", "cpuid"):
            cat["special"] += n
        else:
            cat["integer/control"] += n

    reasons = {k: len(v) for k, v in sorted(rejected.items(), key=lambda kv: -len(kv[1]))}
    summary = {
        "text_bytes": tvs,
        "instruction_bytes": covered,
        "jump_table_data_bytes": data_bytes,
        "padding_bytes": pad_bytes,
        "unexplained_bytes": unexplained,
        "instruction_coverage_pct": round(100 * covered / tvs, 1),
        "explained_pct": round(100 * (covered + data_bytes + pad_bytes) / tvs, 1),
        "functions_total": len(funcs),
        "tier1_entry_and_calls": t1,
        "tier2_data_pointers_added": t2 - t1,
        "tier3_gap_starts_added": t3 - t2,
        "by_origin": dict(collections.Counter(f["why"] for f in funcs.values())),
        "rejected_seeds_by_reason": reasons,
        "rejected_total": len(reject_seen),
        "jump_tables_resolved": len(jump_tables),
        "jump_table_entries": sum(j["entries"] for j in jump_tables),
        "unresolved_or_indirect_sites": dict(indirect),
        "kernel_call_sites": sum(kcalls.values()),
        "kernel_imports_called": len(kcalls),
        "lib_call_sites": dict(lib_calls),
        "instruction_categories": dict(cat),
    }
    print(json.dumps(summary, indent=2))
    print("\nLargest unexplained gaps (size, addr):",
          [(s, hex(a)) for s, a in big[:12]])
    print("Special/privileged still present:",
          {m: n for m, n in mnem.items() if m in JUNK or m in ("rdtsc", "cpuid")})

    json.dump({
        "base": base, "entry": entry, "text": [tva, tvs],
        "functions": {hex(k): v for k, v in sorted(funcs.items())},
        "jump_tables": [{**j, "jmp": hex(j["jmp"]), "table": hex(j["table"]),
                         "byte_table": hex(j["byte_table"]) if j["byte_table"] else None}
                        for j in jump_tables],
        "indirect_sites": [(hex(a), k) for a, k in indirect_sites],
        "kernel_calls_by_ordinal": dict(kcalls),
        "rejected": {hex(a): r for a, r in sorted(reject_seen.items())},
        "unexplained_gaps": [(hex(a), s) for s, a in big[:200]],
        "summary": summary,
    }, open(os.path.join(outdir, "functions.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
