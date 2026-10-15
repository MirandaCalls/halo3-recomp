#!/usr/bin/env python3
"""Extract every compiled Xenos shader from game data files.

Xbox 360 D3D compiled shaders are stored as containers whose first word is the
shader flags 0x102A11xx. The Shader record sits at container+shaderOffset and
its microcode at container + virtualSize + Shader.physicalOffset (Shader.size
bytes). Output is keyed by the same XXH3-64 hash Xenia's shader storage uses
(over the microcode bytes as stored in guest memory), so it lines up with
runtime-recorded shaders.
"""
import argparse
import json
import mmap
import re
import struct
import sys
from pathlib import Path

import xxhash

CONTAINER = re.compile(rb"\x10\x2A\x11[\x00-\xff]")


def scan(path):
    with open(path, "rb") as f:
        m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for mt in CONTAINER.finditer(m):
            off = mt.start()
            if off % 4 or off + 36 > len(m):
                continue
            flags, vsize, psize, _fc, cto, dto, so, _1c, _20 = struct.unpack(">9I", m[off:off + 36])
            if not (0 < vsize < 0x40000 and 0 < psize < 0x40000 and so + 8 <= vsize):
                continue
            phys_off, size = struct.unpack(">II", m[off + so:off + so + 8])
            start = off + vsize + phys_off
            if size == 0 or size % 4 or phys_off + size > psize or start + size > len(m):
                continue
            ucode = m[start:start + size]
            yield {"hash": xxhash.xxh3_64_intdigest(ucode), "flags": flags, "offset": off,
                   "container_size": vsize + psize, "ucode_size": size}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, required=True, help="JSON index to write")
    args = ap.parse_args()
    index = {}
    for p in args.files:
        n = 0
        for s in scan(p):
            e = index.setdefault(s["hash"], {"flags": s["flags"], "ucode_size": s["ucode_size"], "sources": []})
            if len(e["sources"]) < 4:
                e["sources"].append([p.name, s["offset"], s["container_size"]])
            n += 1
        print(f"{p.name}: {n} containers", file=sys.stderr)
    args.out.write_text(json.dumps({f"{h:016X}": v for h, v in index.items()}))
    print(f"{len(index)} unique shaders -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
