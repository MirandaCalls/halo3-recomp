#!/usr/bin/env bash
# Boot the latest Halo 3 port (resync SDK build).
#
# Usage: ./play-halo3.sh [options] [-- extra runtime flags]
#   --fullscreen   fullscreen instead of a 1280x800 window (the mouse cursor auto-hides)
#   --size WxH     window size instead of 1280x800
#   --keyboard     play with keyboard and mouse (SDK mnk_mode, mouse look)
#   --fps          print the guest frame rate every 5 s (no effect yet: nothing reads
#                  AUTOPORT_FPS; use AUTOPORT_STUTTER_LOG + tools/stutter-summary.awk)
#   --vsync        present with vsync: no tearing, but noticeably more input lag
#   --novsync      present without vsync (the default: immediate mode, can tear)
#   --metal-capture  macOS: `touch metal-captures/capture` while playing saves the
#                  next 3 frames as metal-captures/frame-<n>.gputrace (open in Xcode)
#   --scale N      render at N x N internal resolution (Halo 3 is 1152x640)
#   --pad DIR      enable the scripted pad (drive it with autoport/pad.sh DIR ...)
#   --instance N   system link on one PC: instance N (1..16) uses IP 127.0.0.<N+1>,
#                  gamertag PlayerN and its own saves/caches (games/halo3/instances/N);
#                  several instances may run at once
#   --name NAME    gamertag; across PCs each system link player needs its own
#                  (without --instance the game uses this PC's LAN address)
#   --zt           system link over ZeroTier: advertise this PC's ZeroTier IPv4
#                  address (one game per PC; the LAN address is not advertised)
#   --ip ADDR      system link using this IPv4 address instead of the default NIC
#   -h, --help     show this help
# Anything after `--` is passed to the game unchanged.
set -euo pipefail

ROOT=$(cd "$(dirname "$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$0")")" && pwd)
# shellcheck source=autoport/host.sh
. "$ROOT/autoport/host.sh"
BUILD=$ROOT/games/halo3/port/out/build/$BUILD_DIR_NAME
SDK_LIB=$ROOT/tools/rexglue-resync/$LIBDIR
# Halo 3 runtime fixes built from the ref/rexglue-h3 worktree (branch halo3); only
# librexruntime is replaced, everything else comes from the resync SDK.
H3_LIB=$ROOT/tools/rexglue-h3/$LIBDIR
GAME=$ROOT/games/halo3/game

window=(--fullscreen=false --window_width=1280 --window_height=800)
extra=()
envs=()
plugin=xenos
instance=
explicit_ip=

while [ $# -gt 0 ]; do
  case "$1" in
    --fullscreen) window=(--fullscreen=true) ;;
    --size)
      shift
      if ! [[ $1 =~ ^([0-9]+)x([0-9]+)$ ]]; then
        echo "--size wants WIDTHxHEIGHT, e.g. 640x400" >&2
        exit 1
      fi
      window=(--fullscreen=false --window_width="${BASH_REMATCH[1]}" --window_height="${BASH_REMATCH[2]}")
      ;;
    --keyboard) extra+=(--mnk_mode=true --mnk_mouse=true) ;;
    --fps) envs+=(AUTOPORT_FPS=1) ;;
    --metal-capture)
      mkdir -p "$ROOT/metal-captures"
      envs+=(MTL_CAPTURE_ENABLED=1 AUTOPORT_METAL_CAPTURE="$ROOT/metal-captures")
      ;;
    --instance)
      shift
      instance=$1
      ;;
    --name)
      shift
      extra+=(--user_name="$1")
      ;;
    --ip)
      shift
      explicit_ip=$1
      extra+=(--net_ip="$1")
      ;;
    --zt)
      if [ "$HOST_OS" = macos ]; then
        # ZeroTier's macOS interfaces are feth<N>
        explicit_ip=$(ifconfig 2>/dev/null |
          awk '/^[a-z]/ { iface = $1 } iface ~ /^(feth|zt)/ && $1 == "inet" { print $2; exit }')
      else
        explicit_ip=$(ip -4 -o addr show 2>/dev/null |
          awk '$2 ~ /^zt/ { sub(/\/.*/, "", $4); print $4; exit }')
      fi
      if [ -z "$explicit_ip" ]; then
        echo "--zt: no ZeroTier interface with an IPv4 address (is ZeroTier joined?)" >&2
        exit 1
      fi
      echo "ZeroTier system link address: $explicit_ip"
      extra+=(--net_ip="$explicit_ip")
      ;;
    --novsync) extra+=(--vulkan_allow_present_mode_immediate=true --vulkan_allow_present_mode_mailbox=true) ;;
    --vsync) extra+=(--vulkan_allow_present_mode_immediate=false --vulkan_allow_present_mode_mailbox=false --vulkan_allow_present_mode_fifo_relaxed=false) ;;
    --scale)
      shift
      extra+=(--draw_resolution_scale_x="$1" --draw_resolution_scale_y="$1")
      ;;
    --pad)
      shift
      mkdir -p "$1"
      envs+=("AUTOPORT_INPUT_FILE=$(abspath "$1")/cmd.txt")
      ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
  esac
  shift
done

# Window flags given after `--` replace the defaults: the game rejects a flag
# given twice.
for arg in "${extra[@]}"; do
  case "$arg" in --fullscreen=*|--window_width=*|--window_height=*) window=() ;; esac
done

# Halo 3 sets its exposure from a resolved image it reads back on the CPU. With
# readback off (the SDK default) the campaign renders almost black on macOS.
# Untested on Linux, so left as is there. A value given after `--` wins.
game_defaults=()
if [ "$HOST_OS" = macos ]; then
  game_defaults=(--readback_resolve=fast)
fi
for arg in "${extra[@]}"; do
  case "$arg" in --readback_resolve=*) game_defaults=() ;; esac
done

if [ -n "$instance" ] && [ -n "$explicit_ip" ]; then
  echo "--instance cannot be combined with --zt/--ip" >&2
  exit 1
fi

if [ -n "$instance" ]; then
  if ! [[ $instance =~ ^([1-9]|1[0-6])$ ]]; then
    echo "--instance must be 1..16" >&2
    exit 1
  fi
  data=$ROOT/games/halo3/instances/$instance
  mkdir -p "$data"
  extra=(--net_ip=127.0.0.$((instance + 1)) --user_name=Player$instance
         --user_data_root="$data" --cache_root="$data/cache" --log_file="$data/halo3.log" --log_max_file_size_mb=200
         "${extra[@]}")
elif pgrep -x halo3 >/dev/null; then
  echo "halo3 is already running (pid $(pgrep -x halo3 | tr '\n' ' '))" >&2
  exit 1
fi

# Keep the host driver's compiled-pipeline cache from evicting the game's pipelines
# (the NVIDIA default is about 1 GB shared by every application).
export __GL_SHADER_DISK_CACHE_SIZE=${__GL_SHADER_DISK_CACHE_SIZE:-10737418240}
export __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=${__GL_SHADER_DISK_CACHE_SKIP_CLEANUP:-1}
export MESA_SHADER_CACHE_MAX_SIZE=${MESA_SHADER_CACHE_MAX_SIZE:-10G}

# Free guest-memory files leaked by earlier crashed runs (only ones no process has open).
# Linux only: macOS has no /dev/shm to inspect.
if [ "$HOST_OS" = linux ]; then
  "$ROOT/autoport/clean_shm.sh"
fi

cd "$BUILD"
# The GPU plugin lives in the SDK. Relink it if this tree was copied from
# another machine/path (the symlink port.sh made is absolute).
gpu_plugin=librexgpu-xenos.$DLEXT
if [ -e "$SDK_LIB/$gpu_plugin" ] && [ ! -e "$gpu_plugin" ]; then
  ln -sfn "$SDK_LIB/$gpu_plugin" "$gpu_plugin"
fi
exec env "${envs[@]}" "$LIBPATH_VAR=$BUILD:$H3_LIB:$SDK_LIB" ./halo3 \
  --game_data_root="$GAME" --gpu_plugin=$plugin "${window[@]}" "${game_defaults[@]}" "${extra[@]}"
