// autoport: backing for the PowerPC delay hints (db16cyc) in recompiled code.
//
// XDK busy-wait loops (e.g. D3D waiting for GPU ring-buffer space) issue
// db16cyc to burn ~16 cycles per hint. The recompiler emits nothing for it, so
// those loops spin flat out and take a whole host core away from the emulated
// GPU thread they are waiting on. patch_spin_hints.py inserts a call to
// autoport_spin_hint() after every db16cyc: each call is a pause instruction,
// and every kHintsPerSleep calls (one pass of a typical XDK wait loop) the
// thread sleeps briefly. AUTOPORT_SPIN_SLEEP_US overrides the sleep (0 = only
// yield).
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define AUTOPORT_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define AUTOPORT_PAUSE() __asm__ __volatile__("yield")
#else
#define AUTOPORT_PAUSE()
#endif

namespace {
constexpr uint32_t kHintsPerSleep = 32;

int SleepMicroseconds() {
  static const int us = [] {
    const char* v = std::getenv("AUTOPORT_SPIN_SLEEP_US");
    return v ? std::atoi(v) : 20;
  }();
  return us;
}
}  // namespace

extern "C" void autoport_spin_hint() {
  thread_local uint32_t hints = 0;
  AUTOPORT_PAUSE();
  if (++hints % kHintsPerSleep == 0) {
    int us = SleepMicroseconds();
    if (us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(us));
    } else {
      std::this_thread::yield();
    }
  }
}
