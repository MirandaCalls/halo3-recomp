# Ranked summary of an AUTOPORT_GPU_TIMING_LOG file (GPU time per render pass).
#   awk -f tools/gpu-timing-summary.awk gpu-timing.log
#   awk -v from=25 -f tools/gpu-timing-summary.awk gpu-timing.log   # skip the first 25 s
# Passes are grouped by kind, size and render targets; "per frame" divides by the
# number of guest frames in the measured range. At 30 fps a frame has 33.3 ms.
/^#/ { if ($0 ~ /device|not supported|not timed/) notes[nnotes++] = $0; next }
NF < 8 || $1 < from { next }
{
  if (!($2 in seen)) { seen[$2] = 1; frames++ }
  if (first == "" || $1 < first) first = $1
  if (last == "" || $1 > last) last = $1
  ms = $4; passes++; total += ms; if (ms > 0) nonzero++
  if ($5 > 0) gap += $5
  kind_ms[$3] += ms; kind_n[$3]++
  key = $3 " " $6 " " $8
  if (!(key in g_ms)) keys[nkeys++] = key
  g_ms[key] += ms; g_n[key]++; g_draws[key] += $7
  if (ms > g_max[key]) g_max[key] = ms
  # five longest single passes
  for (i = 0; i < 5; i++) if (ms > top_ms[i]) break
  if (i < 5) {
    for (j = 4; j > i; j--) { top_ms[j] = top_ms[j-1]; top_line[j] = top_line[j-1] }
    top_ms[i] = ms; top_line[i] = $0
  }
}
END {
  for (i = 0; i < nnotes; i++) print notes[i]
  if (!passes || frames < 2) { print "no timed passes" (from ? " after " from " s" : ""); exit }
  if (!nonzero) print "WARNING: every pass measured 0 ms, so this GPU driver's timestamps are not usable"
  printf "%d frames over %.0f s (%.1f fps), %d passes (%.0f per frame)\n", frames, last - first,
         (last > first) ? frames / (last - first) : 0, passes, passes / frames
  printf "GPU ms per frame: %.1f in render passes, %.1f between passes (compute, copies)\n\n",
         total / frames, gap / frames
  print "by kind (D game draws, X EDRAM transfer/clear, P copy to window):"
  for (k in kind_ms)
    printf "  %s  %6.1f ms/frame  %5.1f%%  %6.1f passes/frame\n", k, kind_ms[k] / frames,
           100 * kind_ms[k] / total, kind_n[k] / frames
  # rank groups by total time (selection of the top 15)
  print "\nmost expensive pass groups:"
  printf "  %8s %6s %8s %8s %8s  %s\n", "ms/frame", "share", "n/frame", "avg ms", "max ms", "kind size draws/pass targets"
  for (r = 0; r < 15 && r < nkeys; r++) {
    best = -1
    for (i = 0; i < nkeys; i++) if (!(i in used) && (best < 0 || g_ms[keys[i]] > g_ms[keys[best]])) best = i
    used[best] = 1; k = keys[best]; split(k, f, " ")
    printf "  %8.2f %5.1f%% %8.1f %8.3f %8.3f  %s %s %.0f %s\n", g_ms[k] / frames, 100 * g_ms[k] / total,
           g_n[k] / frames, g_ms[k] / g_n[k], g_max[k], f[1], f[2], g_draws[k] / g_n[k], f[3]
  }
  print "\nlongest single passes (seconds frame kind gpu_ms gap_ms size draws targets):"
  for (i = 0; i < 5 && top_line[i] != ""; i++) print "  " top_line[i]
}
