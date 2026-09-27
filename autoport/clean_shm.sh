#!/usr/bin/env bash
# Remove guest-memory shared-memory files leaked by crashed or killed runs.
# ReXGlue/Xenia back guest memory with /dev/shm/xenia_memory_*; a run that dies
# without cleanup leaks 0.5-3 GB, and a full /dev/shm makes new runs SIGBUS.
# Only files that no running process holds open are removed.
set -euo pipefail
shopt -s nullglob
removed=0
for f in /dev/shm/xenia_memory_*; do
  if ! fuser -s "$f" 2>/dev/null; then
    rm -f -- "$f"
    removed=$((removed + 1))
  fi
done
echo "clean_shm: removed $removed leaked file(s); $(df -h /dev/shm | awk 'NR==2 {print $3 " used of " $2}')"
