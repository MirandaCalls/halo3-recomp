// xenon-gpu: debug switches shared by the native modules.
#pragma once

namespace xgpu {

// Per-draw / per-texture tracing to stdout (XGPU_TRACE, or one frame chosen by
// the plugin with XGPU_TRACE_FRAME).
extern bool g_trace;

}  // namespace xgpu
