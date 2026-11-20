#!/usr/bin/env python3
"""Name the static XDK library functions in default.xbe by matching Cxbx-Reloaded's XbSymbolDatabase
OOVPA signatures (tools/oovpa/, ODbL-1.0) against the library sections. For every function name the
newest signature at or below the XDK build (5849) is used. XREF entries are ignored; only the plain
{offset, byte} pairs are matched.

usage: oovpa_match.py <default.xbe> <out.json> [xdk_build]
"""
import glob
import json
import os
import re
import sys
import importlib.util

spec = importlib.util.spec_from_file_location("disc", "tools/discover.py")
disc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(disc)

# database dir -> sections its functions can live in
LIBS = {"D3D8": ["D3D", "D3DX"], "DSound": ["DSOUND"], "XGraphic": ["XGRPH"], "Xapi": ["XPP", ".text"]}
HDR = re.compile(r"(OOVPA_[A-Z_]+)\(\s*(\w+)\s*,\s*(\d+)")
BYTE = re.compile(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}")
OVM = re.compile(r"OV_MATCH\(\s*(0x[0-9A-Fa-f]+)\s*((?:,\s*0x[0-9A-Fa-f]+\s*)+)\)")


def parse_file(path):
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    heads = [m for m in HDR.finditer(text) if m.group(1) not in ("OOVPA_SIG_MATCH", "OOVPA_SIG_MATCH_DUMMY")]
    out = []
    for i, m in enumerate(heads):
        body = text[m.end():heads[i + 1].start() if i + 1 < len(heads) else len(text)]
        pairs = {}
        for off, val in BYTE.findall(body):
            pairs[int(off, 16)] = int(val, 16)
        for off, vals in OVM.findall(body):
            for k, v in enumerate(re.findall(r"0x[0-9A-Fa-f]+", vals)):
                pairs[int(off, 16) + k] = int(v, 16)
        out.append((m.group(2), int(m.group(3)), pairs))
    return out


def main():
    xbe, outp = sys.argv[1], sys.argv[2]
    build = int(sys.argv[3]) if len(sys.argv) > 3 else 5849
    base, img, secs, entry, thunk = disc.load_xbe(xbe)
    result, ambiguous = {}, {}
    for lib, sections in LIBS.items():
        best = {}                                   # name -> (version, pairs)
        for f in glob.glob(f"tools/oovpa/{lib}/*.inl"):
            for name, ver, pairs in parse_file(f):
                if ver <= build and pairs and (name not in best or (ver, len(pairs)) > (best[name][0], len(best[name][1]))):
                    best[name] = (ver, pairs)
        for sec in sections:
            if sec not in secs:
                continue
            va, vs, _ = secs[sec]
            lo, hi = va - base, va - base + vs
            for name, (ver, pairs) in best.items():
                items = sorted(pairs.items())
                span = items[-1][0]
                for o in range(lo, hi - span):
                    if all(img[o + k] == v for k, v in items):
                        result.setdefault(hex(base + o), []).append({"name": name, "lib": lib, "version": ver})
    json.dump(result, open(outp, "w"), indent=1, sort_keys=True)
    named = len(result)
    multi = sum(1 for v in result.values() if len(v) > 1)
    print(f"{named} addresses named, {multi} with more than one candidate")


main()
