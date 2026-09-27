#!/usr/bin/env bash
# Boot the latest Halo: Reach port (resync SDK build).
#
# Usage: ./play-reach.sh [options] [-- extra runtime flags]
#   --fullscreen   fullscreen instead of a 1280x800 window
#   --fps          print the guest frame rate every 5 s
#   --novsync      allow immediate/mailbox present (old behaviour, uses more CPU)
#   --scale N      render at N x N internal resolution (Reach is 1152x720, so 3 = 3456x2160)
#   --pad DIR      enable the scripted pad (drive it with autoport/pad.sh DIR ...)
#   --native       render with the xenon-gpu native renderer (rexgpu-xgpu plugin)
#   -h, --help     show this help
# Anything after `--` is passed to the game unchanged.
set -euo pipefail

ROOT=/home/aaron/xenon-pc-comp
BUILD=$ROOT/games/reach-clean/port/out/build/resync
SDK_LIB=$ROOT/tools/rexglue-resync/lib64
GAME=$ROOT/games/reach-clean/game

window=(--fullscreen=false --window_width=1280 --window_height=800)
extra=()
envs=()
plugin=xenos

while [ $# -gt 0 ]; do
  case "$1" in
    --fullscreen) window=(--fullscreen=true) ;;
    --fps) envs+=(AUTOPORT_FPS=1) ;;
    --native) plugin=xgpu ;;
    --novsync) extra+=(--vulkan_allow_present_mode_immediate=true --vulkan_allow_present_mode_mailbox=true) ;;
    --scale)
      shift
      extra+=(--draw_resolution_scale_x="$1" --draw_resolution_scale_y="$1")
      ;;
    --pad)
      shift
      mkdir -p "$1"
      envs+=("AUTOPORT_INPUT_FILE=$(realpath "$1")/cmd.txt")
      ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
  esac
  shift
done

if pgrep -x halo_reach >/dev/null; then
  echo "halo_reach is already running (pid $(pgrep -x halo_reach | tr '\n' ' '))" >&2
  exit 1
fi

# Keep the host driver's compiled-pipeline cache from evicting Reach's pipelines
# (the NVIDIA default is about 1 GB shared by every application).
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-10G}

# Free guest-memory files leaked by earlier crashed runs (only ones no process has open).
"$ROOT/autoport/clean_shm.sh"

cd "$BUILD"
exec env "${envs[@]}" LD_LIBRARY_PATH="$BUILD:$SDK_LIB" ./halo_reach \
  --game_data_root="$GAME" --gpu_plugin=$plugin "${window[@]}" "${extra[@]}"
