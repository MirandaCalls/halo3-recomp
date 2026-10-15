# Shader tools

Tools for working with Halo 3's Xenos shaders outside the game, kept from
upstream's experimental `xenon-gpu` renderer. They're the starting point for
pre-compiling the shader cache (see "Known issues and next steps" in
`docs/technical-notes.md`).

| File | What |
| --- | --- |
| `extract_shaders.py` | Finds compiled shader containers in game files (e.g. `Halo 3/maps/*.map`) and indexes them by hash. Note: on Halo 3 its hashes don't yet match the runtime's (it reads the code at the wrong spot), and most runtime shaders are patched versions of the disc ones. |
| `shader_scan.cpp`, `cf_parse.cpp`, `ucode.h` | Finds every plausible Xenos shader program in arbitrary files and writes a shader storage file (`.xsh`). Build: `clang++ -std=c++20 -O2 -I <sdk>/thirdparty/xxHash shader_scan.cpp cf_parse.cpp -o xgpu-shader-scan` |
| `xsh.py` | Reads and writes shader storage files (`.xsh`). |
| `xenos_cf.py` | Decodes Xenos shader control flow, for inspecting shaders. |
| `shader_aot.cpp` | Translates a `.xsh` corpus to SPIR-V with the SDK's translator. Needs the SDK's shader translator sources and headers to build. |

The game's own shader storage is in
`~/.local/share/halo3/cache/shaders/shareable/` (`4D5307E6.xsh` for shaders,
`4D5307E6.fbo.vk.xpso` for pipeline descriptions).
