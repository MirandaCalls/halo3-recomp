# Halo 3, native on Linux and macOS — with ZeroTier system link

Recompiles your own Halo 3 (Xbox 360) disc image into a native Linux or macOS binary and
plays it with working **system link (LAN) multiplayer**, including between
machines that only share a **ZeroTier** network.

Built on [ReXGlue](https://github.com/rexglue/rexglue-sdk), the open-source
Xbox 360 static-recompilation toolchain. This repository is the whole recipe:
the ISO → native pipeline (`autoport/`), the ReXGlue changes that make Halo 3
(and specifically system link) work on Linux, and the launcher.

## Requirements

- Linux x86-64 (developed on Fedora 44), a Vulkan-capable GPU
- `podman`, `git`, `python3`
- ~40 GB free disk
- Your own Halo 3 disc image (ISO); it stays on your machine
- For internet play: both machines on the same ZeroTier network

### macOS (experimental, untested)

- macOS on Apple Silicon or Intel, with Xcode installed (the SDK builds MoltenVK
  from source, which needs the full Xcode, not just the command-line tools)
- Homebrew: `brew install bash cmake ninja python` (the scripts need bash 4.4+,
  which macOS lacks, and python 3.11+; setup installs `capstone` into a private
  venv under `tools/venv`)
- ~40 GB free disk

On macOS everything builds natively with Xcode's clang instead of in the
container; Vulkan runs through MoltenVK, which the SDK bundles. The same
`./setup-halo3.sh` and `./play-halo3.sh` commands work. This port has not been
built or run on a Mac yet, so expect to fix build errors along the way; see
"macOS port notes" below.

## Build

```bash
git clone <this repo> xenon-pc-comp
cd xenon-pc-comp
./setup-halo3.sh "/path/to/Halo 3.iso"
```

The first run is long (the patched ReXGlue SDK is built from source, then the
game is recompiled). It is re-run safe: finished steps are skipped.

The disc is extracted to `Halo 3/` in the repo (git-ignored). If you already have
the extracted files, put them there and the extraction step is skipped.

## Play

```bash
./play-halo3.sh --name YourName                  # system link on the LAN
./play-halo3.sh --zt --name YourName             # system link over ZeroTier
```

Every player needs a different `--name` (the XUID is derived from it). In game:
**Multiplayer → System Link**, one player hosts/creates the game, the others
join. While a game is running, `tools/zt-probe.py` lists every Halo 3 it can
see on the ZeroTier link (they all answer the probe on UDP 37023).

Useful launcher flags: `--instance N` (several instances on one PC),
`--ip ADDR` (pick the system-link interface), `--native` (experimental native
renderer), `--fps`, `--scale N`, `--help`.

If the game aborts with "Call to invalid or unregistered function" somewhere the
setup's automatic runs never reached (it only plays the boot and menu), play to
that point with a log, then add the missing functions and rebuild:

```bash
./play-halo3.sh --name YourName -- --log_file="$PWD/halo3.log" --log_max_file_size_mb=200
tools/add-missed-functions.sh halo3.log
```

Repeat until it gets past that point; each crash names one more function.

## ZeroTier notes

Both machines only need to join the same ZeroTier network — no port
forwarding. `--zt` advertises this machine's ZeroTier address and, thanks to the
runtime patch, sends system-link discovery broadcasts out the ZeroTier
interface instead of the default route. If a machine's ZeroTier address is
managed and reachable (`zerotier-cli listnetworks` says `OK`), the games find
each other.

## What's in here

| Path | What |
| --- | --- |
| `setup-halo3.sh` | One-command build: SDK patching + ISO port |
| `autoport/` | ISO → native pipeline (codegen fixups, runtime hooks, build) |
| `autoport/sdk-patches/halo3-system-link.diff` | Everything changed in ReXGlue: Canary GPU re-sync, Halo 3 fixes, system link |
| `play-halo3.sh` | Launcher |
| `tools/zt-probe.py` | Find running Halo 3s on the ZeroTier/LAN link |
| `xenon-gpu/` | Experimental native renderer (`--native`); optional |
| `docker/Containerfile` | Build container (clang, GTK3, shader tools) |

Status: Halo 3 boots to the menu, campaign loads (Arrival → Sierra 117 and
beyond), and system link multiplayer works between instances/machines. The
`--native` renderer is experimental; the default emulated Xenos renderer is the
well-tested path.

## Notes

- Guest ports below 1024 are mapped up by `net_port_offset` (default 36000).
  Every machine in a system link must use the same offset.
- `--instance 1..16` gives each local instance its own `127.0.0.N`, saves and
  cache for system link on a single PC.
- Kernel/GPU details and the session history are in `autoport/` comments.

## macOS port notes

What differs from the Linux path:

- `autoport/host.sh` picks per-platform settings: `.dylib`/`lib` instead of
  `.so`/`lib64`, `DYLD_LIBRARY_PATH`, a native build instead of podman, and the
  CPU architecture flags.
- `autoport/sdk-patches/macos.diff` is applied on top of
  `halo3-system-link.diff`. It replaces the Linux-only `process_vm_readv` (GPU
  capture memory dump) with `mach_vm_read_overwrite`, makes the native
  renderer's `--no-undefined` link flag ld64-compatible, and compiles out the
  RenderDoc frame-capture hooks (`AUTOPORT_RDOC_*`), whose header has no macOS
  support. It also fixes a race in the SDK's `XThread` startup, where a new thread
  and its creator could write the thread's name at once and corrupt the heap
  (macOS aborts with "pointer being freed was not allocated").
  It turns off 16-bit gamma render targets by default on macOS
  (`gamma_render_target_as_unorm16`), which cost about 4 ms of GPU time per frame
  on an M3 with no visible difference. Finally, it adds per-render-pass GPU timing
  (see below).
- The runtime fix-up loop (`autoport/runtime_loop.py`) opens a real window on
  macOS rather than SDL's offscreen driver.
- On macOS the GPU uses the guest's physical memory directly
  (`--vulkan_shared_memory_host_import`, on by default there) through
  `VK_EXT_external_memory_host`, so guest data needs no uploads and GPU writes
  land in guest memory. Halo 3 sets its exposure from a resolved image it reads
  on the CPU, and this gives it the current frame's image with no readback.
  Resolve and memexport readbacks are skipped while it is on, so
  `--readback_resolve` has no effect. GPU time is the same as without it.
- If the import fails (the log lacks "Shared memory: using guest physical
  memory directly"), the normal buffer is used, and `play-halo3.sh`'s default
  `--readback_resolve=fast` keeps the lighting right. Without either, the
  campaign renders almost black (the menu looks normal).
- `AUTOPORT_PS_NAN=show` paints pixels whose pixel shader output is NaN or
  infinite magenta; `AUTOPORT_PS_NAN=zero` replaces those values with 0
  (diagnostics for single-pixel artifacts).
- `--zt` finds the ZeroTier address on the `feth*` interfaces macOS uses.
- `autoport/clean_shm.sh` is skipped (no `/dev/shm` on macOS).
- MoltenVK has no `VK_EXT_graphics_pipeline_library`, so the patch's pipeline
  library path is disabled automatically and the standard pipeline path is used.

### Measuring GPU time per render pass

`AUTOPORT_GPU_TIMING_LOG=<file>` writes one line per render pass with its GPU
time, measured with Vulkan timestamp queries and read back without stalling. Each
line records the pass's kind (the game's draws, an EDRAM transfer between render
targets, or the copy to the window), its size, its guest draw count and its render
target formats, plus the GPU time since the previous pass and what was recorded
in that gap (texture loads, resolves, uploads, barriers and so on).
`tools/gpu-timing-summary.awk` ranks the passes and the gaps by GPU time per
frame:

```sh
AUTOPORT_GPU_TIMING_LOG=$PWD/gpu-timing.log ./play-halo3.sh --name <gamertag>
awk -v from=25 -f tools/gpu-timing-summary.awk gpu-timing.log   # skip the boot logos
```

Adding `AUTOPORT_GPU_TIMING_PER_DRAW=1` gives every guest draw its own render pass
and labels it with its vertex and pixel shader hashes (as in `--dump_shaders` file
names), and the summary then ranks pixel shaders by GPU time. The extra passes
inflate the totals, so use it to rank draws, not to measure the frame rate.

`./play-halo3.sh --metal-capture` captures frames without a debugger (Xcode's
debugger stops on the faults the runtime uses to watch GPU memory). While playing,
`touch metal-captures/capture` saves the next 3 frames as
`metal-captures/frame-<n>.gputrace`, which Xcode opens with pixel history and
shader debugging.

`AUTOPORT_SKIP_SHADERS=<hash>[,<hash>...]` drops every draw that uses one of those
vertex or pixel shaders, to see what an expensive shader draws and how much GPU
time skipping it saves. It's a diagnostic: the skipped effect is missing.

To find the shader behind an artifact, bisect live: run the game with
`AUTOPORT_SKIP_SHADERS_FILE` and, standing where the artifact shows,
`tools/shader-bisect.py` in a second terminal. It skips half of the shaders on
screen at a time and asks whether the artifact is still there:

```sh
AUTOPORT_SKIP_SHADERS_FILE=$PWD/shader-skip.txt ./play-halo3.sh --name <gamertag>
tools/shader-bisect.py shader-skip.txt
```

## Legal

No game code or data is included. `setup-halo3.sh` reads your own disc image;
the resulting binary is for your own use. ReXGlue and its dependencies are
BSD/MIT-licensed; `tools/extract-xiso` ships under its own license. Not
affiliated with Microsoft, Bungie or 343 Industries.
