#!/usr/bin/env python3
"""Print the recompiled C++ of guest functions, with their PowerPC comments.

  tools/guest-funcs.py 825A5420 825A4E88 ... > funcs.txt

Searches the ported game's generated code (games/halo3/port by default) for
DEFINE_REX_FUNC(sub_<addr>) and prints each body, to read what an
unidentified function does without the original binary's symbols.
"""
import argparse
import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addresses", nargs="+", help="guest addresses, e.g. 825A5420 or 0x825A5420")
    ap.add_argument("--port", type=Path, default=ROOT / "games" / "halo3" / "port")
    args = ap.parse_args()
    wanted = {a.upper().removeprefix("0X").rjust(8, "0") for a in args.addresses}
    found = {}
    start = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
    manifest = tomllib.loads((args.port / "halo3_manifest.toml").read_text())
    generated = args.port / manifest["entrypoint"].get("out_directory_path", "generated")
    for cpp in sorted(generated.glob("*.cpp")):
        current = None
        for line in cpp.read_text(errors="replace").splitlines():
            m = start.match(line)
            if m:
                current = m.group(1) if m.group(1) in wanted else None
                if current:
                    found[current] = [f"// {cpp.name}", line]
                continue
            if current:
                found[current].append(line)
                if line == "}":
                    current = None
    for addr in sorted(wanted):
        if addr in found:
            print("\n".join(found[addr]))
            print()
        else:
            print(f"// sub_{addr}: not found\n")


if __name__ == "__main__":
    main()
