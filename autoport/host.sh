# Host platform settings shared by setup-halo3.sh, autoport/port.sh and play-halo3.sh.
# Source it; it defines:
#   HOST_OS          linux | macos
#   DLEXT            shared library extension (so | dylib)
#   LIBDIR           SDK install library directory (lib64 on Fedora, lib on macOS)
#   LIBPATH_VAR      dynamic loader search path variable
#   BUILD_DIR_NAME   CMake build directory name under out/build
#   NPROC            parallel build jobs
#   ARCH_FLAGS       extra CMake arguments for the target CPU
# and the helpers abspath and host_build.

# macOS ships bash 3.2, which treats an empty "${array[@]}" as unbound under set -u
if [ "${BASH_VERSINFO[0]}" -lt 4 ] || { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 4 ]; }; then
  echo "bash 4.4 or newer is required (this is $BASH_VERSION); on macOS: brew install bash" >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    HOST_OS=macos
    DLEXT=dylib
    LIBDIR=lib
    LIBPATH_VAR=DYLD_LIBRARY_PATH
    BUILD_DIR_NAME=macos-release
    NPROC=$(sysctl -n hw.ncpu)
    case "$(uname -m)" in
      arm64) ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_C_FLAGS=-march=armv8-a -DCMAKE_CXX_FLAGS=-march=armv8-a" ;;
      *) ARCH_FLAGS="-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2" ;;
    esac
    ;;
  *)
    HOST_OS=linux
    DLEXT=so
    LIBDIR=lib64
    LIBPATH_VAR=LD_LIBRARY_PATH
    BUILD_DIR_NAME=linux-release
    NPROC=$(nproc)
    ARCH_FLAGS="-DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
    ;;
esac

# realpath -m (GNU only): absolute path, the target need not exist
abspath() { python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$1"; }

# host_build <workdir> <command...>: run a build command. Linux builds inside the
# Fedora build container (xenon-build); macOS builds natively with Xcode's clang.
host_build() {
  local work=$1
  shift
  if [ "$HOST_OS" = macos ]; then
    (cd "$work" && bash -c "$*")
  else
    podman run --rm --userns=keep-id -v "$ROOT:$ROOT:Z" -w "$work" xenon-build bash -c "$*"
  fi
}
