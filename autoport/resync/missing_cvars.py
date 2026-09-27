#!/usr/bin/env python3
"""List cvars read via REXCVAR_GET in ReXGlue graphics/ui code that have no
REXCVAR_DEFINE_* anywhere, together with the Canary DEFINE_* block for each."""
import re, subprocess, sys
from pathlib import Path

REX = Path(sys.argv[1]); CANARY = Path(sys.argv[2])
src = [p for d in ("src", "include") for p in (REX / d).rglob("*") if p.suffix in (".cpp", ".h", ".inc")]
text = {p: p.read_text(errors="replace") for p in src}
used = {}
for p, t in text.items():
    for m in re.finditer(r"REXCVAR_GET\((\w+)\)", t):
        used.setdefault(m.group(1), set()).add(str(p.relative_to(REX)))
alltext = "\n".join(text.values())
defined = set(re.findall(r"REXCVAR_DEFINE_\w+\(\s*(\w+)", alltext))
canary = subprocess.run(["git", "-C", str(CANARY), "grep", "-h", "-A12", "-E", r"^DEFINE_\w+\($|^DEFINE_\w+\(\w",
                         "origin/canary_experimental", "--", "src"], capture_output=True, text=True).stdout
blocks = re.findall(r"(DEFINE_(\w+)\(\s*(\w+)\s*,\s*(.*?)\);)", canary, re.S)
cmap = {b[2]: b for b in blocks}
for name in sorted(set(used) - defined):
    b = cmap.get(name)
    print(f"== {name}  used in {sorted(used[name])[:3]}")
    print(b[0] if b else "   (no Canary definition)")
