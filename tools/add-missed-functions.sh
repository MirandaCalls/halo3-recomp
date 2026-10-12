#!/usr/bin/env bash
# Fix "Call to invalid or unregistered function" crashes found while playing.
#
#   ./play-halo3.sh --name <gamertag> -- --log_file="$PWD/halo3.log" --log_max_file_size_mb=200
#   tools/add-missed-functions.sh halo3.log
#
# The port's setup finds functions the recompiler missed by running the boot and
# menu, so code first reached later (the campaign, for example) can still call an
# untranslated function and abort. This adds every such function named in the log
# to the project, regenerates the recompiled code and rebuilds the game.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# shellcheck source=../autoport/host.sh
. "$ROOT/autoport/host.sh"
LOG=${1:-}
if [ -z "$LOG" ] || [ ! -f "$LOG" ]; then
  echo "usage: $0 <game log written with --log_file>" >&2
  exit 1
fi
LOG=$(abspath "$LOG")

SDK=$ROOT/tools/rexglue-resync
PORT=$ROOT/games/halo3/port
B=$PORT/out/build/$BUILD_DIR_NAME
if [ ! -f "$PORT/halo3_manifest.toml" ]; then
  echo "no ported game in $PORT; run ./setup-halo3.sh first" >&2
  exit 1
fi
# setup-halo3.sh's venv has the python packages the codegen fix-ups need.
[ -x "$ROOT/tools/venv/bin/python3" ] && export PATH="$ROOT/tools/venv/bin:$PATH"
if [ "$HOST_OS" = macos ]; then
  BUILD=""
else
  BUILD="podman run --rm --userns=keep-id -v $ROOT:$ROOT:Z -w $PORT xenon-build"
fi

cd "$PORT"
python3 -u "$ROOT/autoport/runtime_loop.py" halo3_manifest.toml --from-log "$LOG" \
  --rexglue "$SDK/bin/rexglue" --sdk-lib "$SDK/$LIBDIR" --build-dir "$B" --exe halo3 \
  --dlext "$DLEXT" --libpath-var "$LIBPATH_VAR" \
  --build-cmd "$BUILD cmake --build out/build/$BUILD_DIR_NAME -j$NPROC"
