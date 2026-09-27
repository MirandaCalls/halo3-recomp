#!/usr/bin/env python3
"""Find code that no recompiled function covers and register it as function entries.

Functions only reached through pointers (vtables, pointer-to-member thunks,
callback tables) often have no .pdata entry, and ReXGlue's GapFill folds them
into a neighbouring segment where they are dropped as unreachable. Every such
function starts right after an unconditional terminator (blr / bctr / b).

For each module:
  1. decrypt the XEX image (xex_image.py)
  2. rebuild the set of emitted instruction addresses from the generated C++
     (one `// <insn>` comment per guest instruction, `loc_X:` labels anchor)
  3. every non-emitted address A in the code range where word(A-4) is an
     unconditional terminator and word(A) decodes as a plausible instruction
     becomes `0xA = {}` in config/<stem>_auto.toml
"""

import argparse
import re
import struct
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import capstone  # noqa: E402
import xex_image  # noqa: E402
from fix_unresolved import module_include  # noqa: E402

RE_FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+?)_?([0-9A-F]{8})?\)")
RE_FUNC_ADDR = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
RE_LABEL = re.compile(r"^loc_([0-9A-F]{8}):")
RE_INSN = re.compile(r"^\t// \S")
RE_CODE_BASE = re.compile(r"#define REX_CODE_BASE (0x[0-9A-Fa-f]+)")
RE_CODE_SIZE = re.compile(r"#define REX_CODE_SIZE (0x[0-9A-Fa-f]+)")

# Primary opcodes that plausibly start a function (loads/stores/arith/branches/mflr etc.).
START_OPCODES = {7, 10, 11, 12, 14, 15, 16, 18, 19, 20, 21, 24, 25, 28, 29, 31, 32, 33, 34, 35,
                 36, 37, 38, 40, 42, 44, 48, 50, 52, 54, 58, 59, 62, 63, 4}


def is_terminator(w):
    if w in (0x4E800020, 0x4E800420):  # blr, bctr
        return True
    return (w >> 26) == 18 and (w & 3) == 0  # b (no link, relative)


_md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
RE_REG = re.compile(r"\b(r\d+)\b")
NONVOLATILE = {f"r{i}" for i in range(14, 32)}


def looks_like_entry(img, off, addr, starts, max_insns=64):
    """Decode to the first terminator. A real entry never reads r14-r31 before
    writing them (switch-case bodies and frame-sharing chunks do), and is valid code."""
    written = set()
    n = 0
    for insn in _md.disasm(img[off:off + max_insns * 4], addr):
        n += 1
        regs = RE_REG.findall(insn.op_str)
        m = insn.mnemonic
        if m.startswith(("st", "cmp", "mt", "tw", "td", "dcb", "b")):
            reads, writes = set(regs), set()
        else:
            reads, writes = set(regs[1:]), set(regs[:1])
        if (reads & NONVOLATILE) - written:
            return False
        written |= writes
        w = struct.unpack(">I", img[off + (n - 1) * 4:off + n * 4])[0]
        if (w >> 26) == 18 and (w & 3) == 0:
            # Tail call must land on a known function start; switch-case
            # fragments branch back into the middle of their parent instead.
            disp = w & 0x03FFFFFC
            if disp & 0x02000000:
                disp -= 0x04000000
            return (insn.address + disp) in starts
        if is_terminator(w):
            return True
    return False  # hit undecodable data or ran too long without a terminator


def emitted_addresses(out_dir):
    covered = set()
    for cpp in sorted(out_dir.glob("*_recomp.*.cpp")):
        addr = None
        for line in cpp.read_text(errors="replace").splitlines():
            if line.startswith("DEFINE_REX_FUNC("):
                m = RE_FUNC_ADDR.match(line)
                addr = int(m.group(1), 16) if m else None
            elif m := RE_LABEL.match(line):
                addr = int(m.group(1), 16)
            elif addr is not None and RE_INSN.match(line):
                covered.add(addr)
                addr += 4
    return covered


def scan(xex_path, out_dir, project):
    pch = (out_dir / f"{project}_pch.h").read_text()
    code_base = int(RE_CODE_BASE.search(pch).group(1), 16)
    code_size = int(RE_CODE_SIZE.search(pch).group(1), 16)
    load, img = xex_image.load(xex_path)
    covered = emitted_addresses(out_dir)
    starts = set()
    for h in out_dir.glob(f"{project}_funcs.h"):
        starts |= {int(x, 16) for x in re.findall(r"DECLARE_REX_FUNC\(\w*?([0-9A-F]{8})\)", h.read_text())}

    def word(a):
        o = a - load
        return struct.unpack(">I", img[o:o + 4])[0]

    found = []
    for a in range(code_base + 4, code_base + code_size, 4):
        if a in covered:
            continue
        w = word(a)
        if w == 0 or (w >> 26) not in START_OPCODES:
            continue
        if not is_terminator(word(a - 4)):
            continue
        # Jump tables / scope tables: words that are pointers into this image.
        if any(load <= word(a + k) < load + len(img) for k in (0, 4)):
            continue
        if not looks_like_entry(img, a - load, a, starts):
            continue
        found.append(a)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    manifest = args.manifest.resolve()
    root = manifest.parent
    data = tomllib.loads(manifest.read_text())
    project = data["project"]["name"]

    total = 0
    for b in [data["entrypoint"]] + data.get("modules", []):
        stem = Path(b["file_path"]).stem
        found = scan(root / b["file_path"], root / b["out_directory_path"], project)
        print(f"{stem}: {len(found)} orphan entry candidates"
              + (f" e.g. {', '.join(f'0x{a:08X}' for a in found[:5])}" if found else ""))
        if args.dry_run or not found:
            continue
        inc = module_include(manifest, stem)
        text = inc.read_text() if inc.exists() else "# Auto-generated by autoport\n\n[functions]\n"
        new = [a for a in found if f"0x{a:08X} = " not in text]
        text += "".join(f"0x{a:08X} = {{}}  # orphan after terminator\n" for a in new)
        inc.write_text(text)
        total += len(new)
    print(f"total new: {total}")


if __name__ == "__main__":
    main()
