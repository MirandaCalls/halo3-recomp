# Frame-time summary of an AUTOPORT_STUTTER_LOG file (S = guest frame).
#   awk -f tools/stutter-summary.awk stutter.log
# Halo 3 targets 30 fps, so a smooth frame is 33.3 ms.
$2 == "S" { if (last != "") { dt[n++] = ($1 - last) * 1000; } last = $1; next }
$2 == "K" { skipped++ }
$2 == "T" { tex++; tex_ms += $3 }
$2 == "P" { pso++; pso_ms += $3 }
END {
  if (n < 2) { print "not enough frames"; exit }
  for (i = 0; i < n; i++) { s[i] = dt[i]; total += dt[i]; if (dt[i] > 33.4) over33++; if (dt[i] > 50) over50++; if (dt[i] > 100) over100++ }
  qsort(s, 0, n - 1)
  printf "frames: %d over %.0f s, average %.1f fps\n", n, total / 1000, n / (total / 1000)
  printf "frame time ms: median %.1f  p90 %.1f  p99 %.1f  max %.1f\n", s[int(n*0.5)], s[int(n*0.9)], s[int(n*0.99)], s[n-1]
  printf "frames slower than 33 ms: %d (%.1f%%)  50 ms: %d  100 ms: %d\n", over33, 100*over33/n, over50, over100
  printf "skipped swaps (K): %d  texture loads (T): %d, %.0f ms  shader compiles (P): %d, %.0f ms\n", skipped, tex, tex_ms, pso, pso_ms
}
function qsort(a, lo, hi,   i, j, p, t) {
  if (lo >= hi) return
  p = a[int((lo + hi) / 2)]; i = lo; j = hi
  while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { t = a[i]; a[i] = a[j]; a[j] = t; i++; j-- } }
  qsort(a, lo, j); qsort(a, i, hi)
}
