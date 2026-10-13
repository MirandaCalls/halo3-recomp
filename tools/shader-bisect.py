#!/usr/bin/env python3
"""Find the pixel shader behind a rendering artifact, live, by bisection.

  AUTOPORT_SKIP_SHADERS_FILE=$PWD/shader-skip.txt ./play-halo3.sh --name <gamertag>
  tools/shader-bisect.py shader-skip.txt        # in a second terminal

Stand still where the artifact is visible. The script reads the pixel shaders
the game drew in the last two seconds (<file>.used, written by the emulator),
then skips half of them at a time through <file> (reloaded within a quarter of a
second) and asks whether the artifact is still there. Each round tests both
halves, so a half whose skipping hides everything doesn't mislead it. The
answer is checked at the end by skipping that one shader alone.
"""
import argparse
import sys
import time
from pathlib import Path


def ask(question):
    while True:
        answer = input(f"{question} [y = still there / n = gone] ").strip().lower()
        if answer in ("y", "n"):
            return answer == "y"


def skip(path, hashes):
    path.write_text("".join(f"{h}\n" for h in hashes))
    time.sleep(0.5)  # the emulator checks the file every 0.25 s


def read_used(used):
    ps = {}
    for line in used.read_text().splitlines():
        kind, hash_, draws = line.split()
        if kind == "ps":
            ps[hash_] = int(draws)
    return ps


def split(candidates, draws):
    # Halves with about the same number of draws, so neither is mostly empty.
    ordered = sorted(candidates, key=lambda h: -draws[h])
    a, b, sa, sb = [], [], 0, 0
    for h in ordered:
        if sa <= sb:
            a.append(h)
            sa += draws[h]
        else:
            b.append(h)
            sb += draws[h]
    return a, b


def bisect(path, candidates, draws):
    pending = []  # halves set aside when both halves hid the artifact
    while True:
        while len(candidates) > 1:
            a, b = split(candidates, draws)
            print(f"\n{len(candidates)} candidate shaders; testing two halves of {len(a)} and {len(b)}")
            skip(path, a)
            a_there = ask("Half 1 skipped. Is the artifact still there?")
            skip(path, b)
            b_there = ask("Half 2 skipped. Is the artifact still there?")
            if not a_there and b_there:
                candidates = a
            elif a_there and not b_there:
                candidates = b
            elif not a_there and not b_there:
                print("Both halves hide it (one may hide the whole area); following half 1 first.")
                pending.append(b)
                candidates = a
            else:
                print("Skipping either half leaves it, so it doesn't come from a single pixel "
                      "shader (or needs shaders from both halves).")
                return None
        if candidates:
            shader = candidates[0]
            skip(path, [shader])
            if not ask(f"Only {shader} skipped. Is the artifact still there?"):
                return shader
            print("Not this one alone.")
        if not pending:
            return None
        candidates = pending.pop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("skip_file", type=Path, help="the file AUTOPORT_SKIP_SHADERS_FILE names")
    args = ap.parse_args()
    path = args.skip_file.resolve()
    used = Path(str(path) + ".used")
    path.write_text("")
    print(f"Waiting for {used} (is the game running with AUTOPORT_SKIP_SHADERS_FILE={path}?)")
    start = time.time()
    while not used.exists() or used.stat().st_mtime < start + 2:
        time.sleep(0.5)
    draws = read_used(used)
    print(f"{len(draws)} pixel shaders drawn in the last two seconds.")
    try:
        shader = bisect(path, list(draws), draws)
    finally:
        path.write_text("")
    if shader:
        print(f"\nThe artifact comes from draws using pixel shader {shader}.")
        print("Get its code with --dump_shaders=<dir> (files named after the hash):")
        print("  ./play-halo3.sh --name <gamertag> -- --dump_shaders=$PWD/shader-dump")
        print(f"  ls shader-dump | grep -i {shader}")
    else:
        print("\nNo single pixel shader found.")
        sys.exit(1)


if __name__ == "__main__":
    main()
