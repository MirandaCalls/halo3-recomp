#!/usr/bin/env python3
"""Recover jump tables the code generator sized too small.

A recompiled `bctr` jump table becomes `switch (ctx.rN.u32) { case ... default:
__builtin_trap(); // Switch case out of range }`. When the table has no bounds
check in front of it (e.g. Halo: Reach indexes a state table directly), the
generator can under-detect its size - Reach got 1 case for a 10-entry table,
and campaign loading hit the trap and spun forever.

For every such switch this reads the real table from the executable: the base
comes from the `lis`/`addi` pair feeding the `lwzx` before the `bctr`, and
entries are taken while they point inside the containing function. If the
real table is longer than the generated switch, a [[switch_tables]] override
is written to config/<stem>_switch.toml (added to the manifest's includes).
Re-run codegen afterwards (fix_unresolved.py does).

Usage: fix_switch_tables.py <manifest.toml> [--dry-run]
"""
import argparse
import bisect
import re
import struct
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xex_image  # noqa: E402
from fix_unresolved import module_include  # noqa: E402

RE_DEF = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
RE_LOC = re.compile(r"^loc_([0-9A-F]{8}):")
RE_INSN = re.compile(r"^\t// (\S+)(.*)$")
RE_SWITCH = re.compile(r"^\tswitch \(ctx\.r(\d+)\.u32\) \{")
RE_CASE = re.compile(r"^\tcase (\d+):")
MAX_ENTRIES = 4096


def scan(gen_dir):
    """Yield (function_start, bctr_address, index_register, table_base, case_count)."""
    for cpp in sorted(gen_dir.glob("*_recomp.*.cpp")):
        func = addr = None
        history = []  # recent (address, mnemonic, operands)
        pending = None  # switch being read: [func, bctr, reg, base, cases]
        for line in cpp.read_text(errors="replace").splitlines():
            if m := RE_DEF.match(line):
                func = addr = int(m.group(1), 16)
                history = []
                continue
            if m := RE_LOC.match(line):
                addr = int(m.group(1), 16)
                continue
            if func is None:
                continue
            if m := RE_INSN.match(line):
                history.append((addr, m.group(1), m.group(2).strip()))
                history = history[-12:]
                addr += 4
                continue
            if m := RE_SWITCH.match(line):
                bctr = next((a for a, op, _ in reversed(history) if op == "bctr"), None)
                base = table_base(history)
                if bctr is not None and base is not None:
                    pending = [func, bctr, int(m.group(1)), base, 0]
                continue
            if pending and RE_CASE.match(line):
                pending[4] += 1
                continue
            if pending and "Switch case out of range" in line:
                yield tuple(pending)
                pending = None


def table_base(history):
    """Table address from `lis rT,hi` + `addi rT,rT,lo` before the lwzx."""
    lis = {}
    base = None
    for _, op, args in history:
        parts = [p.strip() for p in args.split(",")]
        if op == "lis" and len(parts) == 2:
            lis[parts[0]] = int(parts[1]) & 0xFFFF
        elif op == "addi" and len(parts) == 3 and parts[1] in lis:
            base = ((lis[parts[1]] << 16) + int(parts[2])) & 0xFFFFFFFF
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    data = tomllib.loads(manifest.read_text())
    entry = data["entrypoint"]
    gen = manifest.parent / entry["out_directory_path"]
    image_base, image = xex_image.load(manifest.parent / entry["file_path"])

    starts = sorted(int(m.group(1), 16) for cpp in gen.glob("*_recomp.*.cpp")
                    for m in RE_DEF.finditer(cpp.read_text(errors="replace")) if True)
    starts = sorted(set(starts))

    overrides = []
    for func, bctr, reg, base, cases in scan(gen):
        i = bisect.bisect_right(starts, func)
        func_end = starts[i] if i < len(starts) else func + 0x10000
        labels = []
        off = base - image_base
        while len(labels) < MAX_ENTRIES and 0 <= off <= len(image) - 4:
            target = struct.unpack(">I", image[off:off + 4])[0]
            if not (func <= target < func_end) or target & 3:
                break
            labels.append(target)
            off += 4
        if len(labels) > cases:
            overrides.append((bctr, reg, labels, cases))

    for bctr, reg, labels, cases in overrides:
        print(f"switch at 0x{bctr:08X} (r{reg}): generated {cases} case(s), table has {len(labels)}")
    if not overrides or args.dry_run:
        print(f"{len(overrides)} under-sized jump tables")
        return
    path = module_include(manifest, Path(entry["file_path"]).stem, "switch")
    existing = path.read_text() if path.exists() else ""
    out = [existing.rstrip() + "\n" if existing.strip() else "# Auto-generated by autoport/fix_switch_tables.py\n"]
    for bctr, reg, labels, _ in overrides:
        if f"address = 0x{bctr:08X}" in existing:
            continue
        out.append(f"\n[[switch_tables]]\naddress = 0x{bctr:08X}\nregister = {reg}\n"
                   f"labels = [{', '.join(f'0x{t:08X}' for t in labels)}]\n")
    path.write_text("".join(out))
    print(f"{len(overrides)} under-sized jump tables -> {path}")


if __name__ == "__main__":
    main()
