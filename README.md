# reclaimed-ring

A native macOS port of Halo 3 for Apple Silicon Macs. It recompiles your own
Halo 3 (Xbox 360) disc image into a native Mac app and plays it, including
working **system link (LAN) multiplayer**, even between machines that only
share a **ZeroTier** network.

reclaimed-ring started as a fork of
[summers-aaron/xenon-pc-comp](https://github.com/summers-aaron/xenon-pc-comp),
a Linux port, and is now macOS-first: it adds the macOS port (Vulkan through
MoltenVK) and the rendering fixes and performance work it needed.

Built on [ReXGlue](https://github.com/rexglue/rexglue-sdk), the open-source
Xbox 360 static-recompilation toolchain.

**Status:** the campaign renders correctly and system link works. On an M3
MacBook Air it runs at about 29 fps on average, mostly 29–30 in combat (the
game caps at 30). See [known issues](docs/technical-notes.md#known-issues-and-next-steps)
for what's left.

## Requirements

- A Mac with Apple Silicon (tested on an M3 MacBook Air; Intel Macs are
  untested)
- Xcode, the full app rather than just the command-line tools (the build
  compiles MoltenVK from source)
- Homebrew: `brew install bash cmake ninja python` (the scripts need bash 4.4+,
  which macOS lacks, and python 3.11+; setup installs `capstone` into a private
  venv under `tools/venv`)
- ~40 GB free disk
- Your own Halo 3 disc image (ISO); it stays on your machine
- For internet play: both Macs on the same ZeroTier network

## Build

```bash
git clone https://github.com/MirandaCalls/reclaimed-ring-macos.git
cd reclaimed-ring-macos
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

Useful launcher flags: `--fullscreen` (the cursor auto-hides), `--keyboard`
(keyboard and mouse), `--vsync` (no tearing, but costs frame rate; see the
known issues), `--instance N` (several instances on one Mac), `--ip ADDR`
(pick the system-link interface), `--scale N`, `--help`.

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

## Notes

- Guest ports below 1024 are mapped up by `net_port_offset` (default 36000).
  Every machine in a system link must use the same offset.
- `--instance 1..16` gives each local instance its own `127.0.0.N`, saves and
  cache for system link on a single Mac.
- Kernel/GPU details and the session history are in `autoport/` comments.

## More

[docs/technical-notes.md](docs/technical-notes.md) covers how the port works:
what's in this repository, the macOS-specific changes and the switches for
diagnosing rendering problems, measuring GPU time, the known issues and next
steps, and the Linux build the project started from.

## Legal

No game code or data is included. `setup-halo3.sh` reads your own disc image;
the resulting binary is for your own use. ReXGlue and its dependencies are
BSD/MIT-licensed; `tools/extract-xiso` ships under its own license. Not
affiliated with Microsoft, Bungie or 343 Industries.
