# Frame rate and GPU time per stretch of an AUTOPORT_GPU_TIMING_LOG file, to
# find where a run slows down before summarizing that range with
# gpu-timing-summary.awk (-v from= -v to=).
#   awk -f tools/gpu-timing-timeline.awk gpu-timing.log            # 10 s stretches
#   awk -v step=5 -f tools/gpu-timing-timeline.awk gpu-timing.log  # 5 s stretches
# "GPU busy" is render passes plus the time between them in a submission; when
# it is well under the frame time, the GPU is waiting on the CPU or on syncs.
BEGIN { if (step == "") step = 10 }
/^#/ || NF < 8 { next }
{
  b = int($1 / step)
  if (!(b in seen_b)) { seen_b[b] = 1; if (min_b == "" || b < min_b) min_b = b; if (b > max_b) max_b = b }
  if (!(($2 SUBSEP b) in seen_f)) { seen_f[$2, b] = 1; frames[b]++ }
  if ($5 > 0) gap[b] += $5
  if ($3 != "S" && $3 != "E") pass[b] += $4
}
END {
  print "    from  frames    fps  frame ms  GPU busy  passes  between"
  for (b = min_b; b <= max_b; b++) {
    if (!frames[b]) continue
    f = frames[b]
    printf "%7ds %7d %6.1f %9.1f %9.1f %7.1f %8.1f\n", b * step, f, f / step,
           1000 * step / f, (pass[b] + gap[b]) / f, pass[b] / f, gap[b] / f
  }
}
