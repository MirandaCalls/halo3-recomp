# Frame times of an AUTOPORT_GPU_TIMING_LOG file in 4 ms bins, from the per-frame
# W lines (builds from 1bc953b on). Frames held to the 60 Hz vblank bunch at
# 33, 50 and 67 ms.
#   awk -f tools/frame-time-histogram.awk gpu-timing.log
#   awk -v from=60 -v to=120 -f tools/frame-time-histogram.awk gpu-timing.log
$3 == "W" && $1 >= from + 0 && (to == "" || $1 <= to + 0) {
  if (p != "") { d = ($1 - p) * 1000; b = int(d / 4) * 4; if (b > 100) b = 100; h[b]++; n++ }
  p = $1
}
END { for (b = 0; b <= 100; b += 4) if (h[b]) printf "%3d-%3d ms %6d %5.1f%%\n", b, b + 4, h[b], 100 * h[b] / n }
