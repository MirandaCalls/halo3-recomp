#!/usr/bin/env bash
# Pipeline harvest for Halo: Reach: plays a Slayer custom game on every
# multiplayer map with the scripted pad, so the runtime records each graphics
# pipeline the maps need (4D53085B.fbo.vk.xpso). Shipped with the port, those
# pipelines are compiled at boot instead of the first time they appear.
#
# Usage: reach_harvest.sh [seconds per map]   (Reach must NOT be running)
# Menu route (custom game lobby, cursor on START GAME):
#   map:   UP UP (MAP) A, DOWN (next map in list) A (map) A (variant)
#   start: DOWN DOWN (START GAME), A until loaded
#   leave: START A (END GAME) UP A (confirm) B (credits) B (carnage report)
set -u
SECS=${1:-60}
ROOT=/home/aaron/xenon-pc-comp
D=$ROOT/games/reach-clean/harvest-run
PAD=$ROOT/autoport/pad.sh
XPSO=$HOME/.local/share/halo_reach/cache/shaders/shareable/4D53085B.fbo.vk.xpso
MAPS=(Boardwalk Boneyard Countdown "Forge World" Powerhouse Reflection Spire "Sword Base" Zealot)
mkdir -p "$D"
if pgrep -x halo_reach >/dev/null; then
  echo "halo_reach is already running; close it first" >&2
  exit 1
fi
count_pipelines() { stat -c %s "$XPSO"; }  # storage bytes (grows with each new pipeline)
before=$(count_pipelines)
"$ROOT/play-reach.sh" --pad "$D" > "$D/run.log" 2>&1 &
sleep 25
LOG=$(ls -t "$ROOT"/games/reach-clean/port/out/build/resync/logs/*.log | head -1)

# Boot -> main menu -> Custom Game lobby (cursor ends on START GAME after a map).
$PAD "$D" 'press A' 'wait 1500' 'press START' 'wait 1500' 'press A' 'wait 3000' 'press B' 'wait 2500' \
  'press B' 'wait 2500' 'press A' 'wait 4000' 'press A' 'wait 8000' 'press DOWN' 'wait 400' \
  'press DOWN' 'wait 400' >/dev/null

for i in "${!MAPS[@]}"; do
  # Pick map i: the list opens on the current map, so the first pick goes to
  # the top and later ones step down one entry.
  $PAD "$D" 'press UP' 'wait 400' 'press UP' 'wait 400' 'press A' 'wait 2500' >/dev/null
  if [ "$i" = 0 ]; then
    for _ in 1 2 3 4 5 6 7 8 9; do $PAD "$D" 'press UP' 'wait 250' >/dev/null; done
  else
    $PAD "$D" 'press DOWN' 'wait 400' >/dev/null
  fi
  $PAD "$D" 'press A' 'wait 2000' 'press A' 'wait 3000' 'press DOWN' 'wait 400' 'press DOWN' 'wait 400' >/dev/null
  for _ in $(seq 1 10); do $PAD "$D" 'press A' 'wait 4000' >/dev/null; done

  # Play: sweep the view all the way round, move, look up and down.
  $PAD "$D" "shot map$i" 'rstick 0.35 0 12000' 'lstick 0 1 4000' 'rstick 0 0.5 800' \
    'rstick -0.35 0 12000' 'rstick 0 -0.5 1600' 'lstick 0.6 1 4000' 'rt 1 1500' >/dev/null
  rest=$(( SECS - 36 )); [ "$rest" -gt 0 ] && $PAD "$D" "rstick 0.25 0 $(( rest * 1000 ))" >/dev/null

  bad=$(grep -c 'Unhandled guest access\|XamAlloc.*failed' "$LOG")
  mean=$(magick "$D/map$i.png" -format '%[fx:mean]' info: 2>/dev/null)
  echo "${MAPS[$i]}: pipeline storage $(count_pipelines) bytes (+$(( $(count_pipelines) - before ))), frame mean $mean, faults $bad"
  if ! pgrep -x halo_reach >/dev/null || [ "$bad" -gt 0 ]; then
    echo "stopping: game exited or faulted" >&2
    break
  fi

  # Leave the match and return to the lobby.
  $PAD "$D" 'press START' 'wait 1500' 'press A' 'wait 1500' 'press UP' 'wait 400' 'press A' \
    'wait 12000' 'press B' 'wait 6000' 'press B' 'wait 5000' >/dev/null
done

pkill -x halo_reach
sleep 2
"$ROOT/autoport/clean_shm.sh" >/dev/null
echo "harvest: pipeline storage $before -> $(count_pipelines) bytes"
