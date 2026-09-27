#!/usr/bin/env python3
"""Insert autoport_spin_hint() after every recompiled `db16cyc` delay hint.

The recompiler emits db16cyc (PowerPC `or 28,28,28`, "delay 16 cycles") as a
bare comment, turning XDK wait loops into flat-out host spins. Idempotent.
Backed by autoport/runtime/spin_hint.cpp.
"""
import argparse
import re
import tomllib
from pathlib import Path

DECL = 'extern "C" void autoport_spin_hint();  // autoport/runtime/spin_hint.cpp\n'
CALL = "\tautoport_spin_hint();\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    gen = manifest.parent / tomllib.loads(manifest.read_text())["entrypoint"]["out_directory_path"]
    files = sites = 0
    for cpp in sorted(gen.glob("*_recomp.*.cpp")):
        text = cpp.read_text(errors="replace")
        if "// db16cyc" not in text:
            continue
        lines = text.splitlines(keepends=True)
        out, n = [], 0
        for i, line in enumerate(lines):
            out.append(line)
            if line.strip() == "// db16cyc" and (i + 1 >= len(lines) or lines[i + 1] != CALL):
                out.append(CALL)
                n += 1
        text = "".join(out)
        if DECL not in text:
            m = re.search(r"^#include [^\n]*\n", text, re.M)
            text = text[:m.end()] + DECL + text[m.end():]
        cpp.write_text(text)
        files += 1
        sites += n
    print(f"spin hints: {sites} new db16cyc sites patched in {files} files")


if __name__ == "__main__":
    main()
