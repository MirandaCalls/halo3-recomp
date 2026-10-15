# Ranks the game's draws in one kind of render pass of an AUTOPORT_GPU_TIMING_LOG
# file written with AUTOPORT_GPU_TIMING_PER_DRAW=1, by vertex + pixel shader.
#   awk -f tools/gpu-timing-draws.awk gpu-timing-draws.log
#   awk -v targets='c0=k_2_10_10_10_FLOAT' -f tools/gpu-timing-draws.awk gpu-timing-draws.log
# targets: a regular expression the pass's render targets must match (default:
# Halo 3's main scene pass, depth + two 8_8_8_8 color targets); from=/to= limit
# the time range. Each timed draw also carries the cost of its own render pass
# (load, store, setup), so "excess" subtracts the cheapest draw time seen in
# these passes (the floor) from every draw: the part that grows with the draw.
BEGIN { if (targets == "") targets = "depth=kD24FS8,c0=k_8_8_8_8,c1=k_8_8_8_8,vs=" }
/^#/ || NF < 8 || $3 == "W" || $3 == "I" || $3 == "C" { next }
$1 < from + 0 || (to != "" && $1 > to + 0) { next }
{ if (!($2 in seen)) { seen[$2] = 1; frames++ } }
$3 != "D" || $8 !~ targets || !match($8, /,vs=[0-9A-F]+,ps=[0-9A-F]+/) { next }
{
  key = substr($8, RSTART + 1, RLENGTH - 1)
  v = 0; if (match($8, /,v=[0-9]+/)) v = substr($8, RSTART + 3, RLENGTH - 3) + 0
  n++; ms[n] = $4 + 0; k[n] = key; vv[n] = v
  if (floor == "" || $4 + 0 < floor) floor = $4 + 0
}
END {
  if (!n) { print "no matching per-draw passes (AUTOPORT_GPU_TIMING_PER_DRAW=1, targets=" targets ")"; exit }
  for (i = 1; i <= n; i++) {
    key = k[i]; if (!(key in cnt)) keys[nk++] = key
    cnt[key]++; raw[key] += ms[i]; ex = ms[i] - floor; if (ex < 0) ex = 0
    exc[key] += ex; verts[key] += vv[i]; total_ex += ex; total_raw += ms[i]
  }
  printf "%d frames, %.1f draws/frame, floor %.3f ms per draw\n", frames, n / frames, floor
  printf "raw %.2f ms/frame, excess over the floor %.2f ms/frame\n\n", total_raw / frames, total_ex / frames
  printf "  %8s %6s %8s %9s %9s  %s\n", "excess", "share", "n/frame", "avg exc", "avg vert", "shaders (excess ms/frame)"
  for (r = 0; r < 25 && r < nk; r++) {
    best = -1
    for (i = 0; i < nk; i++) if (!(i in used) && (best < 0 || exc[keys[i]] > exc[keys[best]])) best = i
    used[best] = 1; key = keys[best]
    printf "  %8.3f %5.1f%% %8.2f %9.4f %9.0f  %s\n", exc[key] / frames, total_ex ? 100 * exc[key] / total_ex : 0,
           cnt[key] / frames, exc[key] / cnt[key], verts[key] / cnt[key], key
  }
}
