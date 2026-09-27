#!/usr/bin/env python3
"""Locate statically linked XAPI functions that the SDK replaces natively (`[rexcrt]`).

Currently: the XAPI fiber module. Guest fibers switch stacks in hand-written
assembly, which static recompilation cannot express, so these must be mapped to
the SDK's host-fiber implementations. Every rule below keys on how the function
uses the current fiber pointer at KTHREAD+0x164 (KTHREAD = [r13+0x100]) and the
0xA50-byte X_FIBER_CONTEXT allocation, which are stable across XDK builds.

Writes `[rexcrt]` entries into the entrypoint's config/<stem>_auto.toml.
"""

import argparse
import bisect
import re
import sys
import tomllib
from pathlib import Path

import capstone

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xex_image  # noqa: E402
from fix_unresolved import module_include  # noqa: E402

RE_CODE_BASE = re.compile(r"#define REX_CODE_BASE (0x[0-9A-Fa-f]+)")
RE_CODE_SIZE = re.compile(r"#define REX_CODE_SIZE (0x[0-9A-Fa-f]+)")
# Address table in <project>_init.cpp keeps every start, including ones already
# renamed to rexcrt_* by a previous pass (keeps this pass idempotent).
RE_START = re.compile(r"\{ 0x([0-9A-F]{8}), \w+ \}")


def load_function_bodies(xex, out_dir, project):
    pch = (out_dir / f"{project}_pch.h").read_text()
    lo = int(RE_CODE_BASE.search(pch).group(1), 16)
    hi = lo + int(RE_CODE_SIZE.search(pch).group(1), 16)
    starts = sorted({int(x, 16) for x in RE_START.findall((out_dir / f"{project}_init.cpp").read_text())})
    load, img = xex_image.load(xex)
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.skipdata = True
    funcs = {}
    for insn in md.disasm(img[lo - load:hi - load], lo):
        k = bisect.bisect_right(starts, insn.address) - 1
        if k >= 0:
            funcs.setdefault(starts[k], []).append(f"{insn.mnemonic} {insn.op_str}".strip())
    return funcs


def has(body, pattern):
    return any(re.fullmatch(pattern, i) for i in body)


def compares_current_fiber_with_arg(body):
    """DeleteFiber checks `lpFiber == current fiber` (self-delete). The argument
    is compared in r3 directly or through a copy (`mr r31, r3`, Halo 3)."""
    arg_regs = {"r3"} | {m.group(1) for i in body if (m := re.fullmatch(r"mr (r\d+), r3", i))}
    return any((m := re.fullmatch(r"cmplw cr\d, r\d+, (r\d+)", i)) and m.group(1) in arg_regs
               for i in body)


def detect_fibers(funcs):
    cands = {}
    FIBER_READ = r"lwz r\d+, 0x164\(r\d+\)"
    FIBER_WRITE = r"stw r\d+, 0x164\(r\d+\)"
    ALLOC_CTX = r"li r3, 0xa50"
    for addr, body in funcs.items():
        head = body[:6]
        # SwitchToFiber body: save non-volatiles into the current fiber context.
        if (len(head) >= 4 and re.fullmatch(r"lwz r4, 0x100\(r13\)", head[0])
                and re.fullmatch(r"lwz r5, 0x164\(r4\)", head[1])
                and head[2] == "mfcr r6" and head[3] == "mflr r7"):
            cands.setdefault("SwitchToFiber_body", []).append(addr)
        elif has(body, ALLOC_CTX) and has(body, FIBER_WRITE):
            cands.setdefault("ConvertThreadToFiber", []).append(addr)
        elif has(body, ALLOC_CTX) and not has(body, FIBER_READ):
            cands.setdefault("CreateFiber", []).append(addr)
        elif has(body, FIBER_READ) and has(body, FIBER_WRITE) and not has(body, ALLOC_CTX):
            cands.setdefault("ConvertFiberToThread", []).append(addr)
        elif has(body, FIBER_READ) and compares_current_fiber_with_arg(body):
            cands.setdefault("DeleteFiber", []).append(addr)
    # fiber.obj links as one contiguous block: anchor on the fiber-context allocators
    # and pick each candidate nearest to them.
    anchors = cands.get("CreateFiber", []) + cands.get("ConvertThreadToFiber", [])
    if not anchors:
        return {}
    anchor = min(anchors)
    found = {name: min(addrs, key=lambda a: abs(a - anchor)) for name, addrs in cands.items()}
    # Callers enter SwitchToFiber through a one-instruction `b body` stub when present.
    body = found.pop("SwitchToFiber_body", None)
    if body is not None:
        stub = next((a for a, b in funcs.items() if b[:1] == [f"b 0x{body:x}"]), None)
        found["SwitchToFiber"] = stub or body
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    root = manifest.parent
    data = tomllib.loads(manifest.read_text())
    project = data["project"]["name"]
    ep = data["entrypoint"]

    funcs = load_function_bodies(root / ep["file_path"], root / ep["out_directory_path"], project)
    found = detect_fibers(funcs)
    for name, addr in sorted(found.items(), key=lambda kv: kv[1]):
        print(f"  {name:22s} 0x{addr:08X}")
    if not found:
        return

    # Own include file: appending a table to *_auto.toml would capture later
    # [functions] entries appended by other passes.
    inc = module_include(manifest, Path(ep["file_path"]).stem, suffix="xapi")
    inc.parent.mkdir(exist_ok=True)
    inc.write_text("# Auto-generated by autoport/detect_xapi.py\n\n[rexcrt]\n"
                   + "".join(f"{n} = 0x{a:08X}\n" for n, a in sorted(found.items())))


if __name__ == "__main__":
    main()
