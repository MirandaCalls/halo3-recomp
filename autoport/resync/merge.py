#!/usr/bin/env python3
"""Three-way merge Xenia Canary GPU changes into ReXGlue.

For each entry of manifest.json (produced by upstream's map_bases.py, removed here):
  ours   = ReXGlue file
  base   = Canary file at the per-file base revision, translated to ReXGlue dialect
  theirs = Canary file at HEAD, translated the same way
and run `git merge-file`. Conflicts inside the preamble (before the first
`namespace rex`) are resolved to "ours" (ReXGlue includes / cvar definitions);
other conflicts are kept with markers for manual resolution.

Output: merged files written in place into --rex (a git checkout, so `git diff`
shows the result), plus a report.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REF = "origin/canary_experimental"


def git(repo, *args, check=False):
    r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, errors="replace")
    if check and r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout


def translate(text, sub_namespace):
    t = text
    # Namespaces.
    ns = "rex::graphics" + (f"::{sub_namespace}" if sub_namespace else "")
    t = re.sub(r"namespace xe \{\s*namespace gpu \{\s*namespace (\w+) \{", lambda m: f"namespace rex::graphics::{m.group(1)} {{", t)
    t = re.sub(r"namespace xe \{\s*namespace gpu \{", "namespace rex::graphics {", t)
    t = re.sub(r"namespace xe \{\s*namespace ui \{\s*namespace (\w+) \{", lambda m: f"namespace rex::ui::{m.group(1)} {{", t)
    t = re.sub(r"namespace xe \{\s*namespace ui \{", "namespace rex::ui {", t)
    t = re.sub(r"\}\s*// namespace \w+\s*\}\s*// namespace gpu\s*\}\s*// namespace xe", f"}}  // namespace {ns}", t)
    t = re.sub(r"\}\s*// namespace gpu\s*\}\s*// namespace xe", "}  // namespace rex::graphics", t)
    t = re.sub(r"\}\s*// namespace \w+\s*\}\s*// namespace ui\s*\}\s*// namespace xe", "}  // namespace rex::ui", t)
    t = re.sub(r"namespace xe::gpu", "namespace rex::graphics", t)
    t = re.sub(r"\bxe::gpu::", "rex::graphics::", t)
    t = re.sub(r"\bxe::", "rex::", t)
    t = re.sub(r"\bgpu::", "graphics::", t)
    # Logging.
    for a, b in [("XELOGE", "REXGPU_ERROR"), ("XELOGW", "REXGPU_WARN"), ("XELOGI", "REXGPU_INFO"),
                 ("XELOGD", "REXGPU_DEBUG"), ("XELOGGPU", "REXGPU_DEBUG"), ("XELOGV", "REXGPU_TRACE")]:
        t = re.sub(rf"\b{a}\b", b, t)
    # Cvars.
    t = re.sub(r"\bcvars::(\w+)", r"REXCVAR_GET(\1)", t)
    return t


def clang_format(texts, style_dir, image="xenon-build"):
    """Format several texts in one container call. Returns list of formatted texts."""
    tmp = Path(style_dir) / "cf_tmp"
    tmp.mkdir(exist_ok=True)
    names = []
    for i, t in enumerate(texts):
        p = tmp / f"f{i}.cpp"
        p.write_text(t)
        names.append(p.name)
    subprocess.run(["podman", "run", "--rm", "--userns=keep-id", "-v", f"{style_dir}:{style_dir}:Z",
                    "-w", str(tmp), image, "bash", "-c",
                    f"for f in {' '.join(names)}; do clang-format -i --style=file:../.clang-format $f; done"],
                   check=True)
    return [(tmp / n).read_text() for n in names]


def resolve_preamble(merged):
    """Resolve conflict blocks that appear before the first namespace to 'ours'."""
    first_ns = merged.find("\nnamespace rex")
    out, pos = [], 0
    pat = re.compile(r"<<<<<<< ours\n(.*?)\|\|\|\|\|\|\| base\n.*?=======\n.*?>>>>>>> theirs\n", re.S)
    for m in pat.finditer(merged):
        if first_ns != -1 and m.start() < first_ns:
            out.append(merged[pos:m.start()]); out.append(m.group(1)); pos = m.end()
    out.append(merged[pos:])
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rex", type=Path, required=True)
    ap.add_argument("--canary", type=Path, required=True)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True, help="scratch dir holding .clang-format")
    ap.add_argument("--only", default="", help="comma-separated substrings of rex paths to process")
    args = ap.parse_args()

    manifest = json.loads(args.manifest.read_text())
    only = [s for s in args.only.split(",") if s]
    report = []
    for e in manifest:
        if not e.get("canary") or e.get("base") in (None, REF):
            continue
        if only and not any(s in e["rex"] for s in only):
            continue
        rex_path = args.rex / e["rex"]
        sub_ns = "vulkan" if "/vulkan/" in e["rex"] else ("d3d12" if "/d3d12/" in e["rex"] else "")
        base_txt = git(args.canary, "show", f"{e['base']}:{e['canary']}")
        head_txt = git(args.canary, "show", f"{REF}:{e['canary']}")
        if base_txt == head_txt:
            report.append((e["rex"], "unchanged upstream", 0))
            continue
        base_f, head_f = clang_format([translate(base_txt, sub_ns), translate(head_txt, sub_ns)], args.work)
        bp = args.work / "base.tmp"; hp = args.work / "head.tmp"
        bp.write_text(base_f); hp.write_text(head_f)
        r = subprocess.run(["git", "merge-file", "-p", "--diff3", "-L", "ours", "-L", "base", "-L", "theirs",
                            str(rex_path), str(bp), str(hp)], capture_output=True, text=True, errors="replace")
        merged = resolve_preamble(r.stdout)
        conflicts = merged.count("<<<<<<< ours")
        rex_path.write_text(merged)
        report.append((e["rex"], f"merged from {e['base_date']}", conflicts))
        print(f"{conflicts:4d} conflicts  {e['rex']}  (base {e['base_date']}, sim {e['base_sim']:.2f})", flush=True)
    total = sum(c for _, _, c in report)
    print(f"\n{len(report)} files processed, {sum(1 for r in report if r[2])} with conflicts, {total} conflict blocks")


if __name__ == "__main__":
    main()
