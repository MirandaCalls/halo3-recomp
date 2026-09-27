// Scripted virtual controller for autoport test runs.
//
// When AUTOPORT_INPUT_FILE=<path> is set, a synthetic gamepad is added to the
// input system (merged into guest user 0 alongside any real pad) and a thread
// follows <path> like `tail -f`, executing one command per line:
//
//   press A B [ms]       tap buttons (default 150 ms)
//   hold A / release A   hold / release buttons ("release all" clears everything)
//   lstick x y [ms]      left stick, x/y in -1..1; with ms, returns to centre after
//   rstick x y [ms]      right stick
//   lt v [ms] / rt v [ms]  triggers, v in 0..1
//   wait ms
//   shot name            write the current guest frame to <path dir>/name.ppm
//
// Buttons: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT GUIDE.
// Every executed command is appended to <path>.log ("<seconds> done <line>"), so
// a driver can wait for completion before looking at a shot.

#pragma once

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace autoport {

using rex::X_RESULT;
using rex::X_STATUS;

class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  static constexpr rex::input::DeviceId kDevice = static_cast<rex::input::DeviceId>(0x41505400);

  ScriptedInputDriver() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kDevice;
    info.name = "autoport scripted pad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lock(mutex_);
    out->packet_number = packet_;
    out->gamepad.buttons = buttons_;
    out->gamepad.left_trigger = lt_;
    out->gamepad.right_trigger = rt_;
    out->gamepad.thumb_lx = lx_;
    out->gamepad.thumb_ly = ly_;
    out->gamepad.thumb_rx = rx_;
    out->gamepad.thumb_ry = ry_;
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* caps) override {
    if (id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (caps) {
      std::memset(caps, 0, sizeof(*caps));
      caps->type = 0x01;
      caps->sub_type = 0x01;
      caps->gamepad.buttons = 0xFFFF;
      caps->gamepad.left_trigger = 0xFF;
      caps->gamepad.right_trigger = 0xFF;
      caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Command interface, called from the script thread.
  void SetButtons(uint16_t mask, bool down) {
    Update([&] { buttons_ = down ? (buttons_ | mask) : (buttons_ & ~mask); });
  }
  void ReleaseAll() {
    Update([&] { buttons_ = 0; lt_ = rt_ = 0; lx_ = ly_ = rx_ = ry_ = 0; });
  }
  void SetStick(bool left, float x, float y) {
    Update([&] {
      (left ? lx_ : rx_) = Axis(x);
      (left ? ly_ : ry_) = Axis(y);
    });
  }
  void SetTrigger(bool left, float v) {
    Update([&] { (left ? lt_ : rt_) = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); });
  }

 private:
  static int16_t Axis(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }
  template <typename F>
  void Update(F&& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    f();
    ++packet_;
  }

  std::mutex mutex_;
  uint32_t packet_ = 1;
  uint16_t buttons_ = 0;
  uint8_t lt_ = 0, rt_ = 0;
  int16_t lx_ = 0, ly_ = 0, rx_ = 0, ry_ = 0;
};

inline bool CaptureGuestFramePpm(rex::Runtime* runtime, const std::string& path) {
  auto* gfx = runtime->graphics_system();
  auto* presenter = gfx ? gfx->presenter() : nullptr;
  rex::ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return false;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
  for (uint32_t y = 0; y < image.height; y++) {
    const uint8_t* row = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; x++) std::fwrite(row + x * 4, 1, 3, f);
  }
  std::fclose(f);
  return true;
}

inline void StartScriptedInput(rex::Runtime* runtime) {
  const char* path_env = std::getenv("AUTOPORT_INPUT_FILE");
  if (!path_env || !runtime) return;
  auto* input = dynamic_cast<rex::input::InputSystem*>(runtime->input_system());
  if (!input) return;
  auto owned = std::make_unique<ScriptedInputDriver>();
  ScriptedInputDriver* pad = owned.get();
  input->AddDriver(std::move(owned));

  std::string path = path_env;
  { std::ofstream(path, std::ios::trunc); }
  { std::ofstream(path + ".log", std::ios::trunc); }

  std::thread([runtime, pad, path] {
    static const std::unordered_map<std::string, uint16_t> kButtons = {
        {"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008},
        {"START", 0x0010}, {"BACK", 0x0020}, {"LS", 0x0040},   {"RS", 0x0080},
        {"LB", 0x0100},    {"RB", 0x0200},   {"GUIDE", 0x0400}, {"A", 0x1000},
        {"B", 0x2000},     {"X", 0x4000},    {"Y", 0x8000}};
    const auto start = std::chrono::steady_clock::now();
    const std::string dir = std::filesystem::path(path).parent_path().string();
    auto sleep_ms = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    std::ifstream in(path);
    std::string line;
    for (;;) {
      if (!std::getline(in, line)) {
        in.clear();
        sleep_ms(20);
        continue;
      }
      std::istringstream ss(line);
      std::string cmd;
      if (!(ss >> cmd) || cmd[0] == '#') continue;
      std::vector<std::string> args;
      for (std::string a; ss >> a;) args.push_back(a);
      auto trailing_ms = [&](int def) {
        if (!args.empty() && std::isdigit(static_cast<unsigned char>(args.back()[0]))) {
          int v = std::atoi(args.back().c_str());
          args.pop_back();
          return v;
        }
        return def;
      };
      auto mask_of = [&] {
        uint16_t m = 0;
        for (auto& a : args) {
          auto it = kButtons.find(a);
          if (it != kButtons.end()) m |= it->second;
        }
        return m;
      };
      std::string status = "done";
      if (cmd == "press") {
        int ms = trailing_ms(150);
        uint16_t m = mask_of();
        pad->SetButtons(m, true);
        sleep_ms(ms);
        pad->SetButtons(m, false);
      } else if (cmd == "hold") {
        pad->SetButtons(mask_of(), true);
      } else if (cmd == "release") {
        if (!args.empty() && args[0] == "all") {
          pad->ReleaseAll();
        } else {
          pad->SetButtons(mask_of(), false);
        }
      } else if (cmd == "lstick" || cmd == "rstick") {
        bool left = cmd == "lstick";
        int ms = args.size() >= 3 ? trailing_ms(0) : 0;
        float x = args.size() > 0 ? std::strtof(args[0].c_str(), nullptr) : 0.0f;
        float y = args.size() > 1 ? std::strtof(args[1].c_str(), nullptr) : 0.0f;
        pad->SetStick(left, x, y);
        if (ms > 0) {
          sleep_ms(ms);
          pad->SetStick(left, 0.0f, 0.0f);
        }
      } else if (cmd == "lt" || cmd == "rt") {
        bool left = cmd == "lt";
        int ms = args.size() >= 2 ? trailing_ms(0) : 0;
        float v = args.empty() ? 1.0f : std::strtof(args[0].c_str(), nullptr);
        pad->SetTrigger(left, v);
        if (ms > 0) {
          sleep_ms(ms);
          pad->SetTrigger(left, 0.0f);
        }
      } else if (cmd == "wait") {
        sleep_ms(trailing_ms(0));
      } else if (cmd == "shot") {
        std::string name = args.empty() ? "shot" : args[0];
        if (!CaptureGuestFramePpm(runtime, dir + "/" + name + ".ppm")) status = "failed";
      } else {
        status = "unknown";
      }
      double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::ofstream(path + ".log", std::ios::app) << t << " " << status << " " << line << "\n";
    }
  }).detach();
}

}  // namespace autoport
