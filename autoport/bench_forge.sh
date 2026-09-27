#!/usr/bin/env bash
# Reach gameplay benchmark: boots with the scripted pad, starts a Custom Game
# (Slayer on Hemorrhage / Forge World), walks a fixed route and measures a
# 40 s window of play.
#
# Usage: bench_forge.sh <label> [extra play-reach.sh options...]
# Output: guest FPS samples, CPU-ms per guest frame per thread, and shots, in
#         games/reach-clean/bench/<label>/.
set -u
LABEL=$1; shift
ROOT=/home/aaron/xenon-pc-comp
D=$ROOT/games/reach-clean/bench/$LABEL
PAD=$ROOT/autoport/pad.sh
mkdir -p "$D"

if pgrep -x halo_reach >/dev/null; then
  echo "halo_reach is already running; close it first" >&2
  exit 1
fi

"$ROOT/play-reach.sh" --fps --pad "$D" "$@" > "$D/run.log" 2>&1 &
sleep 3
PID=$(pgrep -x halo_reach)
sleep 22

# Intro -> Start menu -> title -> main menu -> Custom Game lobby -> start.
$PAD "$D" 'press A' 'wait 1500' 'press START' 'wait 1500' 'press A' 'wait 3000' 'press B' 'wait 2500' \
  'press B' 'wait 2500' 'press A' 'wait 4000' 'press A' 'wait 10000' >/dev/null
# Map load time varies: keep pressing A (Start Game in the lobby, jump once in game).
for i in $(seq 1 12); do $PAD "$D" 'press A' 'wait 5000' >/dev/null; done
$PAD "$D" 'wait 5000' 'shot spawn' >/dev/null
# Out of the spawn canyon.
$PAD "$D" 'rstick 1 0 900' 'lstick 0 1 3500' 'rstick -1 0 500' 'lstick 0 1 3000' >/dev/null

snap() { for t in /proc/$PID/task/*; do echo "$(basename "$t") $(tr ' ' _ < "$t/comm") $(awk '{print $14+$15}' "$t/stat")"; done | sort; }

snap > "$D/cpu0"
f0=$(grep -c 'fps' "$D/run.log")
t0=$(date +%s.%N)
# 40 s of play: sweep the view and move so the scene keeps changing.
$PAD "$D" 'rstick 0.5 0 4000' 'lstick 0 1 4000' 'rstick -0.5 0 4000' 'lstick 0.5 1 4000' \
  'rstick 0.5 0.2 4000' 'lstick -0.5 1 4000' 'rt 1 1500' 'rstick -0.5 -0.2 4000' 'lstick 0 1 5000' \
  'shot mid' 'rstick 0.7 0 3000' >/dev/null
t1=$(date +%s.%N)
snap > "$D/cpu1"
if [ -z "${BENCH_KEEP:-}" ]; then
  kill "$PID"; wait "$PID" 2>/dev/null
  "$ROOT/autoport/clean_shm.sh" >/dev/null
fi

secs=$(echo "$t1 - $t0" | bc)
fps=$(grep -o 'fps [0-9.]*' "$D/run.log" | tail -n +$((f0 + 1)) | awk '{print $2}' | tr '\n' ' ')
avg=$(echo "$fps" | awk '{s=0; for(i=1;i<=NF;i++) s+=$i; if (NF) printf "%.1f", s/NF}')
echo "[$LABEL] window ${secs}s  fps samples: $fps (avg $avg)"
join "$D/cpu0" "$D/cpu1" | awk -v s="$secs" -v f="$avg" '
  { d = $5 - $3; if (d > s && f > 0) { tot += d; printf "  %-24s %5.1f%% core  %5.2f ms/frame\n", $2, d / s, d * 10 / (s * f) } }
  END { if (f > 0) printf "  %-24s %5.1f%% core  %5.2f ms/frame\n", "TOTAL", tot / s, tot * 10 / (s * f) }' | sort -k2 -rn
for s in spawn mid; do [ -f "$D/$s.ppm" ] && magick "$D/$s.ppm" -resize 960x "$D/$s.png" && rm -f -- "$D/$s.ppm"; done
