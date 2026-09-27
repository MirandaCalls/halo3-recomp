// Serve game:/d: from the original disc image instead of an extracted folder.
//
// The SDK mounts game data as a host directory, so the volume reports a
// host filesystem name. Titles detect "running from disc" by checking for the
// GDFX filesystem on d:\ (Halo: Reach does); on anything else they take the
// installed-to-HDD path, which skips utility-cache setup and content streaming.
// Mounting the ISO with the SDK's DiscImageDevice reports GDFX, as on console
// and in Xenia. Enabled by AUTOPORT_DISC_IMAGE=<path to .iso>.

#pragma once

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>

#include <cstdlib>
#include <filesystem>
#include <memory>

namespace autoport {

inline bool MountDiscImage(rex::Runtime* runtime) {
  const char* iso = std::getenv("AUTOPORT_DISC_IMAGE");
  if (!iso || !runtime || !runtime->file_system()) return false;
  constexpr const char* kMount = "\\Device\\Cdrom0";
  auto device = std::make_unique<rex::filesystem::DiscImageDevice>(kMount, std::filesystem::path(iso));
  if (!device->Initialize()) return false;
  auto* vfs = runtime->file_system();
  vfs->RegisterDevice(std::move(device));
  for (const char* link : {"game:", "d:"}) {
    vfs->UnregisterSymbolicLink(link);
    vfs->RegisterSymbolicLink(link, kMount);
  }
  return true;
}

}  // namespace autoport
