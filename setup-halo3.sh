#!/usr/bin/env bash
# Build Halo 3 for Linux from your own disc image.
#
#   ./setup-halo3.sh /path/to/Halo\ 3.iso
#
# Steps (all re-run safe):
#   1. build the build container            (docker/Containerfile -> xenon-build)
#   2. build extract-xiso                   (tools/extract-xiso)
#   3. clone ReXGlue v0.10.0 and apply our SDK/runtime patch
#   4. build and install the patched SDK    (tools/rexglue-resync)
#   5. port the game: ISO -> codegen -> native build (games/halo3)
#   6. install the patched runtime          (tools/rexglue-h3)
# Then: ./play-halo3.sh --name <gamertag>
set -euo pipefail

ROOT=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ISO=${1:-}
CONTAINER=xenon-build
SDK=$ROOT/tools/rexglue-resync
SRC=$ROOT/ref/rexglue-sdk
PATCH=$ROOT/autoport/sdk-patches/halo3-system-link.diff

if [ -z "$ISO" ] || [ ! -f "$ISO" ]; then
  echo "usage: $0 /path/to/Halo 3.iso" >&2
  exit 1
fi
ISO=$(realpath "$ISO")

step() { printf '\n== %s\n' "$*"; }
run() {  # run <workdir> <command...>
  local work=$1
  shift
  podman run --rm --userns=keep-id -v "$ROOT:$ROOT:Z" -w "$work" "$CONTAINER" bash -c "$*"
}

for tool in podman git python3; do
  command -v "$tool" >/dev/null || {
    echo "missing required tool: $tool" >&2
    exit 1
  }
done

step "build container ($CONTAINER)"
if ! podman image exists "$CONTAINER"; then
  podman build -t "$CONTAINER" -f "$ROOT/docker/Containerfile" "$ROOT/docker"
fi

step "extract-xiso"
if [ ! -x "$ROOT/tools/extract-xiso/build/extract-xiso" ]; then
  run "$ROOT/tools/extract-xiso" \
    'cmake -S . -B build -G Ninja >/dev/null && cmake --build build'
fi

step "ReXGlue v0.10.0 + patch"
mkdir -p "$ROOT/ref"
if [ ! -d "$SRC/.git" ]; then
  git clone --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git "$SRC"
  git -C "$SRC" submodule update --init --recursive
fi
if git -C "$SRC" apply --reverse --check "$PATCH" 2>/dev/null; then
  echo "patch already applied"
else
  git -C "$SRC" apply "$PATCH"
  echo "patch applied"
fi

step "build the patched SDK (the long step)"
if [ ! -x "$SDK/bin/rexglue" ]; then
  run "$SRC" "cmake -S . -B out/build/src-release -G Ninja \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
      -DCMAKE_CXX_STANDARD=23 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$SDK && \
    cmake --build out/build/src-release -j\$(nproc) && \
    cmake --install out/build/src-release"
fi

step "port Halo 3 from $ISO"
if [ ! -x "$ROOT/games/halo3/port/out/build/linux-release/halo3" ]; then
  AUTOPORT_SDK="$SDK" "$ROOT/autoport/port.sh" "$ISO" "$ROOT/games/halo3" halo3
fi

step "patched runtime"
mkdir -p "$ROOT/tools/rexglue-h3/lib64"
cp -f "$SDK/lib64/librexruntime.so" "$ROOT/tools/rexglue-h3/lib64/librexruntime.so"

step "done"
cat <<'EOF'
Run it with:
  ./play-halo3.sh --name <gamertag>       # system link on the LAN
  ./play-halo3.sh --zt --name <gamertag>  # system link over ZeroTier
Find other players: tools/zt-probe.py
EOF
