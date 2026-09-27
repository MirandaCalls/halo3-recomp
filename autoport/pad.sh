#!/usr/bin/env bash
# Send commands to a running autoport scripted pad and wait until they finish.
# Usage: pad.sh <dir> 'cmd1' 'cmd2' ...   (dir holds cmd.txt / cmd.txt.log)
# Any "shot NAME" is converted to <dir>/NAME.png.
D=$1; shift
before=$(wc -l < "$D/cmd.txt.log" 2>/dev/null || echo 0)
for c in "$@"; do echo "$c" >> "$D/cmd.txt"; done
want=$((before + $#))
for i in $(seq 1 3000); do
  [ "$(wc -l < "$D/cmd.txt.log")" -ge "$want" ] && break
  sleep 0.1
done
tail -n $# "$D/cmd.txt.log"
for c in "$@"; do
  set -- $c
  if [ "$1" = shot ] && [ -f "$D/$2.ppm" ]; then magick "$D/$2.ppm" -resize 960x "$D/$2.png" && rm -f -- "$D/$2.ppm"; fi
done
