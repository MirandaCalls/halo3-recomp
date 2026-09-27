// xgpu-shader-aot: translate a Xenos shader corpus to SPIR-V ahead of time.
//
// Input: a Xenia/ReXGlue shader storage file (.xsh), e.g. from
// xgpu-shader-scan or recorded at runtime. Every program is analyzed and
// translated with the SPIR-V translator (default vertex/pixel modification,
// the same one the runtime uses for the common case). Programs that fail
// analysis or translation, or crash the translator (isolated in a worker
// process), are counted, not written; for a scanned corpus that filters out
// false hits.
//
// Output: a pack file of translated shaders:
//   header: 'XGSP' u32, version u32, count u32
//   entry:  hash u64, modification u64, stage u32, size u32, SPIR-V bytes
//
// Usage: xgpu-shader-aot --in corpus.xsh --out corpus.spvpack [--limit N]
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/xenos.h>
#include <rex/string/buffer.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <exception>
#include <string>
#include <vector>

using namespace rex::graphics;

namespace {

struct Stored {
  uint64_t hash;
  bool pixel;
  std::vector<uint32_t> ucode;  // guest (big-endian) dwords as stored
};

bool ReadXsh(const char* path, std::vector<Stored>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint32_t header[2];
  if (std::fread(header, sizeof(header), 1, f) != 1 || header[0] != 0x48534558u) {
    std::fclose(f);
    return false;
  }
  for (;;) {
    uint64_t hash;
    uint32_t bits;
    if (std::fread(&hash, 8, 1, f) != 1 || std::fread(&bits, 4, 1, f) != 1) break;
    Stored s{hash, (bits >> 31) != 0, std::vector<uint32_t>(bits & 0x7FFFFFFF)};
    if (!s.ucode.empty() && std::fread(s.ucode.data(), 4, s.ucode.size(), f) != s.ucode.size()) break;
    out.push_back(std::move(s));
  }
  std::fclose(f);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  size_t limit = SIZE_MAX;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--in") && i + 1 < argc) in_path = argv[++i];
    else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
    else if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
  }
  if (!in_path) {
    std::fprintf(stderr, "usage: %s --in corpus.xsh [--out corpus.spvpack] [--limit N]\n", argv[0]);
    return 1;
  }
  std::vector<Stored> corpus;
  if (!ReadXsh(in_path, corpus)) {
    std::fprintf(stderr, "%s: not a shader storage file\n", in_path);
    return 1;
  }

  FILE* out = out_path ? std::fopen(out_path, "wb") : nullptr;
  uint32_t written = 0;
  if (out) {
    const uint32_t header[3] = {0x50534758u /* 'XGSP' */, 1, 0};
    std::fwrite(header, sizeof(header), 1, out);
  }

  // The translator assumes well-formed microcode; scanned corpora can still
  // contain programs that crash it. Translation runs in a worker process that
  // streams one record per shader; if it dies, the shader after the last
  // record is logged as a crasher and a new worker resumes after it.
  const size_t total = std::min(corpus.size(), limit);
  size_t n_invalid_ucode = 0, n_failed = 0, n_ok = 0;
  std::vector<uint64_t> crashers;
  uint64_t spirv_bytes = 0;
  double seconds = 0.0, worst = 0.0;
  size_t next = 0;
  while (next < total) {
    int fds[2];
    if (pipe(fds)) {
      std::perror("pipe");
      return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
      close(fds[0]);
      FILE* w = fdopen(fds[1], "wb");
      // All features on, no EDRAM interlock: the common Vulkan configuration.
      SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true), true, true, false);
      rex::string::StringBuffer disasm;
      for (size_t i = next; i < total; ++i) {
        const Stored& s = corpus[i];
        auto t0 = std::chrono::steady_clock::now();
        uint32_t status = 0;  // 0 ok, 1 invalid ucode, 2 translation failed
        uint64_t modification = 0;
        std::vector<uint8_t> spirv;
        Shader shader(s.pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, s.hash,
                      s.ucode.data(), s.ucode.size());
        try {
          shader.AnalyzeUcode(disasm);
        } catch (const std::exception&) {
        }
        if (!shader.is_ucode_analyzed()) {
          status = 1;
        } else {
          modification =
              s.pixel ? translator.GetDefaultPixelShaderModification(xenos::kMaxShaderTempRegisters)
                      : translator.GetDefaultVertexShaderModification(xenos::kMaxShaderTempRegisters);
          Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
          bool ok = false;
          try {
            ok = translator.TranslateAnalyzedShader(*translation) && translation->is_valid();
          } catch (const std::exception&) {
          }
          if (ok) {
            spirv = translation->translated_binary();
          } else {
            status = 2;
          }
        }
        float dt = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        const uint32_t index = uint32_t(i), size = uint32_t(spirv.size());
        std::fwrite(&index, 4, 1, w);
        std::fwrite(&status, 4, 1, w);
        std::fwrite(&modification, 8, 1, w);
        std::fwrite(&dt, 4, 1, w);
        std::fwrite(&size, 4, 1, w);
        if (size) std::fwrite(spirv.data(), 1, size, w);
        std::fflush(w);
      }
      std::fclose(w);
      _exit(0);
    }
    close(fds[1]);
    FILE* r = fdopen(fds[0], "rb");
    size_t last = SIZE_MAX;
    std::vector<uint8_t> spirv;
    for (;;) {
      uint32_t index, status, size;
      uint64_t modification;
      float dt;
      if (std::fread(&index, 4, 1, r) != 1 || std::fread(&status, 4, 1, r) != 1 ||
          std::fread(&modification, 8, 1, r) != 1 || std::fread(&dt, 4, 1, r) != 1 ||
          std::fread(&size, 4, 1, r) != 1) {
        break;
      }
      spirv.resize(size);
      if (size && std::fread(spirv.data(), 1, size, r) != size) break;
      last = index;
      if (status == 1) {
        ++n_invalid_ucode;
        continue;
      }
      seconds += dt;
      worst = std::max(worst, double(dt));
      if (status == 2) {
        ++n_failed;
        continue;
      }
      ++n_ok;
      spirv_bytes += size;
      if (out) {
        const Stored& s = corpus[index];
        const uint32_t stage = s.pixel ? 1 : 0;
        std::fwrite(&s.hash, 8, 1, out);
        std::fwrite(&modification, 8, 1, out);
        std::fwrite(&stage, 4, 1, out);
        std::fwrite(&size, 4, 1, out);
        std::fwrite(spirv.data(), 1, size, out);
        ++written;
      }
    }
    std::fclose(r);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    const size_t resume = last == SIZE_MAX ? next : last + 1;
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
      next = total;
    } else {
      // The worker died on corpus[resume].
      crashers.push_back(corpus[resume].hash);
      next = resume + 1;
    }
  }
  if (out) {
    std::fseek(out, 8, SEEK_SET);
    std::fwrite(&written, 4, 1, out);
    std::fclose(out);
  }
  std::fprintf(stderr,
               "%zu programs: %zu translated, %zu invalid ucode, %zu translation failures, "
               "%zu crashed the translator\n"
               "SPIR-V %.1f MB; translate time %.1f s total, %.2f ms avg, %.1f ms worst\n",
               total, n_ok, n_invalid_ucode, n_failed, crashers.size(), spirv_bytes / 1048576.0,
               seconds, (n_ok + n_failed) ? seconds * 1000.0 / double(n_ok + n_failed) : 0.0,
               worst * 1000.0);
  for (uint64_t h : crashers) std::fprintf(stderr, "crasher %016llX\n", (unsigned long long)h);
  return 0;
}
