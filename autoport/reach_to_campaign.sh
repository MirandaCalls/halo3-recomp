#!/usr/bin/env bash
# Boot Reach with the scripted pad and drive it into the solo campaign.
# Usage: reach_to_campaign.sh <play-dir>   (extra env/args via REACH_ENV / REACH_ARGS)
set -u
D=$1; mkdir -p "$D"
PAD=/home/aaron/xenon-pc-comp/autoport/pad.sh
B=/home/aaron/xenon-pc-comp/games/reach-clean/port/out/build/resync
L=/home/aaron/xenon-pc-comp/tools/rexglue-resync/lib64
pkill -x halo_reach; sleep 2
/home/aaron/xenon-pc-comp/autoport/clean_shm.sh >/dev/null
(cd "$B" && env ${REACH_ENV:-} AUTOPORT_INPUT_FILE="$D/cmd.txt" LD_LIBRARY_PATH="$B:$L" ./halo_reach \
  --game_data_root=/home/aaron/xenon-pc-comp/games/reach-clean/game --gpu_plugin=xenos \
  --fullscreen=false --window_width=1280 --window_height=800 ${REACH_ARGS:-} > "$D/run.log" 2>&1 &)
sleep 25
$PAD "$D" 'press A' 'wait 1500' 'press START' 'wait 1500' 'press A' 'wait 3000' 'press B' 'wait 2500' \
  'press B' 'wait 2500' 'shot title' 'press UP' 'wait 700' 'press A' 'wait 3000' 'press A' 'wait 25000' \
  'shot c1' 'press A' 'wait 800' 'press Y' 'wait 10000' 'shot c2' 'wait 15000' 'shot c3' >/dev/null
for s in title c1 c2 c3; do echo "$s mean=$(magick "$D/$s.png" -format '%[fx:mean]' info: 2>/dev/null)"; done
