#!/usr/bin/env python3
"""Print the recompiled C++ of guest functions, with their PowerPC comments.

  tools/guest-funcs.py 825A5420 825A4E88 ... > funcs.txt
  tools/guest-funcs.py --containing 82469FC8 ...   # the function a caller address is in

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
    ap.add_argument("--containing", action="store_true",
                    help="treat addresses as code addresses (e.g. callers from tools/d3d-trace.sh) "
                         "and print the function each one is in")
    args = ap.parse_args()
    wanted = {a.upper().removeprefix("0X").rjust(8, "0") for a in args.addresses}
    found = {}
    start = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", re.M)
    manifest = tomllib.loads((args.port / "halo3_manifest.toml").read_text())
    generated = args.port / manifest["entrypoint"].get("out_directory_path", "generated")
    if args.containing:
        # A function's code runs from its start to the next function's start.
        starts = sorted(int(m.group(1), 16) for cpp in generated.glob("*.cpp")
                        for m in start.finditer(cpp.read_text(errors="replace")))
        import bisect
        resolved = set()
        for a in wanted:
            i = bisect.bisect_right(starts, int(a, 16)) - 1
            if i >= 0:
                print(f"// {a} is in sub_{starts[i]:08X}")
                resolved.add(f"{starts[i]:08X}")
        print()
        wanted = resolved
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
