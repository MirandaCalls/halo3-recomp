// Headless frame capture for autoport test runs.
//
// When AUTOPORT_CAPTURE_DIR is set, a background thread grabs the guest output
// image every AUTOPORT_CAPTURE_INTERVAL seconds (default 5) and writes it as
// frame_NNN.ppm. Works with SDL's offscreen video driver, so runs can be
// checked without a window.

#pragma once

#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace autoport {

inline void StartFrameCapture(rex::Runtime* runtime) {
  const char* dir = std::getenv("AUTOPORT_CAPTURE_DIR");
  if (!dir || !runtime) return;
  const char* interval_env = std::getenv("AUTOPORT_CAPTURE_INTERVAL");
  const int interval = interval_env ? std::atoi(interval_env) : 5;

  std::thread([runtime, out = std::string(dir), interval] {
    for (int n = 0;; n++) {
      std::this_thread::sleep_for(std::chrono::seconds(interval));
      auto* gfx = runtime->graphics_system();
      auto* presenter = gfx ? gfx->presenter() : nullptr;
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) continue;

      char path[512];
      std::snprintf(path, sizeof(path), "%s/frame_%03d.ppm", out.c_str(), n);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
        for (uint32_t y = 0; y < image.height; y++) {
          const uint8_t* row = image.data.data() + y * image.stride;
          for (uint32_t x = 0; x < image.width; x++) std::fwrite(row + x * 4, 1, 3, f);
        }
        std::fclose(f);
      }
    }
  }).detach();
}

}  // namespace autoport
