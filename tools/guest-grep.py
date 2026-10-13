#!/usr/bin/env python3
"""Search the recompiled game's code and report which guest functions match.

  tools/guest-grep.py 'ori r\\d+,r\\d+,(13824|8704)\\b'      # PowerPC comments are searched too

Prints "sub_<addr> (<file>): <count> match(es)" and the matching lines, so
code patterns (a PM4 packet header, a device field offset) can be traced to
the functions that contain them.
"""
import argparse
import re
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern", help="Python regular expression, matched against each line")
    ap.add_argument("--port", type=Path, default=ROOT / "games" / "halo3" / "port")
    ap.add_argument("--max-lines", type=int, default=3, help="matching lines shown per function")
    args = ap.parse_args()
    pattern = re.compile(args.pattern)
    start = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
    manifest = tomllib.loads((args.port / "halo3_manifest.toml").read_text())
    generated = args.port / manifest["entrypoint"].get("out_directory_path", "generated")
    hits = {}
    for cpp in sorted(generated.glob("*.cpp")):
        current = None
        for line in cpp.read_text(errors="replace").splitlines():
            m = start.match(line)
            if m:
                current = (m.group(1), cpp.name)
                continue
            if current and pattern.search(line):
                hits.setdefault(current, []).append(line.strip())
    for (addr, name), lines in sorted(hits.items()):
        print(f"sub_{addr} ({name}): {len(lines)} match(es)")
        for line in lines[:args.max_lines]:
            print(f"    {line}")
    print(f"{len(hits)} function(s)")


if __name__ == "__main__":
    main()
