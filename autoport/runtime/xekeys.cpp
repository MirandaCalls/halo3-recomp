// XeKeysConsolePrivateKeySign that reports success.
//
// The SDK stub returns 0 with a "// Success" comment, but the export is a BOOL:
// 0 tells the title that signing failed. Halo: Reach signs its utility-cache
// and preference files this way; on failure it keeps retrying and never
// finishes streaming content, leaving the 3D scene black. This mirrors
// Xenia: fill a plausible retail XE_CONSOLE_CERTIFICATE and return TRUE.

#include <rex/hook.h>

#include <cstdint>
#include <cstring>

// BOOL XeKeysConsolePrivateKeySign(const u8* hash, XE_CONSOLE_CERTIFICATE* out)
REX_HOOK_RAW(__imp__XeKeysConsolePrivateKeySign) {
  const uint32_t hash = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  if (!hash || !out) {
    ctx.r3.u64 = 0;
    return;
  }
  uint8_t* cert = base + out;
  std::memset(cert, 0, 0x1A8);  // sizeof(XE_CONSOLE_CERTIFICATE)
  // console_id (0x2, 5 bytes): same packed bitfield bytes Xenia produces.
  const uint8_t console_id[5] = {0x93, 0x01, 0x64, 0xE6, 0x07};
  std::memcpy(cert + 0x2, console_id, sizeof(console_id));
  // console_type (0x18, be32) = Retail
  cert[0x18] = 0;
  cert[0x19] = 0;
  cert[0x1A] = 0;
  cert[0x1B] = 2;
  // manufacture_date (0x1C, 8 bytes)
  const uint8_t date[8] = {2, 0, 0, 5, 1, 1, 2, 2};
  std::memcpy(cert + 0x1C, date, sizeof(date));
  ctx.r3.u64 = 1;
}
