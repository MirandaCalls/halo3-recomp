#!/usr/bin/env python3
"""Per-entry-point behaviour table for a title's statically linked Xbox D3D library.

Input: the manifest and the triage JSON (autoport/gpu/triage.py --json), which
gives the D3D core address range. For every D3D core function that engine
code calls, collects - over the function and everything it reaches inside the
core - the kernel imports used, PM4 packet types emitted and how many engine
call sites use it. These features are what identify D3D API functions
(Present -> VdSwap, CreateDevice -> VdInitializeEngines, draws -> DRAW_INDX...)
without symbols.
"""
import argparse
import collections
import json
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from callgraph import build as build_callgraph  # noqa: E402
from triage import pm4_writers  # noqa: E402


def addr(name):
    return int(name[4:], 16) if name.startswith("sub_") else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    ap.add_argument("triage_json", type=Path)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    gen = manifest.parent / tomllib.loads(manifest.read_text())["entrypoint"]["out_directory_path"]
    tri = json.loads(args.triage_json.read_text())
    lo, hi = (int(x, 16) for x in tri["d3d_core"])
    in_core = lambda n: (a := addr(n)) is not None and lo <= a < hi  # noqa: E731

    graph = build_callgraph(gen)
    pm4 = pm4_writers(gen)
    engine_calls = collections.Counter()
    for f, cs in graph.items():
        if not in_core(f):
            engine_calls.update(c for c in cs if in_core(c))

    def reach(root):
        seen, stack = {root}, [root]
        while stack:
            for c in graph.get(stack.pop(), []):
                if in_core(c) and c not in seen:
                    seen.add(c)
                    stack.append(c)
        return seen

    rows = []
    for f, n in engine_calls.most_common():
        closure = reach(f)
        imports = collections.Counter(c[len("__imp__"):] for g in closure for c in graph.get(g, [])
                                      if c.startswith("__imp__"))
        ops = collections.Counter()
        for g in closure:
            ops.update(pm4.get(addr(g), {}))
        rows.append({"func": f, "engine_call_sites": n, "own_callees": len(graph.get(f, [])),
                     "closure_size": len(closure), "imports": dict(imports), "pm4": dict(ops)})
    for r in rows:
        imp = ",".join(sorted(r["imports"]))[:70]
        ops = ",".join(f"{k}x{v}" for k, v in sorted(r["pm4"].items()))[:60]
        print(f"{r['func']} calls={r['engine_call_sites']:4d} closure={r['closure_size']:3d}  {imp:70s} {ops}")
    if args.json:
        args.json.write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
