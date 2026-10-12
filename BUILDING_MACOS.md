# Building on macOS

This fork targets **ReXGlue SDK v0.10.0**, the first SDK release with macOS support
(Vulkan through MoltenVK). Upstream targets SDK 0.5.0, which has no macOS support.

## Requirements

- macOS on Apple Silicon (arm64) or Intel (x86_64)
- Xcode Command Line Tools (`xcode-select --install`)
- CMake 3.25+ and Ninja (`brew install cmake ninja`)
- Your own copy of `halo3_cache_release.xex` from
  `Halo 3 08172.07.03.08.2240.delta (March 8 2007)` (SHA256 in
  `halo3_cache_release_config.toml`) and that build's game data (maps etc.)

## Steps

```sh
# 1. SDK, next to this repo's sources
git clone --recursive --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git thirdparty/rexglue-sdk

# 2. The game executable, where the manifest expects it
mkdir -p ../assets && cp /path/to/halo3_cache_release.xex ../assets/

# 3. Configure and build (codegen runs automatically as part of the build)
cmake --preset mac-arm64-release -DREXSDK_DIR=thirdparty/rexglue-sdk   # Intel: mac-amd64-release
cmake --build --preset mac-arm64-release
```

To run codegen by hand instead, put the SDK's `rexglue` tool on your `PATH` and run
`./ProjectCodegen.sh`.

## Running

When no debugger is attached the game uses the current directory as its game, user and
update data root, so start it from the folder that holds `halo3_cache_release.xex` and the
game data.

## What changed from upstream

- **SDK 0.10 migration**: added `halo3_cache_release_manifest.toml` (the existing config is
  pulled in via `includes`), regenerated `generated/rexglue.cmake`, and moved
  `rex_macros.h` and the hand-written hooks to the 0.10 API (`REX_HOOK`, `REX_EXTERN`,
  `DEFINE_REX_FUNC`, `rex::ppc::GuestToHostFunction`, `u32`/`mapped_string`).
- **64-bit `long`**: `long` is 32-bit on Windows but 64-bit on macOS/Linux, which broke every
  struct that mirrors guest memory. All 32-bit `long`/`unsigned long` became
  `int32_t`/`uint32_t`, and guest UTF-16 `wchar_t` became `char16_t`.
- **MSVC-isms**: `__pragma` became `_Pragma`, `__forceinline` and `_inline` became `inline`,
  forward-declared enums now have an `int32_t` underlying type, and there are stand-ins for
  `strncpy_s`, `vsnprintf_s` and `GetTickCount`. Removed `<vadefs.h>`/`<vcruntime.h>`, fixed
  a backslash `#include`.
- **Input**: off Windows, gamepads are read through the SDK's input system (SDL) instead of
  host XInput. The Win32 raw-input keyboard/mouse path (`enable_pc_joystick`) is Windows-only;
  on macOS use the SDK's `mnk_mode` cvar instead.
- **Networking**: the host Winsock passthrough hooks are Windows-only, so on macOS the guest
  keeps using the SDK's kernel networking.

## Known limitations

- This has only been syntax-checked against the SDK 0.10 headers. Nobody has built or run
  it on a Mac yet, so expect some further fixes once codegen and linking run for real.
- Performance on Apple Silicon is unknown. Other ReXGlue ports report correct rendering but
  low frame rates on M1.
