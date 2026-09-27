#!/usr/bin/env python3
"""Build a call graph (caller -> callees) from ReXGlue generated C++."""
import re, sys, json
from pathlib import Path

RE_DEF = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
RE_CALL = re.compile(r"^\s*(sub_[0-9A-F]{8}|__imp__\w+|rexcrt_\w+)\(ctx, base\);")
RE_BL = re.compile(r"^\s*// bl 0x([0-9a-f]+)")

def build(gen_dir):
    graph = {}
    cur = None
    for cpp in sorted(Path(gen_dir).glob("*_recomp.*.cpp")):
        for line in cpp.read_text(errors="replace").splitlines():
            if m := RE_DEF.match(line):
                cur = m.group(1); graph.setdefault(cur, [])
            elif cur and (m := RE_CALL.match(line)):
                graph[cur].append(m.group(1))
    return graph

if __name__ == "__main__":
    g = build(sys.argv[1])
    json.dump(g, open(sys.argv[2], "w"))
    print(len(g), "functions,", sum(map(len, g.values())), "call sites")
