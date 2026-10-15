# A native Metal renderer: plan

Status: proposal. Nothing here is implemented yet.

## Why

On macOS the game's frames currently go through three translations:

```
Xbox 360 GPU commands → SDK GPU emulator (rexgpu-xenos, Vulkan backend)
                      → Vulkan → MoltenVK → Metal → Apple GPU
```

Measured on an M3 (Halo 3, 3D menu and Sierra 117), the GPU is the bottleneck
(99.9% busy), not the CPU or MoltenVK's call translation. The GPU time goes to:

- the game's own 4x MSAA scene passes (~30 ms/frame on the menu),
- one particle shader (~15 ms/frame on the menu, `43A59E77A4B66B05`),
- EDRAM emulation: ~50 render target transfer passes and ~110 drawing passes per
  frame, each of which makes a tile-based GPU store and reload whole render
  targets, plus ~150 small barrier/upload/resolve gaps (~13 ms/frame).

A Metal backend does not make the game's own shading cheaper. What it can do,
and Vulkan through MoltenVK can't express, is use the Apple GPU's tile memory
and unified memory the way the Xbox 360 used EDRAM and its shared RAM. That is
where the emulation overhead is.

## What Apple GPUs offer that the emulator can use

1. **Unified memory, zero-copy guest RAM.** Guest physical memory is a
   page-aligned shared-memory mapping. `newBufferWithBytesNoCopy` can wrap it in
   an `MTLBuffer` directly, so the GPU reads and writes guest memory itself:
   no upload copies (`u` in the timing log), and resolves/memexport results are
   visible to the CPU without readback copies. (This one can be tried in the
   existing Vulkan path first: MoltenVK supports `VK_EXT_external_memory_host`,
   which the SDK enables but never uses.)
2. **Raster order groups.** Native on Apple silicon. This is the hardware
   feature behind Xenia's most accurate EDRAM path (D3D12 ROV / Vulkan FSI).
   MoltenVK's generic FSI mapping rendered black and froze in our test, but a
   Metal backend can use ROGs deliberately.
3. **Tile memory (imageblocks, tile shaders, programmable blending).** EDRAM
   is a small, fast on-chip render target memory; so is an Apple GPU tile.
   Keeping guest render targets in tile memory across draws, and resolving with
   tile shaders, could remove most transfer passes and pass splits. This is
   the research part.
4. **Load/store actions and memoryless targets,** so a split render pass doesn't
   have to write out and reload 4x MSAA attachments it doesn't need.

## How the SDK is structured

The emulator core is API-independent; each host API is a backend behind the
same interfaces, selected in `src/graphics/plugin_main.cpp`:

| Component | Shared base | Vulkan backend (lines) |
|---|---|---|
| Command processor (PM4 → draws, resolves, swaps) | `CommandProcessor` | `vulkan/command_processor.cpp` (7,800) |
| Pipelines and shader binding | — | `vulkan/pipeline_cache.cpp` (4,300) |
| EDRAM / render targets | `RenderTargetCache` | `vulkan/render_target_cache.cpp` (6,200) |
| Textures | `TextureCache` | `vulkan/texture_cache.cpp` (3,500) |
| Guest memory on the GPU | `SharedMemory` | `vulkan/shared_memory.cpp` (400) |
| Index/primitive conversion | `PrimitiveProcessor` | `vulkan/primitive_processor.cpp` (230) |
| Xenos shader → host shader | `ShaderTranslator` | SPIR-V translator (13,500, shared) |
| Window, presentation, UI | `Presenter`, `GraphicsProvider` | `ui/vulkan/*` (7,200) |
| Built-in shaders (resolve, texture load, gamma, transfers) | — | 97 precompiled SPIR-V modules |

The D3D12 backend (17,000 lines) is the same shape, which shows a new backend
fits without touching the emulator core.

## Approach

- **New backend `metal`** in `src/graphics/metal/` and `src/ui/metal/`, chosen
  with `--gpu_backend=metal` (or the plugin's backend string), macOS only. The
  Vulkan backend stays as the fallback and as the reference to compare against.
- **C++ bindings:** Apple's `metal-cpp` (header-only, Apache 2.0), so the
  backend stays C++ like the rest of the SDK.
- **Shaders: reuse the SPIR-V translator, convert in-process with SPIRV-Cross**
  (already in the tree under MoltenVK) to MSL, then compile with
  `newLibraryWithSource`. A direct Xenos → MSL translator would be another
  10,000+ lines; the SPIR-V route reuses years of Xenia's shader correctness
  work and gives full control over resource binding and MSL options, unlike
  MoltenVK. The 97 built-in shaders are converted the same way, once.
- **Render targets, first pass:** port the host render target (FBO) path as is,
  for parity with today. Then replace it with the Apple-specific designs above,
  measured one at a time.

## Phases

Each phase ends with something runnable on the Mac.

0. **Spike (go/no-go), about a week of work.**
   - Zero-copy shared memory in the *existing* Vulkan path via
     `VK_EXT_external_memory_host`; measure the upload and readback gaps.
   - SPIRV-Cross in-process: translate a captured Halo 3 shader set to MSL and
     compile it with Metal, to confirm the shader route works for the game's
     shaders.
   - A macOS CI build (GitHub Actions) so Metal code is compile-checked on
     every push; it can't be built in a Linux container.
1. **Presenter and UI:** `MetalProvider`, `MetalPresenter` on the SDL window's
   `CAMetalLayer`, immediate drawer for the overlay. The game runs with a black
   screen and working UI.
2. **Draws:** command processor, pipeline cache, shared memory, primitive
   processor, texture cache, host render target path. The menu renders.
3. **The rest of the GPU:** resolves, memexport, occlusion queries (Metal
   visibility buffers), gamma and swap, readback. Campaign parity with the
   Vulkan backend, checked with Metal frame captures.
4. **Apple-specific EDRAM:** raster order group path, then tile-memory render
   targets; measured with the GPU timing log against the Vulkan baseline.

Rough size: 15,000–25,000 lines for phases 1–3, mostly ports of the Vulkan
backend's structure. Phase 4 is open-ended.

## How we'd work

The Metal code can only be compiled and run on a Mac, so:

- CI on a macOS runner compiles every push (phase 0), so a broken build is
  caught before it reaches the Mac.
- Each runnable step is tested on the M3 with the existing tools: the GPU
  timing log, `--metal-capture` frame captures and the stutter log.
- The Vulkan backend stays the default until the Metal one matches it.

## Risks

- **Scale.** This is the largest piece of the port by far, and correctness
  bugs like the ones already found (exposure readback, foliage dots) will
  recur in new forms.
- **SDK upstream.** The backend lives in our SDK patch (`macos.diff`), which
  grows a lot; rebasing onto newer ReXGlue releases gets harder.
- **The payoff is in phase 4.** Phases 1–3 alone reproduce today's design on
  Metal; most of the speedup comes from the Apple-specific EDRAM work after.
  Zero-copy memory (phase 0) is the exception and may pay off immediately.
