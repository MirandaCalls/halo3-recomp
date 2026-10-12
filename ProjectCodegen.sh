#!/bin/sh
# macOS/Linux counterpart of ProjectCodegen.bat
# rexglue codegen halo3_cache_release_manifest.toml --log-level=debug --log-file=halo3_cache_release_codegen.log
cd "$(dirname "$0")" && rexglue codegen halo3_cache_release_manifest.toml
