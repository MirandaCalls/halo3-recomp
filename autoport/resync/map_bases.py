#!/usr/bin/env python3
"""Map ReXGlue graphics files to Xenia Canary files and find each file's merge base.

For every ReXGlue source file under src/graphics and include/rex/graphics:
  1. find the Canary file (src/xenia/gpu, src/xenia/ui/vulkan) with the most
     similar normalized token stream at Canary HEAD;
  2. walk that Canary file's history (since --since) and pick the revision most
     similar to the ReXGlue copy - the point it was (last) synced from.
Writes a JSON manifest used by merge.py.
"""

import argparse
import difflib
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, "/tmp/claude-1000/-home-aaron-xenon-pc-comp/3f09f79c-6a95-4657-89f1-02ef030f7db0/scratchpad")
from normdiff_lib import toks_from_text  # noqa: E402

CANARY_DIRS = ["src/xenia/gpu", "src/xenia/ui/vulkan", "src/xenia/ui"]


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          errors="replace").stdout


def ratio(a, b):
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    return sm.ratio()


def quick(a, b):
    # Cheap set-based prefilter.
    sa, sb = set(a), set(b)
    return len(sa & sb) / max(1, len(sa | sb))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rex", type=Path, required=True)
    ap.add_argument("--canary", type=Path, required=True)
    ap.add_argument("--since", default="2025-06-01")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    ref = "origin/canary_experimental"
    canary_files = [f for f in git(args.canary, "ls-tree", "-r", "--name-only", ref).splitlines()
                    if f.startswith(tuple(CANARY_DIRS)) and f.endswith((".cc", ".h", ".inc"))
                    and "/testing/" not in f]
    heads = {}
    for f in canary_files:
        heads[f] = toks_from_text(git(args.canary, "show", f"{ref}:{f}"))

    rex_files = sorted(p for d in ("src/graphics", "include/rex/graphics")
                       for p in (args.rex / d).rglob("*") if p.suffix in (".cpp", ".h", ".inc")
                       and "shaders/bytecode" not in str(p))
    manifest = []
    for rf in rex_files:
        rt = toks_from_text(rf.read_text(errors="replace"))
        if len(rt) < 40:
            continue
        stem = rf.stem.replace("_", "")
        cands = sorted(canary_files, key=lambda f: -quick(rt, heads[f]))[:6]
        # Prefer same kind (header vs source).
        want_h = rf.suffix == ".h"
        cands = [c for c in cands if c.endswith(".h") == want_h] or cands
        best = max(cands, key=lambda f: ratio(rt, heads[f]))
        head_sim = ratio(rt, heads[best])
        if head_sim < 0.35:
            manifest.append({"rex": str(rf.relative_to(args.rex)), "canary": None, "sim": head_sim})
            print(f"  no match  {rf.relative_to(args.rex)} (best {best} {head_sim:.2f})")
            continue
        # Candidate base revisions: every commit touching the file since --since, plus HEAD.
        revs = git(args.canary, "log", "--format=%H %ad", "--date=short", f"--since={args.since}",
                   ref, "--", best).split("\n")
        revs = [r.split() for r in revs if r.strip()]
        base, base_sim, base_date = ref, head_sim, "HEAD"
        for sha, date in revs:
            txt = git(args.canary, "show", f"{sha}:{best}")
            if not txt:
                continue
            s = ratio(rt, toks_from_text(txt))
            if s > base_sim + 1e-4:
                base, base_sim, base_date = sha, s, date
        manifest.append({"rex": str(rf.relative_to(args.rex)), "canary": best, "head_sim": head_sim,
                         "base": base, "base_sim": base_sim, "base_date": base_date,
                         "revisions_since_base": len([1 for s, d in revs if base == ref or d > base_date])})
        print(f"{head_sim:.2f}->{base_sim:.2f} {base_date}  {rf.relative_to(args.rex)}  <=  {best}", flush=True)
    args.out.write_text(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
