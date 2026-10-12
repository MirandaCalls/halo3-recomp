#!/usr/bin/env bash
# Build Halo 3 for Linux or macOS from your own disc image.
#
#   ./setup-halo3.sh /path/to/Halo\ 3.iso
#
# Steps (all re-run safe):
#   1. build the build container            (Linux: docker/Containerfile -> xenon-build;
#                                            macOS builds natively with Xcode's clang)
#   2. build extract-xiso                   (tools/extract-xiso)
#   3. clone ReXGlue v0.10.0 and apply our SDK/runtime patch (+ the macOS fixes)
#   4. build and install the patched SDK    (tools/rexglue-resync)
#   5. port the game: ISO -> codegen -> native build (games/halo3; disc files in "Halo 3/")
#   6. install the patched runtime          (tools/rexglue-h3)
# Then: ./play-halo3.sh --name <gamertag>
set -euo pipefail

ROOT=$(cd "$(dirname "$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$0")")" && pwd)
# shellcheck source=autoport/host.sh
. "$ROOT/autoport/host.sh"
ISO=${1:-}
CONTAINER=xenon-build
SDK=$ROOT/tools/rexglue-resync
SRC=$ROOT/ref/rexglue-sdk
PATCH=$ROOT/autoport/sdk-patches/halo3-system-link.diff
# applied on top of PATCH: macOS build fixes (platform-guarded, harmless on Linux)
MACOS_PATCH=$ROOT/autoport/sdk-patches/macos.diff

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
  echo "usage: $0 /path/to/Halo 3.iso" >&2
  exit 1
fi
ISO=$(abspath "$ISO")

step() { printf '\n== %s\n' "$*"; }
run() { host_build "$@"; }  # run <workdir> <command...>

if [ "$HOST_OS" = macos ]; then
  required=(git python3 cmake ninja clang++ openssl)
else
  required=(podman git python3)
fi
for tool in "${required[@]}"; do
  command -v "$tool" >/dev/null || {
    echo "missing required tool: $tool" >&2
    [ "$HOST_OS" = macos ] && echo "  install Xcode, then: brew install bash cmake ninja python" >&2
    exit 1
  }
done

if [ "$HOST_OS" = linux ]; then
  step "build container ($CONTAINER)"
  if ! podman image exists "$CONTAINER"; then
    podman build -t "$CONTAINER" -f "$ROOT/docker/Containerfile" "$ROOT/docker"
  fi
fi

step "python tools (capstone)"
# The autoport scripts need python 3.11+ (tomllib) and capstone. A private venv
# avoids pip's "externally managed environment" refusal on Homebrew python.
VENV=$ROOT/tools/venv
if [ ! -x "$VENV/bin/python3" ]; then
  python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' || {
    echo "python 3.11 or newer is required ($(python3 --version)); on macOS: brew install python" >&2
    exit 1
  }
  python3 -m venv "$VENV"
fi
"$VENV/bin/python3" -c 'import capstone' 2>/dev/null || "$VENV/bin/python3" -m pip install --quiet capstone
export PATH="$VENV/bin:$PATH"

step "extract-xiso"
if [ ! -x "$ROOT/tools/extract-xiso/build/extract-xiso" ]; then
  run "$ROOT/tools/extract-xiso" \
    'cmake -S . -B build -G Ninja >/dev/null && cmake --build build'
fi

step "ReXGlue v0.10.0 + patch"
mkdir -p "$ROOT/ref"
if [ ! -d "$SRC/.git" ]; then
  git clone --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git "$SRC"
fi
# Every run, not just after the clone: resumes an interrupted submodule checkout,
# and is a quick no-op once everything is there.
git -C "$SRC" submodule update --init --recursive
# A stamp records which patch contents are applied. When either patch changes,
# re-apply both to a clean tree; submodules and the out/ build directory are kept,
# so only the files the patches touch get rebuilt.
PATCH_STAMP=.autoport-patches
want=$(cat "$PATCH" "$MACOS_PATCH" | git hash-object --stdin)
if [ "$(cat "$SRC/$PATCH_STAMP" 2>/dev/null)" = "$want" ]; then
  echo "patches already applied"
else
  git -C "$SRC" checkout -- .
  git -C "$SRC" clean -fdq -e out -e "$PATCH_STAMP"
  git -C "$SRC" apply "$PATCH"
  git -C "$SRC" apply "$MACOS_PATCH"
  echo "$want" > "$SRC/$PATCH_STAMP"
  echo "patches applied"
fi

step "build the patched SDK (the long step)"
if [ ! -x "$SDK/bin/rexglue" ]; then
  run "$SRC" "cmake -S . -B out/build/src-release -G Ninja \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ $ARCH_FLAGS \
      -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$SDK && \
    cmake --build out/build/src-release -j$NPROC && \
    cmake --install out/build/src-release"
fi

step "port Halo 3 from $ISO"
# The extracted disc lives in "Halo 3/" at the repo root (git-ignored). The pipeline
# reaches it as games/halo3/game, a symlink, so the space never hits word-split
# commands or runtime flags. A real games/halo3/game from older runs is left as is.
mkdir -p "$ROOT/Halo 3" "$ROOT/games/halo3"
[ -e "$ROOT/games/halo3/game" ] || ln -s "../../Halo 3" "$ROOT/games/halo3/game"
if [ ! -x "$ROOT/games/halo3/port/out/build/$BUILD_DIR_NAME/halo3" ]; then
  AUTOPORT_SDK="$SDK" "$ROOT/autoport/port.sh" "$ISO" "$ROOT/games/halo3" halo3
fi

step "patched runtime"
mkdir -p "$ROOT/tools/rexglue-h3/$LIBDIR"
cp -f "$SDK/$LIBDIR/librexruntime.$DLEXT" "$ROOT/tools/rexglue-h3/$LIBDIR/librexruntime.$DLEXT"

step "done"
cat <<'EOF'
Run it with:
  ./play-halo3.sh --name <gamertag>       # system link on the LAN
  ./play-halo3.sh --zt --name <gamertag>  # system link over ZeroTier
Find other players: tools/zt-probe.py
EOF
