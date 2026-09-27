// xenon-gpu: runtime GLSL -> SPIR-V compilation (glslang) for built-in shaders.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xgpu {

enum class GlslStage { kVertex, kFragment, kCompute };

bool CompileGlsl(GlslStage stage, std::string_view source, std::vector<uint32_t>& spirv,
                 std::string* error);

}  // namespace xgpu
