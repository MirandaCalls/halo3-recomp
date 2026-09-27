#!/usr/bin/env bash
# Unattended 90 s Reach run: prints guest FPS (every 5 s) and process CPU time.
# Usage: bench_reach.sh <build-dir> <sdk-lib> <label>
set -u
B=$1; L=$2; LABEL=$3
OUT=/home/aaron/xenon-pc-comp/games/reach-clean/bench-$LABEL.log
/home/aaron/xenon-pc-comp/autoport/clean_shm.sh >/dev/null
cd "$B" || exit 1
AUTOPORT_FPS=1 LD_LIBRARY_PATH=$B:$L ./halo_reach --game_data_root=/home/aaron/xenon-pc-comp/games/reach-clean/game \
  --gpu_plugin=xenos --fullscreen=false --window_width=1280 --window_height=800 ${BENCH_ARGS:-} > "$OUT" 2>&1 &
PID=$!
timeout 90 tail -f /dev/null
T=$(awk '{print $14+$15}' /proc/$PID/stat)
kill $PID; wait $PID 2>/dev/null
echo "$LABEL: cpu_seconds=$((T/100)) over 90s wall"
grep -o 'fps [0-9.]*' "$OUT" | awk '{print $2}' | tr '\n' ' '; echo
