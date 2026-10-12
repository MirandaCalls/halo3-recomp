#!/usr/bin/env bash
# autoport: Xbox 360 ISO -> native Linux/macOS build (ReXGlue SDK), no manual config.
#
#   autoport/port.sh <game.iso> <workdir> <project_name>
#
# Steps:
#   1. extract ISO (extract-xiso)
#   2. rexglue init (+ DLL modules), fix host path case for DLLs
#   3. codegen loop: tail-merge chunks, flagged targets   (fix_unresolved.py)
#   4. static passes: orphan entries, XAPI fibers          (scan_orphans.py, detect_xapi.py)
#   5. cross-DLL import thunks                             (gen_xdll_imports.py)
#   6. build (Linux: Fedora container, clang + gtk3; macOS: native, Xcode clang)
#   7. headless runtime loop: missed indirect targets      (runtime_loop.py)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
AP=$ROOT/autoport
# shellcheck source=host.sh
. "$AP/host.sh"
ISO=$(abspath "$1"); WORK=$(abspath "$2"); NAME=$3
# Patched ReXGlue v0.10.0 built from ref/rexglue-sdk (fixes rexglue-sdk#456 and friends).
SDK=${AUTOPORT_SDK:-$ROOT/tools/rexglue-src}
SDK_LIB=$SDK/$LIBDIR
REXGLUE=$SDK/bin/rexglue
XISO=$ROOT/tools/extract-xiso/build/extract-xiso
PORT=$WORK/port
if [ "$HOST_OS" = macos ]; then
  BUILD=""  # native: the build commands below already run in $PORT
else
  BUILD="podman run --rm --userns=keep-id -v $ROOT:$ROOT:Z -w $PORT xenon-build"
fi

mkdir -p "$WORK"
[ -f "$WORK/game/default.xex" ] || "$XISO" -x "$ISO" -d "$WORK/game"

if [ ! -f "$PORT/${NAME}_manifest.toml" ]; then
  "$REXGLUE" init --project-name "$NAME" --xex-path "$WORK/game/default.xex" \
    --game-root "$WORK/game" --scan-dll --project-root "$PORT"
  # `init --scan-dll` lowercases DLL paths; restore on-disk case (Linux, and some macOS
  # volumes, are case-sensitive).
  python3 - "$PORT/${NAME}_manifest.toml" "$WORK/game" <<'EOF'
import re, sys, os
manifest, game = sys.argv[1], sys.argv[2]
real = {os.path.relpath(os.path.join(d, f), game).lower(): os.path.relpath(os.path.join(d, f), game)
        for d, _, fs in os.walk(game) for f in fs}
text = open(manifest).read()
def fix(m):
    rel = os.path.relpath(os.path.normpath(os.path.join(os.path.dirname(manifest), m.group(1))), game)
    fixed = real.get(rel.lower(), rel)
    return f'file_path = "{os.path.relpath(os.path.join(game, fixed), os.path.dirname(manifest))}"'
open(manifest, "w").write(re.sub(r'file_path = "([^"]+)"', fix, text))
EOF
  cat >> "$PORT/CMakeLists.txt" <<EOF

# autoport: cross-DLL import thunks
include(generated/autoport_xdll.cmake OPTIONAL)

# autoport: portable runtime fixes shared by every game (kernel export overrides)
file(GLOB AUTOPORT_RUNTIME_SOURCES CONFIGURE_DEPENDS $AP/runtime/*.cpp)
target_sources($NAME PRIVATE \${AUTOPORT_RUNTIME_SOURCES})
EOF
  # Hook the autoport runtime into the generated app (python, not sed: BSD sed on
  # macOS does not turn \n in a replacement into a newline).
  python3 - "$PORT/src/${NAME}_app.h" "$AP" <<'EOF'
import sys
path, ap = sys.argv[1], sys.argv[2]
text = open(path).read()
includes = "".join(f'\n#include "{ap}/runtime/{h}.h"'
                   for h in ("frame_capture", "cache_mounts", "disc_mount", "scripted_input"))
text = text.replace("#include <rex/rex_app.h>", "#include <rex/rex_app.h>\n" + includes, 1)
text = text.replace("  // void OnPostSetup() override {}",
                    "  void OnPostSetup() override {\n"
                    "    autoport::MountDiscImage(runtime());\n"
                    "    autoport::MountCachePartitions(runtime());\n"
                    "    autoport::StartFrameCapture(runtime());\n"
                    "    autoport::StartScriptedInput(runtime());\n"
                    "  }", 1)
open(path, "w").write(text)
EOF
fi

cd "$PORT"
M=${NAME}_manifest.toml
python3 "$AP/fix_unresolved.py" "$M" --rexglue "$REXGLUE"
python3 "$AP/scan_orphans.py" "$M"
python3 "$AP/detect_xapi.py" "$M"
python3 "$AP/fix_unresolved.py" "$M" --rexglue "$REXGLUE"
# Jump tables sized too small by the generator (then regenerate).
if python3 "$AP/fix_switch_tables.py" "$M" | grep -q " -> "; then
  python3 "$AP/fix_unresolved.py" "$M" --rexglue "$REXGLUE"
fi
python3 "$AP/gen_xdll_imports.py" "$M" --log codegen_auto.log
python3 "$AP/patch_spin_hints.py" "$M"

OSX_ARCH=()
if [ "$HOST_OS" = macos ]; then
  OSX_ARCH=(-DCMAKE_OSX_ARCHITECTURES="$(uname -m)")
fi
[ -f "out/build/$BUILD_DIR_NAME/build.ninja" ] || $BUILD cmake -S . -B "out/build/$BUILD_DIR_NAME" -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SDK" "${OSX_ARCH[@]}"
$BUILD cmake --build "out/build/$BUILD_DIR_NAME" -j"$NPROC"

B=$PORT/out/build/$BUILD_DIR_NAME
ln -sf "$SDK_LIB/librexgpu-xenos.$DLEXT" "$B/"
# SDK dlopen()s guest-DLL modules by bare target name (found through $LIBPATH_VAR).
(cd "$B" && shopt -s nullglob && for f in lib${NAME}_*.$DLEXT; do n=${f#lib}; ln -sf "$f" "${n%.$DLEXT}"; done)

python3 -u "$AP/runtime_loop.py" "$M" --rexglue "$REXGLUE" --sdk-lib "$SDK_LIB" \
  --build-dir "$B" --exe "$NAME" --seconds 90 --dlext "$DLEXT" --libpath-var "$LIBPATH_VAR" \
  --run-args "--game_data_root=$WORK/game --gpu_plugin=xenos" \
  --build-cmd "$BUILD cmake --build out/build/$BUILD_DIR_NAME -j$NPROC"

echo "Built: $B/$NAME"
echo "Run:   $LIBPATH_VAR=$B:$SDK_LIB $B/$NAME --game_data_root=$WORK/game --gpu_plugin=xenos --fullscreen=false --window_width=1280 --window_height=800"
