# halo3-recomp

A native Linux and macOS port of Halo 3. It recompiles your own Halo 3 (Xbox 360)
disc image into a native binary and plays it with working **system link (LAN)
multiplayer**, including between machines that only share a **ZeroTier**
network.

This project is a fork of
[summers-aaron/xenon-pc-comp](https://github.com/summers-aaron/xenon-pc-comp),
which built Halo 3 for Linux. This fork adds the macOS port (Apple Silicon,
through MoltenVK) along with the rendering fixes and performance work it
needed. Most of those changes are macOS-only defaults; the renderer fixes
that apply everywhere (tessellation, memexport barriers) haven't been
retested on Linux yet.

Built on [ReXGlue](https://github.com/rexglue/rexglue-sdk), the open-source
Xbox 360 static-recompilation toolchain. This repository is the whole recipe:
the ISO → native pipeline (`autoport/`), the ReXGlue changes that make Halo 3
(and specifically system link) work on Linux and macOS, and the launcher.

## Requirements

### Linux

- Linux x86-64 (developed on Fedora 44), a Vulkan-capable GPU
- `podman`, `git`, `python3`
- ~40 GB free disk
- Your own Halo 3 disc image (ISO); it stays on your machine
- For internet play: both machines on the same ZeroTier network

### macOS

- macOS on Apple Silicon (tested on an M3 MacBook Air;
  Intel Macs are untested), with Xcode installed (the SDK builds MoltenVK
  from source, which needs the full Xcode, not just the command-line tools)
- Homebrew: `brew install bash cmake ninja python` (the scripts need bash 4.4+,
  which macOS lacks, and python 3.11+; setup installs `capstone` into a private
  venv under `tools/venv`)
- ~40 GB free disk

On macOS everything builds natively with Xcode's clang instead of in the
container; Vulkan runs through MoltenVK, which the SDK bundles. The same
`./setup-halo3.sh` and `./play-halo3.sh` commands work. See "macOS port notes"
below for what differs and the switches for diagnosing rendering issues.

## Build

```bash
git clone https://github.com/MirandaCalls/halo3-recomp.git
cd halo3-recomp
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

On macOS (M3 MacBook Air) the campaign renders correctly, including the
tessellated water, at roughly 25–30 fps; the frame rate is still being worked
on (see "Measuring GPU time per render pass").

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
- With guest memory imported, a resolve only lands in guest memory when the
  host GPU runs it, while the command stream's signals to the guest (memory
  and event writes, interrupts) are delivered when the command is processed,
  so the CPU can read a resolve before it's written. Halo 3 reads a small
  luminance resolve for its exposure; when the GPU lagged (heavy plasma
  effects), the whole frame flashed. For resolves up to
  `AUTOPORT_RESOLVE_SYNC_MAX_BYTES` (64 KB by default on macOS; `0` turns it
  off), `AUTOPORT_RESOLVE_SYNC` picks what the CPU sees:
  - `signal` (default): this frame's, by holding the guest's next signals
    until the GPU has run the resolve, as on the 360. The game then waits for
    the GPU every frame, which costs while the GPU can't keep up: 25.6 -> 18.7
    fps at Sierra 117.
  - `previous`: the previous frame's completed resolve, copied into guest
    memory when the resolve is processed (the delayed readback that
    `--readback_resolve=fast` does without import). 27.7 fps, but the flashes
    stay.
  - `immediate`: this frame's, by waiting for the GPU right after the resolve.
    The GPU idles mid-frame and Apple GPUs clock down: 14.4 fps.

  The log lists each distinct resolve once ("autoport: resolve to ...", with
  ", synchronized for the CPU" on the small ones).
- If the import fails (the log lacks "Shared memory: using guest physical
  memory directly"), the normal buffer is used, and `play-halo3.sh`'s default
  `--readback_resolve=fast` keeps the lighting right. Without either, the
  campaign renders almost black (the menu looks normal).
- Vertex shaders mark their position output invariant where SPIRV-Cross reads
  it (on the `gl_Position` member), so MoltenVK compiles them with Metal's
  `preserveInvariance`. Halo 3 draws objects twice, the second (lighting) pass
  with an "equal" depth test; without invariance, the two passes' positions
  for wind-animated foliage differed slightly on some pixels, which kept the
  first pass's unlit color (flickering green dots on ferns).
- `AUTOPORT_DRAW_LOG=<file>` records the state of every draw of two frames
  once `<file>.trigger` exists; `tools/draw-log-pairs.py <file>` lines up the
  passes of multipass draws (same index and vertex buffers, different shaders)
  and marks the state that differs between them:

  ```sh
  AUTOPORT_DRAW_LOG=$PWD/draws.log ./play-halo3.sh --name <gamertag>
  touch draws.log.trigger        # while looking at the artifact
  tools/draw-log-pairs.py draws.log > pairs.txt
  ```
- `AUTOPORT_PASS2_DEPTH_BIAS=<units>` biases draws that depth-test without
  writing depth (Halo 3's lighting pass: "greater or equal" against the albedo
  pass's depth, with a different vertex shader) toward passing, by that many
  depth units.
- `AUTOPORT_PASS2_ALPHA_EPSILON=<e>` lowers the alpha test cutoff by `e` for
  draws that depth-test against an earlier pass without writing depth (0.35 by
  default on macOS, `0` turns it off). Halo 3 alpha-tests foliage at 0.502 in
  both its albedo and lighting passes with the same texture and settings, but on
  Apple GPUs the lighting pass can pick a smaller mip at leaf edges (forcing the
  most detailed mip with `AUTOPORT_TEXTURE_LOD0=1` removes the effect), so its
  alpha drops below the cutoff and the unlit albedo stays: flickering green dots
  on ferns. The lower cutoff hides most of them; the root cause (how the
  lighting pass's texture coordinate derivatives come out) is still open.
- Guest draws track the shared memory ranges they read (vertex and index
  buffers) and memexport, and only get a barrier when a later draw reads or
  overwrites memory an earlier one wrote or read since the last barrier. The
  SDK inserted one after every memexport draw, which ended a render pass each
  time (~110 per frame in Halo 3). `AUTOPORT_MEMEXPORT_BARRIERS=always` restores
  that. `AUTOPORT_MEMEXPORT_BARRIERS=never` drops the remaining ones, which
  each split a render pass, to measure what they cost; draws may then read
  stale memexported data (particles), so it's a measurement, not a fix. Apple
  GPUs have no barriers inside a render pass (MoltenVK ignores them there).
- `AUTOPORT_MEMEXPORT_COMPUTE=1` (experimental, with guest memory imported)
  runs draws that only memexport from their vertex shader (nothing
  rasterized) as compute dispatches recorded in front of the render pass
  that's open, instead of drawing them inside it. Halo 3 updates each particle
  system with such a draw and then draws the particles from the result, and
  since Apple GPUs have no barriers inside a render pass, the pass had to end
  between the two: ~110 times a frame, each storing and reloading the render
  targets (about 10 ms of GPU time per frame at Sierra 117). A dispatch that
  touches memory the pass's earlier draws access ends the pass instead. Every
  300 frames the log says how many went in front of a pass, outside one, ended
  one, or were drawn because the compute pipeline couldn't be created.
  `AUTOPORT_MEMEXPORT_COMPUTE=inplace` runs the dispatches where the draws were
  (ending the pass), to tell translation problems from ordering problems;
  `=emptypass` only moves them in front of a pass with no draws yet.
  `AUTOPORT_MEMEXPORT_COMPUTE_LOG=1` logs, for the first 40 passes with
  dispatches moved past draws and a tessellated draw (water), the pass's draws (shaders, memory read and
  written) and the moved dispatches in order.
  `AUTOPORT_MEMEXPORT_COMPUTE_SKIP=<hash>[,<hash>...]` keeps those vertex
  shaders on the drawing path, to find one that computes differently (the log
  names each converted shader: "vertex shader ... memexports from a compute
  pipeline").
- `AUTOPORT_TEXTURE_STATS=1` logs, every 300 frames, texture loads per frame
  (first loads and reloads), how many texture invalidations came from CPU and
  GPU writes, and the most reloaded textures.
- On macOS, CPU writes invalidate only the guest pages written, instead of up to
  64 pages around them (1 MB with macOS's 16 KB pages). At Sierra 117 this took
  the frame rate from 9.9 to 13.1 fps. `AUTOPORT_INVALIDATION_WIDEN=1` restores
  the widening (`=0` turns it off on other platforms).
- `AUTOPORT_TEXTURE_LOD0=1` samples 2D textures with zero gradients, i.e. the
  most detailed mip (diagnostic; distant textures shimmer).
- `AUTOPORT_TEXTURE_GAMMA=off` treats gamma texture components as plain
  (diagnostic; colors wash out).
- `AUTOPORT_EQUAL_DEPTH=lequal|gequal` relaxes "equal" depth tests, and
  `AUTOPORT_EQUAL_DEPTH=noalphatest` drops the alpha test of draws with an equal,
  "less or equal" or "greater or equal" depth test and no depth write (diagnostics for pixels a multipass lighting
  pass misses).
- `AUTOPORT_PS_NAN=show` paints pixels whose pixel shader output is NaN or
  infinite magenta; `AUTOPORT_PS_NAN=zero` replaces those values with 0
  (diagnostics for single-pixel artifacts).
- `AUTOPORT_FLOAT_RT_CLAMP` clamps colors written to float render targets on
  the host render target path, and turns NaN into 0 (Xenos render targets have
  no NaN). `k_2_10_10_10_FLOAT` targets are clamped to the guest format's range
  (0 to 31.875, alpha 0 to 1); the host stores them as 16-bit float, which keeps
  negative and larger values. `k_16_16_FLOAT` and `k_16_16_16_16_FLOAT` targets
  are clamped to ±65504: Xenos 16-bit floats reach 131008 with no infinity,
  but host ones overflow to infinity above 65504. Without this, plasma hitting
  the player (or a Phantom firing) overflows Halo 3's bloom, and the NaN that
  follows flashes the whole frame purple, blue or rainbow. On by default on
  macOS; `AUTOPORT_FLOAT_RT_CLAMP=0` turns it off, `=1` turns it on elsewhere.
- `AUTOPORT_TEXTURE_SANITIZE=1` replaces infinite texture fetch results with
  ±65504 and NaN with 0. Xenos 16-bit floats have no infinity or NaN (exponent
  31 holds 65536 to 131008), but 16-bit float textures are copied to the host
  bit for bit, so those values sample as infinity or NaN (a candidate for the
  frame-wide flashes when plasma hits the player).
- `AUTOPORT_FLUSH_DENORMALS=1` treats denormal floats as zero where Xenos does:
  in the "0 × anything = 0" rule of multiplications and dot products, and in
  the inputs of `rcp`, `rsq` and `log`. Halo 3's tone mapping pass
  (`373E65D9ADCF4380`) divides by the pixel's brightness, so a denormal there
  would overflow to infinity instead of giving black.
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
shader debugging. With `--metal-capture`, each guest draw is wrapped in a debug
group named after its shaders (`vs <hash> ps <hash>`), so a draw can be found
in Xcode by typing a shader hash into the navigator's filter.

`AUTOPORT_SKIP_SHADERS=<hash>[,<hash>...]` drops every draw that uses one of those
vertex or pixel shaders, to see what an expensive shader draws and how much GPU
time skipping it saves. It's a diagnostic: the skipped effect is missing.

To find the shader behind an artifact, bisect live: run the game with
`AUTOPORT_SKIP_SHADERS_FILE` and, standing where the artifact shows,
`tools/shader-bisect.py` in a second terminal. It skips groups of the shaders
on screen and asks whether the artifact is still there; when a group breaks the
whole view (lighting, post-processing), answer `b` and it narrows down and sets
aside the shaders responsible:

```sh
AUTOPORT_SKIP_SHADERS_FILE=$PWD/shader-skip.txt ./play-halo3.sh --name <gamertag>
tools/shader-bisect.py shader-skip.txt
```

## Legal

No game code or data is included. `setup-halo3.sh` reads your own disc image;
the resulting binary is for your own use. ReXGlue and its dependencies are
BSD/MIT-licensed; `tools/extract-xiso` ships under its own license. Not
affiliated with Microsoft, Bungie or 343 Industries.
