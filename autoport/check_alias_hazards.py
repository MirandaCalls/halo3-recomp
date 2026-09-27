#!/usr/bin/env python3
"""Scan generated C++ for per-lane write/read aliasing hazards.

ReXGlue emits some vector instructions lane by lane:
    ctx.v0.u16[3] = f(ctx.v12.u32[3]);
    ctx.v0.u16[5] = f(ctx.v0.u32[1]);   // reads bytes already overwritten above
When the destination register is also a source, later lanes read clobbered
input (see rexglue-sdk#449). This finds every emitted instruction whose body
writes part of a register and later reads the same register, and groups the
hits by mnemonic so a whole class of translation bug shows up at once.
"""

import collections
import re
import sys
from pathlib import Path

RE_INSN = re.compile(r"^\t// (\S+)(.*)$")
RE_LANE_WRITE = re.compile(r"^\s*ctx\.(v\d+|f\d+|r\d+)\.(\w+)\[[^\]]+\]\s*=(.*);")
RE_READ = re.compile(r"ctx\.(v\d+|f\d+|r\d+)\.")


def scan(gen_dir):
    hits = collections.defaultdict(list)
    for cpp in sorted(Path(gen_dir).glob("*_recomp.*.cpp")):
        mnem = operands = None
        written = set()
        for line in cpp.read_text(errors="replace").splitlines():
            if m := RE_INSN.match(line):
                mnem, operands, written = m.group(1), m.group(2).strip(), set()
                continue
            if mnem is None:
                continue
            if m := RE_LANE_WRITE.match(line):
                dst, rhs = m.group(1), m.group(3)
                if dst in written and dst in RE_READ.findall(rhs):
                    hits[mnem].append(f"{cpp.name}: {mnem} {operands}")
                    mnem = None  # one report per instruction
                    continue
                written.add(dst)
            elif line.strip().startswith(("if", "goto", "return", "DEFINE_REX_FUNC", "}")):
                mnem = None
    return hits


if __name__ == "__main__":
    total = 0
    for d in sys.argv[1:]:
        for mnem, sites in sorted(scan(d).items(), key=lambda kv: -len(kv[1])):
            total += len(sites)
            print(f"{len(sites):6d}  {mnem:14s}  e.g. {sites[0]}")
    print(f"total hazardous sites: {total}")
