#!/usr/bin/env python3
"""Line up the passes of multipass draws in an AUTOPORT_DRAW_LOG file.

  AUTOPORT_DRAW_LOG=$PWD/draws.log ./play-halo3.sh --name <gamertag>
  touch draws.log.trigger            # while looking at the artifact
  tools/draw-log-pairs.py draws.log  # > pairs.txt

Draws of the same mesh share their index buffer, index count and first vertex
buffer; when a frame draws a mesh more than once with different shaders, those
draws are the passes of a multipass effect (Halo 3: albedo, then lighting with
a depth test against the first pass's depth). Each group is printed with the
decoded state of every pass, and the fields that differ between passes are
marked with '*'.
"""
import argparse
import collections

CMP = ["never", "less", "equal", "lequal", "greater", "notequal", "gequal", "always"]
STENCIL_OP = ["keep", "zero", "replace", "incr_clamp", "decr_clamp", "invert", "incr_wrap",
              "decr_wrap"]
COLOR_FORMAT = {0: "8888", 1: "8888_gamma", 2: "2_10_10_10", 3: "2_10_10_10_float", 4: "16_16",
                5: "16_16_16_16", 6: "16_16_float", 7: "16_16_16_16_float",
                10: "2_10_10_10_as_10_10_10_10", 12: "2_10_10_10_float_as_16_16_16_16",
                14: "32_float", 15: "32_32_float"}


def bits(value, shift, count):
    return (value >> shift) & ((1 << count) - 1)


def as_float(value):
    import struct
    return struct.unpack("<f", struct.pack("<I", value))[0]


def decode(r):
    d = {}
    dc = r["depthcontrol"]
    d["depth"] = (f"{CMP[bits(dc, 4, 3)]}{' write' if bits(dc, 2, 1) else ''}"
                  if bits(dc, 1, 1) else "off")
    if bits(dc, 0, 1):
        s = r["stencil"]
        d["stencil"] = (f"{CMP[bits(dc, 8, 3)]} ref={bits(s, 0, 8):02X} mask={bits(s, 8, 8):02X} "
                        f"write={bits(s, 16, 8):02X} fail={STENCIL_OP[bits(dc, 11, 3)]} "
                        f"zpass={STENCIL_OP[bits(dc, 14, 3)]} zfail={STENCIL_OP[bits(dc, 17, 3)]}")
    else:
        d["stencil"] = "off"
    cc = r["colorcontrol"]
    d["alphatest"] = (f"{CMP[bits(cc, 0, 3)]} {as_float(r['alpharef']):.4f}"
                      if bits(cc, 3, 1) else "off")
    d["alpha2mask"] = "on" if bits(cc, 4, 1) else "off"
    b = r["blend0"]
    d["blend0"] = (f"c:{bits(b, 0, 5)}/{bits(b, 5, 3)}/{bits(b, 8, 5)} "
                   f"a:{bits(b, 16, 5)}/{bits(b, 21, 3)}/{bits(b, 24, 5)}")
    d["colormask"] = f"{r['colormask']:08X}"
    sc = r["scmode"]
    cull = "front" if bits(sc, 0, 1) else ("back" if bits(sc, 1, 1) else "none")
    d["cull"] = cull
    if bits(sc, 11, 1) or bits(sc, 12, 1) or bits(sc, 13, 1):
        d["polyoffset"] = (f"front={as_float(r['polyfrontscale']):g},"
                           f"{as_float(r['polyfrontoffset']):g} "
                           f"back={as_float(r['polybackscale']):g},"
                           f"{as_float(r['polybackoffset']):g}")
    else:
        d["polyoffset"] = "off"
    for name in ("color0", "color1"):
        ci = r[name]
        d[name] = (f"{COLOR_FORMAT.get(bits(ci, 16, 4), bits(ci, 16, 4))}@{bits(ci, 0, 12)}"
                   f" bias={bits(ci, 20, 6) - (64 if bits(ci, 25, 1) else 0)}")
    di = r["depthinfo"]
    d["depthrt"] = f"{'d24fs8' if bits(di, 16, 1) else 'd24s8'}@{bits(di, 0, 12)}"
    d["surface"] = f"pitch={bits(r['surface'], 0, 14)} msaa={bits(r['surface'], 16, 2)}"
    d["vte"] = f"{r['vte']:08X}"
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--all", action="store_true",
                    help="also print groups whose passes all use the same pixel shader")
    args = ap.parse_args()
    draws = []
    for line in open(args.log):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        frame, index, vs, ps, prim, count = parts[:6]
        regs = {k: int(v, 16) for k, v in (p.split("=") for p in parts[6:])}
        draws.append(dict(frame=int(frame), index=int(index), vs=vs, ps=ps, prim=int(prim),
                          count=int(count), regs=regs))
    groups = collections.defaultdict(list)
    for d in draws:
        r = d["regs"]
        key = (d["frame"], r["indexbase"], d["count"], r["vf0"] & ~3)
        groups[key].append(d)
    shown = 0
    for key, members in sorted(groups.items(), key=lambda kv: kv[1][0]["index"]):
        if len(members) < 2:
            continue
        if not args.all and len({m["ps"] for m in members}) < 2:
            continue
        decoded = [decode(m["regs"]) for m in members]
        fields = list(decoded[0].keys())
        differing = {f for f in fields if len({dd[f] for dd in decoded}) > 1}
        frame, indexbase, count, vf0 = key
        print(f"== frame {frame}: index buffer {indexbase:08X}, {count} indices, "
              f"vertex buffer {vf0:08X}: {len(members)} passes")
        for m, dd in zip(members, decoded):
            print(f"  draw {m['index']:5}  vs {m['vs']}  ps {m['ps']}")
            for f in fields:
                mark = "*" if f in differing else " "
                print(f"     {mark} {f:11} {dd[f]}")
        print()
        shown += 1
    print(f"{shown} multipass group(s) in {len(draws)} draws")


if __name__ == "__main__":
    main()
