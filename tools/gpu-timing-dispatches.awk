# Memexport compute dispatches of an AUTOPORT_GPU_TIMING_LOG file written with
# AUTOPORT_GPU_TIMING_PER_DISPATCH=1 (C lines), ranked by GPU time per frame.
#   awk -f tools/gpu-timing-dispatches.awk gpu-timing.log
#   awk -v from=60 -v to=120 -f tools/gpu-timing-dispatches.awk gpu-timing.log
# Also fits ms = fixed + per_vertex * vertices over all dispatches, to tell a
# cost per dispatch from a cost per particle.
/^#/ || NF < 8 { next }
$1 < from + 0 || (to != "" && $1 > to + 0) { next }
{ if (!($2 in seen)) { seen[$2] = 1; frames++ } }
$3 != "C" { next }
{
  key = $8; ms = $4; v = $6
  if (!(key in n)) keys[nk++] = key
  n[key]++; t[key] += ms; verts[key] += v; if (ms > mx[key]) mx[key] = ms
  if (!(key in mn) || ms < mn[key]) mn[key] = ms
  all[key, n[key]] = ms
  N++; T += ms; SV += v; SVV += v * v; SVT += v * ms
}
END {
  if (!N) { print "no C lines (run with AUTOPORT_GPU_TIMING_PER_DISPATCH=1)"; exit }
  printf "%d frames, %.1f dispatches/frame, %.2f ms/frame, %.4f ms avg, %.0f vertices avg\n",
         frames, N / frames, T / frames, T / N, SV / N
  d = N * SVV - SV * SV
  if (d > 0) {
    slope = (N * SVT - SV * T) / d; icpt = (T - slope * SV) / N
    printf "fit: %.4f ms per dispatch + %.4f ms per 1000 vertices\n", icpt, slope * 1000
    printf "     fixed part %.2f ms/frame, per-vertex part %.2f ms/frame\n",
           icpt * N / frames, slope * SV / frames
  }
  print "\n  ms/frame  n/frame   avg ms   min ms median ms   max ms  avg verts  shader"
  for (i = 0; i < nk; i++) order[i] = keys[i]
  for (i = 0; i < nk; i++) for (j = i + 1; j < nk; j++)
    if (t[order[j]] > t[order[i]]) { x = order[i]; order[i] = order[j]; order[j] = x }
  for (i = 0; i < nk && i < 25; i++) {
    k = order[i]
    m = n[k]; delete s; for (j = 1; j <= m; j++) s[j] = all[k, j]
    asort_insertion(s, m)
    printf "  %8.3f %8.2f %8.4f %8.4f %9.4f %8.3f %10.0f  %s\n", t[k] / frames, n[k] / frames,
           t[k] / n[k], mn[k], s[int((m + 1) / 2)], mx[k], verts[k] / n[k], k
  }
}

# Sorts a[1..n] ascending (Shell sort; awk on macOS has no asort).
function asort_insertion(a, n,   gap, i, j, x) {
  for (gap = int(n / 2); gap > 0; gap = int(gap / 2))
    for (i = gap + 1; i <= n; i++) {
      x = a[i]
      for (j = i; j > gap && a[j - gap] > x; j -= gap) a[j] = a[j - gap]
      a[j] = x
    }
}
