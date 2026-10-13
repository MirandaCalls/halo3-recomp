#!/usr/bin/env bash
# Build Halo 3 with a tracer of the calls engine code makes into the Xbox D3D
# library, to map the game's renderer for a native (D3D-hook) renderer.
#
#   tools/d3d-trace.sh            # generate the tracer and rebuild the game
#   AUTOPORT_D3D_TRACE=$PWD/d3d-trace.txt ./play-halo3.sh --name <gamertag>
#   tools/d3d-trace.sh --remove   # rebuild without it
#
# The trace file is rewritten every two seconds as "<function> <caller> <count>"
# lines (autoport/gpu/gen_d3d_tracer.py). Unlike the static d3d_features.py
# table, it includes calls made through function pointers (the device's
# render-state table, draw entry points), which is how engine code reaches much
# of D3D. Without AUTOPORT_D3D_TRACE set the tracer only forwards calls.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=../autoport/host.sh
. "$ROOT/autoport/host.sh"
PORT=$ROOT/games/halo3/port
TRACER=generated/autoport_d3d_trace.cpp
MARKER="# autoport: D3D call tracer"
if [ ! -f "$PORT/halo3_manifest.toml" ]; then
  echo "no ported game in $PORT; run ./setup-halo3.sh first" >&2
  exit 1
fi
PY=python3
[ -x "$ROOT/tools/venv/bin/python3" ] && PY=$ROOT/tools/venv/bin/python3

cd "$PORT"
if [ "${1:-}" = --remove ]; then
  rm -f "$TRACER"
else
  mkdir -p autoport_logs
  "$PY" "$ROOT/autoport/gpu/triage.py" halo3_manifest.toml --json autoport_logs/d3d-triage.json >/dev/null
  # Halo 3's D3D library is linked in two blocks: triage finds the one at
  # 825A0000; the ring buffer writer (82147618) and fixed-function draws are in
  # a second block around 82140000, likely with the draw path
  # (docs/halo3-native-renderer.md).
  "$PY" "$ROOT/autoport/gpu/gen_d3d_tracer.py" halo3_manifest.toml autoport_logs/d3d-triage.json \
    --range 82140000-821A0000 --out "$TRACER"
  if ! grep -qF "$MARKER" CMakeLists.txt; then
    cat >> CMakeLists.txt <<'EOF'

# autoport: D3D call tracer (tools/d3d-trace.sh; --remove drops it)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/generated/autoport_d3d_trace.cpp)
  target_sources(halo3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated/autoport_d3d_trace.cpp)
endif()
EOF
  fi
fi
# The EXISTS check is evaluated when CMake configures, so make it reconfigure.
touch CMakeLists.txt
host_build "$PORT" "cmake --build out/build/$BUILD_DIR_NAME -j$NPROC"
if [ -f "$TRACER" ]; then
  echo "tracer built in; run with AUTOPORT_D3D_TRACE=<file> to record"
else
  echo "tracer removed"
fi
