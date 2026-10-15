# Halo 3 native renderer: research notes

Status: paused. Halo 3 draws through pre-recorded PM4 command buffers (see
"The command stream"), so a D3D-hook renderer would still have to emulate
the command processor.

Goal: replace Halo 3's Xbox D3D layer with a renderer that talks to Metal
directly (the UnleashedRecomp approach), instead of emulating the Xenos GPU.
These notes record what is known so far. Addresses are for the retail game
(XDK 2.0.6132).

## Viability

`autoport/gpu/triage.py`: D3D library `D3D9 2.0.6132.0`, **not** `D3D9LTCG`, so
D3D functions are real functions that can be hooked.

## How Halo 3's D3D works

The XDK D3D device is a **register shadow with dirty bits**. Most API calls only
update a copy of GPU registers inside the device object and mark them dirty; the
GPU commands (PM4 packets) are written into a ring buffer later, when state is
flushed for a draw. Device fields seen so far (offsets from the device pointer,
passed in `r3`):

| Offset | Use |
|---|---|
| 16 | 64-bit dirty mask (bits set by the state setters) |
| 48 | ring buffer write cursor (PM4 packets are written with `stwu ...,4(r3)`) |
| 56 | ring buffer limit; past it, `82147618` makes space (likely kicks off the GPU) |
| 10112+ | bitfield array (shader boolean constants, see `825A31F0`) |
| 10420, 10564, 10568 | shadowed GPU register values |
| 10941, 10888, 10896, 10996 | fence / GPU-busy bookkeeping (`825A7EC8`) |
| 12708 | flags OR-ed into packet headers (predication) |

The library is linked in **two blocks**. Triage finds the one at
`825A0000–825C0000`; a second block around `82140000–821A0000` holds the ring
buffer writer (`82147618`, called from the first block), fixed-function draws
that load shaders inline (`IM_LOAD_IMMEDIATE`) and other packet writers. The 20
"engine functions emitting PM4 inline" that triage lists (`82145CA0`–`82193660`)
are this second block, not Bungie's engine.

## Identified functions

From the runtime trace (`tools/d3d-trace.sh`, ~1,461 frames, boot to Sierra 117)
and the recompiled code (`tools/guest-funcs.py`):

| Function | Calls/frame | What it does | Likely API |
|---|---|---|---|
| `825A7EC8` | ~1,150,000 | Checks fence/GPU-busy state with spin hints (`db16cyc`); called in a tight loop from `82145F68` | Busy-wait on a fence (`IsBusy` / `BlockOnFence` style). A native renderer replaces the loop with a real wait. |
| `825A5420` | ~350 | Sets the low 6 bits of the register at +10564, a flag at +10420 when non-zero; dirty bits `0x80` and `1<<44` | A render-state setter |
| `825A1010` | ~170 | Vector memory copy (`dcbzl`, `lvx`/`stvx`) | `XMemCpy` (not graphics) |
| `825A4E88` | ~170 | Sets the low 3 bits of the register at +10568; dirty bit `0x40` | A render-state setter (41 call sites) |
| `825A31F0` | ~53 | Sets one bit per element of a bool array into the bitfield at +10112; dirty bit `1<<56` | `Set{Vertex,Pixel}ShaderConstantB` |
| `825A2968` | ~36 | Writes a `LOAD_ALU_CONSTANT` packet (address, offset, count) | Load shader float constants from memory |
| `825A8160` | ~26 | Writes `EVENT_WRITE` (event 15), optionally a register write | Cache flush / invalidate around resolves |
| `825A4B28` | ~22 | Tail call to `82160D98` | Wrapper |
| `825A7860`, `825A79B8` | ~2.5 | Draws with inline-loaded shaders | Internal fixed-function draws (clears) |
| `8214C3F8` | — | `INDIRECT_BUFFER`, two `IM_LOAD_IMMEDIATE` (25-dword shaders), register writes | Internal fixed-function pass (clear/resolve setup) |
| `82148158` | — | `INDIRECT_BUFFER`, `EVENT_WRITE_SHD` | Command buffer call + fence |
| `8214FA18` | — | `LOAD_ALU_CONSTANT` via the ring | Shader constant load |
| `825BEC98` | ~33 | Called from 77 places game-wide | Probably a general helper (to check) |

## Bungie's rasterizer uses D3D inline (second trace)

The second trace covered `82140000–821A0000`, ~1,373 frames of Sierra 117. The
code of the busiest functions shows that most of that range is **Bungie's own
rasterizer**, not the D3D library, and that it **manipulates the D3D device
directly**, the way the XDK's inline header functions do:

- The device pointer is a global at `828C4694` (`lis r10,-32116` +
  `lwz r3,18068(r10)` throughout).
- `82167BE0(sampler, a, b, c)` (46/frame) writes filter bits straight into the
  device's fetch-constant shadow (bits 13–21 of a texture fetch constant) and
  sets a dirty bit in the 64-bit mask at device+24: an inlined
  `SetSamplerState` for samplers 0–25.
- `8214E7F0(start, count, data)` (84/frame) computes a dirty mask for a range of
  float constants and calls `8214D9D0` to copy them into the device: shader
  float constants (`SetVertexShaderConstantF`-like).
- `8214CEC0` (30/frame) does the same for another constant bank through
  `8214CF40`.
- `82170F30` computes viewport/projection values into the device's constant
  shadow (device+2112…2156) with a dirty bit: Bungie's own constant setup.
- `82170020` only resets Bungie's cached state globals; `82469E98` is a
  per-object setup routine (calls `821713F0`, `825A5420`, `82170020`), not a
  draw.

### What this means for the design

Hooking only the D3D library's real functions is not enough: much of the state
reaches the device through inlined writes into its register shadow, from
hundreds of places. The workable design is a **register-shadow renderer**:

- Keep the guest D3D device object and its register shadow exactly as the game
  writes them (inline or not).
- Hook the few points where D3D turns the shadow into GPU work: the dirty-state
  flush and draw packet writer, render target set / resolve, and present.
- At each of those, read the shadow (Xenos register values, fetch constants,
  shader constants) and issue the equivalent Metal work directly.

Compared with full emulation this still interprets Xenos register meanings
(reusing the SDK's register definitions, shader translator and texture
formats), but skips the PM4 ring buffer, the command processor and, with
resolves and render targets handled at the API level, the EDRAM emulation
that costs the most on Apple GPUs.

## The command stream (PM4 census)

`AUTOPORT_PM4_STATS` (the emulator's PM4 census), ~2,700 frames from the menu
into Sierra 117, per-frame averages over 300-frame windows:

| Command | Per frame |
|---|---|
| `DRAW_INDX` (indexed draws) | 310–2,280 |
| `DRAW_INDX_2` (inline-index draws) | 180–340 |
| `IM_LOAD` (shader loads) | 400–3,600 |
| `LOAD_ALU_CONSTANT` (constants from memory) | 440–4,150 |
| Register writes, of which ALU constants | 42,000–284,000, 27,000–173,000 |
| `INDIRECT_BUFFER` jumps | 165–1,600 buffers, 64,000–450,000 dwords (up to 1.8 MB) |
| `VIZ_QUERY`, `SET_BIN_MASK_*` | up to ~260 and ~1,100 (predicated tiling, visibility) |

Every draw is in an **indirect (called) command buffer**; the ring itself only
holds jumps and a few conditional writes. Of 2,500–10,500 distinct command
buffer addresses per 300 frames, 500–1,900 are used more than once: Halo 3
replays **pre-recorded command buffers** (per mesh or per material) alongside
per-frame ones. No engine or D3D function builds a `DRAW_INDX` header in code
(`tools/guest-grep.py` finds only `DRAW_INDX_2` writers, all at init or for
fixed-function passes), which fits draws being recorded once and replayed.

### What this means

Halo 3 doesn't drive the GPU through D3D calls at draw granularity. Its draws
and much of their state reach the GPU as **raw PM4 command buffers**, many of
them recorded ahead of time. A renderer that hooks D3D, or the D3D register
shadow, would still have to execute those buffers, which means interpreting
PM4 and Xenos register state just as the emulator's command processor does.

So the "native renderer" route does not remove the expensive part for this
game. What remains specific to it, handling render targets and resolves at
the API level instead of emulating EDRAM, is equally available to an
emulator backend that targets Apple GPUs directly (`docs/metal-renderer.md`),
which needs no game-specific reverse engineering. The research here is paused
in favour of that and the cheaper emulator-side improvements.

## Open questions

- **Where are the draws?** Still not found. Draw packet headers carry the
  index count, so they're built at run time and the static PM4 scan misses
  them. Next: search the generated code for `DRAW_INDX` (`0x22`) and
  `DRAW_INDX_2` (`0x36`) opcode constants with `tools/guest-grep.py`.
- **Exact extent of the second block,** and whether Bungie's own rasterizer code
  is mixed into it.
- Which render states the setters at +10564/+10568 control (from the register
  offsets: device register shadow base still to be pinned down).

## Next steps

1. Trace with both blocks covered; identify the draw call, the state flush,
   shader set, texture/sampler set, render target set, resolve, present.
2. Name the device fields and the register shadow layout.
3. First hook milestone: intercept present and draw a Metal frame alongside the
   emulator (proves the hooking and the Metal presenter end to end).
