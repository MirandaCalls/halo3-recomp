#!/usr/bin/env python3
"""Find the pixel shader behind a rendering artifact, live, by bisection.

  AUTOPORT_SKIP_SHADERS_FILE=$PWD/shader-skip.txt ./play-halo3.sh --name <gamertag>
  tools/shader-bisect.py shader-skip.txt        # in a second terminal

Stand still where the artifact is visible. The script reads the pixel shaders
the game drew in the last two seconds (<file>.used, written by the emulator),
then skips groups of them through <file> (reloaded within a quarter of a
second) and asks whether the artifact is still there. When skipping a group
breaks the view (lighting, post-processing), answer "b": it splits that group
further and sets the shaders that break it aside. The
answer is checked at the end by skipping that one shader alone.
"""
import argparse
import sys
import time
from pathlib import Path


def ask(question):
    while True:
        answer = input(f"{question}\n  [y = dots still there / n = gone / b = view too broken to tell] ").strip().lower()
        if answer in ("y", "n", "b"):
            return answer


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
    """Narrow candidates down to the shader whose skipping removes the dots.

    Each round splits the candidates in two and skips one group at a time
    until a group removes the dots. Skipping some shaders (lighting,
    post-processing) breaks the whole view; a group that does is split until
    those shaders are found, and they are set aside. Assumes one shader is
    responsible.
    """
    breaking = []
    while len(candidates) > 1:
        print(f"\n{len(candidates)} candidate shaders left"
              + (f" ({len(breaking)} set aside for breaking the view)" if breaking else ""))
        groups = list(split(candidates, draws))
        found = None
        while groups and found is None:
            group = groups.pop(0)
            skip(path, group)
            answer = ask(f"Skipping {len(group)} shader(s). Are the dots still there?")
            if answer == "n":
                found = group
            elif answer == "b":
                if len(group) == 1:
                    print(f"  {group[0]} breaks the view; setting it aside.")
                    breaking.append(group[0])
                else:
                    groups[:0] = list(split(group, draws))
        if found is None:
            print("\nNo testable group removed the dots.")
            if breaking:
                print("The shader may be one of those that break the view: " + ", ".join(breaking))
            return None
        candidates = found
    shader = candidates[0]
    skip(path, [shader])
    if ask(f"Only {shader} skipped. Are the dots still there?") == "n":
        return shader
    print("Skipping it alone doesn't remove them (the dots may need more than one shader).")
    return None


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
