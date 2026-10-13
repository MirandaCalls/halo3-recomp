#!/usr/bin/env python3
"""Line up the passes of multipass draws in an AUTOPORT_DRAW_LOG file.

  AUTOPORT_DRAW_LOG=$PWD/draws.log ./play-halo3.sh --name <gamertag>
  touch draws.log.trigger            # while looking at the artifact
  tools/draw-log-pairs.py draws.log  # > pairs.txt
  tools/draw-log-pairs.py draws.log --shader 6E32A3F3 --max 3   # groups using one shader

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


FILTER = ["point", "linear", "basemap", "useconst"]
ANISO = ["off", "1x", "2x", "4x", "8x", "16x", "max", "useconst"]


def signed(value, count):
    return value - (1 << count) if value & (1 << (count - 1)) else value


def decode_texture(dw):
    d0, d1, d2, d3, d4, d5 = dw
    if bits(d0, 0, 2) != 2:
        return "(not a texture)"
    return (f"addr={bits(d1, 12, 20) << 12:08X} fmt={bits(d1, 0, 6)} "
            f"size={bits(d2, 0, 13) + 1}x{bits(d2, 13, 13) + 1} "
            f"signs={bits(d0, 2, 8):02X} swz={bits(d3, 1, 12):03X} exp={signed(bits(d3, 13, 6), 6)} "
            f"mag={FILTER[bits(d3, 19, 2)]} min={FILTER[bits(d3, 21, 2)]} "
            f"mip={FILTER[bits(d3, 23, 2)]} aniso={ANISO[bits(d3, 25, 3)]} "
            f"mips={bits(d4, 0, 4)}-{bits(d4, 4, 4)} lodbias={signed(bits(d4, 10, 10), 10) / 32:g} "
            f"grad={signed(bits(d4, 20, 5), 5)},{signed(bits(d4, 25, 5), 5)} "
            f"clamp={bits(d0, 10, 3)}{bits(d0, 13, 3)} mipaddr={bits(d5, 12, 20) << 12:08X}")


def decode(r):
    d = {}
    for key, value in r.items():
        if key.startswith("tf"):
            d[key] = decode_texture(value)
        elif key.startswith("psc"):
            d[key] = " ".join(f"{as_float(v):g}" for v in value)
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
    ap.add_argument("--shader", action="append", default=[],
                    help="only groups with a draw using this vertex or pixel shader (hash prefix); "
                         "repeatable")
    ap.add_argument("--max", type=int, default=0, help="print at most this many groups")
    args = ap.parse_args()
    draws = []
    for line in open(args.log):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        frame, index, vs, ps, prim, count = parts[:6]
        regs = {}
        for k, v in (p.split("=") for p in parts[6:]):
            regs[k] = [int(x, 16) for x in v.split(":")] if ":" in v else int(v, 16)
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
        if args.shader and not any(m[k].startswith(h.upper()) for m in members
                                   for k in ("vs", "ps") for h in args.shader):
            continue
        if args.max and shown >= args.max:
            break
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
