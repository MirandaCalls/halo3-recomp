#!/usr/bin/env python3
"""GPU path triage: can this title get a native (D3D-hook) renderer, or does it stay on Xenos emulation?

Native renderers (UnleashedRecomp-style) replace the title's statically linked
Xbox D3D9 functions. That only works when D3D calls are real, hookable
functions. Titles linked against D3D9LTCG have D3D inlined into engine code at
link time, so API-level hooking is not viable without per-engine renderer work.

Reports:
  - static libraries + XDK build (from the XEX header)
  - D3D variant (D3D9 vs D3D9LTCG)
  - D3D core location (functions calling Vd* kernel GPU exports)
  - engine functions that build PM4 packet headers inline
  - call sites from engine code into the D3D core
"""

import argparse
import collections
import json
import re
import struct
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from callgraph import build as build_callgraph  # noqa: E402

PM4_OPS = {0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x2D: "SET_CONSTANT", 0x2F: "LOAD_ALU_CONSTANT",
           0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD",
           0x59: "EVENT_WRITE_EXT", 0x3C: "WAIT_REG_MEM", 0x21: "REG_RMW", 0x64: "XE_SWAP",
           0x3D: "MEM_WRITE", 0x48: "ME_INIT", 0x10: "NOP", 0x3F: "COND_WRITE",
           0x44: "INVALIDATE_STATE", 0x55: "SET_SHADER_CONSTANTS", 0x3B: "INDIRECT_BUFFER"}
RE_DEF = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
RE_LIS = re.compile(r"\s*// lis (r\d+),(-?\d+)")
RE_LO = re.compile(r"\s*// (ori|addi) (r\d+),(r\d+),(-?\d+)")


def static_libs(xex_path):
    d = Path(xex_path).read_bytes()
    nopt = struct.unpack(">I", d[20:24])[0]
    opts = dict(struct.unpack(">II", d[24 + i * 8:32 + i * 8]) for i in range(nopt))
    off = opts.get(0x200FF)
    if not off:
        return {}
    size = struct.unpack(">I", d[off:off + 4])[0]
    libs = {}
    for i in range((size - 4) // 16):
        e = d[off + 4 + i * 16:off + 4 + (i + 1) * 16]
        maj, mnr, bld, qfe = struct.unpack(">HHHH", e[8:16])
        libs[e[:8].rstrip(b"\0").decode()] = f"{maj}.{mnr}.{bld}.{qfe & 0xFF}"
    return libs


def pm4_writers(gen_dir):
    per = collections.defaultdict(collections.Counter)
    for cpp in Path(gen_dir).glob("*_recomp.*.cpp"):
        cur, lis = None, {}
        for line in cpp.read_text(errors="replace").splitlines():
            if m := RE_DEF.match(line):
                cur, lis = int(m.group(1), 16), {}
            elif m := RE_LIS.match(line):
                lis[m.group(1)] = int(m.group(2)) & 0xFFFF
            elif (m := RE_LO.match(line)) and cur is not None and m.group(3) in lis:
                hi, lo = lis[m.group(3)], int(m.group(4))
                val = ((hi << 16) | (lo & 0xFFFF)) if m.group(1) == "ori" else ((hi << 16) + lo) & 0xFFFFFFFF
                op = (val >> 8) & 0x7F
                if val >> 30 == 3 and val & 0xFF == 0 and op in PM4_OPS and (val >> 16) & 0x3FFF < 0x400:
                    per[cur][PM4_OPS[op]] += 1
    return per


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    data = tomllib.loads(manifest.read_text())
    ep = data["entrypoint"]
    xex = manifest.parent / ep["file_path"]
    gen = manifest.parent / ep["out_directory_path"]

    libs = static_libs(xex)
    d3d_lib = next((k for k in libs if k.startswith("D3D9")), None)
    graph = build_callgraph(gen)
    addr = lambda n: int(n[4:], 16) if n.startswith("sub_") else None  # noqa: E731

    vd_callers = sorted(a for f, cs in graph.items()
                        if (a := addr(f)) and any(c.startswith("__imp__Vd") for c in cs))
    # D3D core = densest 128 KB window of Vd* callers.
    core = None
    if vd_callers:
        best = max(vd_callers, key=lambda s: sum(s <= a < s + 0x20000 for a in vd_callers))
        core = (best & ~0xFFFF, (best & ~0xFFFF) + 0x20000)
    # Grow the window over the contiguous run of D3D-looking functions (Vd* callers
    # and PM4 packet writers) with no gap larger than 32 KB; the D3D library is
    # linked as one contiguous block but is often larger than 128 KB.
    pm4 = pm4_writers(gen)
    if core:
        marks = sorted(set(vd_callers) | set(pm4))
        lo, hi = core
        grown = True
        while grown:
            grown = False
            for a in marks:
                if lo - 0x8000 <= a < lo:
                    lo, grown = a & ~0xF, True
                elif hi <= a < hi + 0x8000:
                    hi, grown = (a + 0x1000) & ~0xFFF, True
        core = (lo, hi)
    in_core = lambda a: core and a is not None and core[0] <= a < core[1]  # noqa: E731

    entry_calls = collections.Counter()
    for f, cs in graph.items():
        if in_core(addr(f)):
            continue
        entry_calls.update(c for c in cs if in_core(addr(c)))
    inline_pm4 = {a: dict(v) for a, v in pm4.items() if not in_core(a)}

    ltcg = bool(d3d_lib and "LTCG" in d3d_lib)
    report = {
        "xdk": libs.get("XAPILIB") or libs.get("XBOXKRNL"),
        "d3d_library": f"{d3d_lib} {libs.get(d3d_lib, '')}".strip() if d3d_lib else None,
        "d3d_core": [f"0x{core[0]:08X}", f"0x{core[1]:08X}"] if core else None,
        "d3d_core_entry_points_used": len(entry_calls),
        "engine_call_sites_into_d3d_core": sum(entry_calls.values()),
        "engine_functions_emitting_pm4_inline": len(inline_pm4),
        "native_renderer": ("not viable via D3D hooks (D3D9LTCG inlines D3D into engine code); "
                            "stay on Xenos emulation or write a per-engine renderer")
                           if ltcg else "candidate for D3D-hook renderer (UnleashedRecomp-style)",
        "static_libraries": libs,
        "d3d_core_entry_points": {f"0x{addr(c):08X}": n for c, n in entry_calls.most_common()},
        "inline_pm4_functions": {f"0x{a:08X}": v for a, v in sorted(inline_pm4.items())},
        "vd_callers": [f"0x{a:08X}" for a in vd_callers],
    }
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ("static_libraries", "d3d_core_entry_points", "inline_pm4_functions", "vd_callers")},
                     indent=2))
    if args.json:
        args.json.write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
