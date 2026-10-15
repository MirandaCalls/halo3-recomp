# Frame rate and GPU time per stretch of an AUTOPORT_GPU_TIMING_LOG file, to
# find where a run slows down before summarizing that range with
# gpu-timing-summary.awk (-v from= -v to=).
#   awk -f tools/gpu-timing-timeline.awk gpu-timing.log            # 10 s stretches
#   awk -v step=5 -f tools/gpu-timing-timeline.awk gpu-timing.log  # 5 s stretches
# All columns but fps are ms per frame.
#   GPU busy   render passes ("passes") plus the time between them ("between")
#   GPU idle   time between submissions, when the GPU had nothing queued
# and, from builds that log them, what the command processor thread waited on:
#   ring       the guest's command ring was empty (the game hadn't submitted)
#   wrm        WAIT_REG_MEM: the guest's commands wait for a value to be written
#   wrm gpu    the part of wrm spent waiting for the GPU to finish (newer builds)
#   ffence     a frame older than the frames-in-flight limit to finish on the GPU
#   fence      other GPU work to finish (readbacks, queries)
#   swap       presenting the frame
#   early      submissions per frame ended early because the GPU ran dry
BEGIN { if (step == "") step = 10 }
/^#/ { next }
$3 == "I" && NF == 4 { idle[int($1 / step)] += $4; has_idle = 1; next }
$3 == "W" && NF >= 8 {
  b = int($1 / step)
  ring[b] += $4; wrm[b] += $5; ffence[b] += $6; fence[b] += $7; swap[b] += $8
  if (NF >= 10) { wrm_gpu[b] += $9; early[b] += $10; has_early = 1 }
  has_waits = 1; next
}
NF < 8 || $3 == "C" { next }
{
  b = int($1 / step)
  if (!(b in seen_b)) { seen_b[b] = 1; if (min_b == "" || b < min_b) min_b = b; if (b > max_b) max_b = b }
  if (!(($2 SUBSEP b) in seen_f)) { seen_f[$2, b] = 1; frames[b]++ }
  if ($5 > 0) gap[b] += $5
  if ($3 != "S" && $3 != "E") pass[b] += $4
}
END {
  printf "    from    fps  frame ms  GPU busy  passes  between"
  if (has_idle) printf "  GPU idle"
  if (has_waits) printf "    ring     wrm  ffence   fence    swap"
  if (has_early) printf "  wrm gpu   early"
  printf "\n"
  for (b = min_b; b <= max_b; b++) {
    if (!frames[b]) continue
    f = frames[b]
    printf "%7ds %6.1f %9.1f %9.1f %7.1f %8.1f", b * step, f / step, 1000 * step / f,
           (pass[b] + gap[b]) / f, pass[b] / f, gap[b] / f
    if (has_idle) printf " %9.1f", idle[b] / f
    if (has_waits) printf " %7.1f %7.1f %7.1f %7.1f %7.1f", ring[b] / f, wrm[b] / f,
                          ffence[b] / f, fence[b] / f, swap[b] / f
    if (has_early) printf " %8.1f %7.1f", wrm_gpu[b] / f, early[b] / f
    printf "\n"
  }
}
