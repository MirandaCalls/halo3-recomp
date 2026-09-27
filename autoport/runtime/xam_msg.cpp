// XMsgStartIORequest fixes for XAM app messages the SDK answers incorrectly.
//
// 0xFE / 0x00020021 is XContentQueryVolumeDeviceType. The SDK writes device
// type 0 and returns success. Xenia (matching the console for titles that are
// not XContent packages, i.e. disc titles, which is how extracted ISOs run)
// returns X_E_INVALIDARG and leaves the output untouched. Halo: Reach uses the
// answer to decide whether it is running from disc; on "success" it skips
// setting up the utility-partition map cache and never streams in textures.
//
// Everything else is forwarded to the SDK implementation.

#include <rex/hook.h>

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
using ImportFn = void (*)(PPCContext&, uint8_t*);
constexpr uint32_t kXamAppId = 0xFE;
constexpr uint32_t kMsgQueryVolumeDeviceType = 0x00020021;
constexpr uint32_t kInvalidArg = 0x80070057;  // X_E_INVALIDARG
}  // namespace

// u32 XMsgStartIORequest(u32 app, u32 message, XOVERLAPPED* overlapped, void* buffer, u32 length)
REX_HOOK_RAW(__imp__XMsgStartIORequest) {
  if (std::getenv("AUTOPORT_TRACE_XMSG")) {
    static FILE* f = std::fopen(std::getenv("AUTOPORT_TRACE_XMSG"), "a");
    std::fprintf(f, "XMsgStartIORequest(%08X, %08X, %08X, %08X, %08X)\n", ctx.r3.u32,
                 ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    std::fflush(f);
  }
  if (ctx.r3.u32 == kXamAppId && ctx.r4.u32 == kMsgQueryVolumeDeviceType) {
    ctx.r3.u64 = kInvalidArg;
    return;
  }
  static auto real = reinterpret_cast<ImportFn>(dlsym(RTLD_NEXT, "__imp__XMsgStartIORequest"));
  real(ctx, base);
}
