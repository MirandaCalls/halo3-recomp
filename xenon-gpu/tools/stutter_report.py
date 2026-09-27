#!/usr/bin/env python3
"""Attribute frame hitches using an AUTOPORT_STUTTER_LOG event log.

Frames are the intervals between guest swaps (S). For each frame longer than
--hitch ms, sums pipeline creations (P) and texture loads (T) that finished in
that interval and counts skipped presents (K)."""
import argparse
import collections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--hitch", type=float, default=50.0)
    ap.add_argument("--after", type=float, default=0.0, help="ignore events before this time (s)")
    args = ap.parse_args()
    ev = []
    for line in open(args.log):
        p = line.split()
        if len(p) >= 5 and float(p[0]) >= args.after:
            ev.append((float(p[0]), p[1], float(p[2]), p[3], p[4]))
    swaps = [e[0] for e in ev if e[1] == "S"]
    kinds = collections.Counter(e[1] for e in ev)
    print(f"events: {dict(kinds)}; frames {len(swaps) - 1}")
    if len(swaps) < 2:
        return
    deltas = [(b - a) * 1000 for a, b in zip(swaps, swaps[1:])]
    deltas_sorted = sorted(deltas)
    pct = lambda q: deltas_sorted[min(len(deltas_sorted) - 1, int(q * len(deltas_sorted)))]
    print(f"frame ms: median {pct(0.5):.1f}  p95 {pct(0.95):.1f}  p99 {pct(0.99):.1f}  max {deltas_sorted[-1]:.1f}")
    for kind, name in (("P", "pipeline creations"), ("T", "texture loads")):
        d = sorted(e[2] for e in ev if e[1] == kind)
        if d:
            print(f"{name}: {len(d)}, total {sum(d):.0f} ms, median {d[len(d)//2]:.2f} ms, max {d[-1]:.1f} ms")
    threads = collections.Counter((e[1], e[4]) for e in ev if e[1] in "PT")
    print("by thread:", ", ".join(f"{k}@{t}:{n}" for (k, t), n in threads.most_common(8)))
    hitches = [(swaps[i], swaps[i + 1], deltas[i]) for i in range(len(deltas)) if deltas[i] > args.hitch]
    print(f"hitches > {args.hitch:.0f} ms: {len(hitches)}")
    cause = collections.Counter()
    for a, b, d in hitches:
        inside = [e for e in ev if a < e[0] <= b]
        p = sum(e[2] for e in inside if e[1] == "P")
        t = sum(e[2] for e in inside if e[1] == "T")
        k = sum(1 for e in inside if e[1] == "K")
        tag = "pipelines" if p > d * 0.3 else ("textures" if t > d * 0.3 else ("skips" if k else "other"))
        cause[tag] += 1
        if len(hitches) <= 40 or cause[tag] <= 5:
            print(f"  t={a:8.2f}  {d:6.1f} ms  pipelines {p:6.1f} ms  textures {t:6.1f} ms  skips {k}  -> {tag}")
    print("hitch causes:", dict(cause))


if __name__ == "__main__":
    main()
